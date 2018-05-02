/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; If not, see <http://www.gnu.org/licenses/>
 */

#if defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__) && !defined(WIN32)
	#define WIN32
#endif
#include <srt/srt.h>
#undef LOG_INFO
#undef LOG_DEBUG
#undef min
#undef max

#include "SRTOut.h"
#include "Mona/String.h"
#include "Mona/AVC.h"
#include "Mona/SocketAddress.h"
#include "Mona/Thread.h"
#include "Mona/Util.h"

#include <sstream>

using namespace Mona;
using namespace std;

static const int64_t epollWaitTimoutMS = 250;
static const int64_t reconnectPeriodMS = 1000;

struct SRTOpenParams {
	enum modeT {
		MODE_NOTSET = 0,
		MODE_CALLER = 1,
		MODE_LISTENER = 2,
		MODE_RENDEZVOUS = 3
	};
	modeT _mode;
	Mona::SocketAddress _address;
	::std::string _password;
	enum keyLenT {
		KEY_LEN_NOTSET = 0,
		KEY_LEN_96     = 12,
		KEY_LEN_128    = 16,
		KEY_LEN_256    = 32
	};
	keyLenT _keylen;
	int _latency;

	SRTOpenParams() :
		_mode(MODE_NOTSET),
		_address(),
		_password(),
		_keylen(KEY_LEN_NOTSET),
		_latency(0)
	{
		// No-op
	}
	bool SetFromURL(const ::std::string& url, ::std::string& err);
};

bool SRTOpenParams::SetFromURL(const ::std::string& url, std::string& err)
{
	std::string address, path, query;
	Mona::Util::UnpackUrl(url, address, path, query);

	DEBUG("address: ", address, " path: ", path, " query: ", query);

	if (address.empty()) {
		err = "Failed to decode target host address from " + url;
		return false;
	}

	SRTOpenParams::modeT mode = SRTOpenParams::MODE_CALLER;
	const char wildcard[] = "0.0.0.0";
	if (address.front() == ':') {
		mode = SRTOpenParams::MODE_LISTENER;
		address =  wildcard + address;
	} else if (address.compare(0, sizeof(wildcard) - 1, wildcard)) {
		mode = SRTOpenParams::MODE_LISTENER;
	}

	Exception ex;
	Mona::SocketAddress addr;
	addr.setWithDNS(ex, address);
	if (ex || addr.family() != IPAddress::IPv4) {
		const std::string& exs(ex);
		err = "Failed to resolve target host address, " + exs;
		return false;
	}
	_mode = mode;
	_address = addr;

	if (query.empty())
		return true;

	Mona::Util::ForEachParameter forEach([this, &err](
			const string& key, const char* value) {
		DEBUG("Query params key: ", key, " value: ", value);
		if (!Mona::String::ICompare(key,"mode")) {
			if (!Mona::String::ICompare(value, "caller")) {
				_mode = SRTOpenParams::MODE_CALLER;
			} else if (!Mona::String::ICompare(value, "listener")) {
				_mode = SRTOpenParams::MODE_LISTENER;
			} else if (!Mona::String::ICompare(value, "rendezvous")) {
				_mode = SRTOpenParams::MODE_LISTENER;
			} else {
				err = "Invalid mode";
				return false;
			}
		} else if (!Mona::String::ICompare(key, "pass", 4)) {
			size_t len = (value) ? strlen(value) : 0;
			if (len >= 10 && len <= 79) {
				_password = value;
			} else {
				// abort on bad password to avoid sending in clear by accident
				err = "Passphrase must be between 10 and 79 characters long";
				return false;
			}
		} else if (!Mona::String::ICompare(key, "key", 3)) {
			int len = Mona::String::ToNumber<int, 0>(value);
			switch(len) {
				case 96:
					_keylen = SRTOpenParams::KEY_LEN_96;
					break;
				case 128:
					_keylen = SRTOpenParams::KEY_LEN_128;
					break;
				case 256:
					_keylen = SRTOpenParams::KEY_LEN_256;
					break;
				default:
					_keylen = SRTOpenParams::KEY_LEN_NOTSET;
					break;
			}
		} else if (!Mona::String::ICompare(key, "latency")) {
			_latency = Mona::String::ToNumber<int, 120>(value);
		}
		return true;
	});
	Mona::Util::UnpackQuery(query, forEach);
	if (!err.empty()) {
		ERROR(err);
		*this = SRTOpenParams();
		return false;
	}

	return true;
}

class SRTOut::Client::OpenSrtPIMPL : private Thread {
	private:
		::SRTSOCKET _socket;
		bool _started;
		SRTOpenParams _openParams;
		std::mutex _mutex;
		std::string _errorDetail;
	public:
		OpenSrtPIMPL() :
			_socket(::SRT_INVALID_SOCK), _started(false), Thread("SRTOut") {
		}
		~OpenSrtPIMPL() {
			Close();
		}

		bool Open(const string& url) {
			if (_started) {
				_errorDetail = "SRT Open: Already open, please close first";
				ERROR(_errorDetail)
				return false;
			}

			if (::srt_startup()) {
				_errorDetail = "SRT Open: Error starting SRT library";
				ERROR(_errorDetail)
				return false;
			}
	
			if (!_openParams.SetFromURL(url, _errorDetail)) {
				return false;
			}

			_started = true;

			::srt_setloghandler(nullptr, logCallback);
			::srt_setloglevel(0xff);
			
			Thread::start();

			INFO("SRT opened")

			return true;
		}

		void Close() {
			Disconnect();
			Thread::stop();

			if (_started) {
				::srt_setloghandler(nullptr, nullptr);
				::srt_cleanup();
				_started = false;
				_openParams = SRTOpenParams();
			}
		}

		bool Connect() {
			std::lock_guard<std::mutex> lock(_mutex);
			return ConnectActual();
		}

		bool ConnectActual() {
			_errorDetail.clear();

			if (_socket != ::SRT_INVALID_SOCK) {
				ERROR("Already connected, please disconnect first")
				return false;
			}

			_socket = ::srt_socket(AF_INET, SOCK_DGRAM, 0);
			if (_socket == ::SRT_INVALID_SOCK ) {
				_errorDetail.assign("SRT create socket: ")
					+ ::srt_getlasterror_str();
				ERROR(_errorDetail);
				return false;
			}

			bool block = false;
			int rc = ::srt_setsockopt(_socket, 0, SRTO_SNDSYN, &block, sizeof(block));
			if (rc != 0) {
				_errorDetail.assign("SRT SRTO_SNDSYN: ")
					+ ::srt_getlasterror_str();
				ERROR(_errorDetail);
				DisconnectActual();
				return false;
			}
			rc = ::srt_setsockopt(_socket, 0, SRTO_RCVSYN, &block, sizeof(block));
			if (rc != 0) {
				_errorDetail.assign("SRT SRTO_RCVSYN: ")
					+ ::srt_getlasterror_str();
				ERROR(_errorDetail);
				DisconnectActual();
				return false;
			}

			int opt = 1;
			::srt_setsockflag(_socket, ::SRTO_SENDER, &opt, sizeof opt);

			if (!_openParams._password.empty()) {
				rc = srt_setsockopt (_socket, 0, SRTO_PASSPHRASE,
					_openParams._password.c_str(),
					_openParams._password.size() + 1);
				if (rc != 0) {
					_errorDetail.assign("SRT SRTO_PASSPHRASE: ")
						+ ::srt_getlasterror_str();
					ERROR(_errorDetail);
					DisconnectActual();
					return false;
				}
				opt = (_openParams._keylen != SRTOpenParams::KEY_LEN_NOTSET) ?
					_openParams._keylen : SRTOpenParams::KEY_LEN_128;
				rc = srt_setsockopt (_socket, 0, SRTO_PBKEYLEN, &opt, sizeof opt);
				if (rc != 0) {
					_errorDetail.assign("SRT SRTO_PBKEYLEN: ")
						+ ::srt_getlasterror_str();
					ERROR(_errorDetail);
					DisconnectActual();
					return false;
				}
			}

			if (_openParams._latency > 0) {
				opt = _openParams._latency;
				rc = srt_setsockopt (_socket, 0, SRTO_LATENCY, &opt, sizeof opt);
				if (rc != 0) {
					_errorDetail.assign("SRT SRTO_LATENCY: ")
						+ ::srt_getlasterror_str();
					ERROR(_errorDetail);
					DisconnectActual();
					return false;
				}
			}

			::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
			if (state != SRTS_INIT) {
				ERROR("SRT Connect: socket is in bad state; ", state)
				DisconnectActual();
				return false;
			}

			INFO("Connecting to ", _openParams._address.host(),
				  " port ", _openParams._address.port())

			// SRT support only IPV4 so we convert to a sockaddr_in
			sockaddr soaddr;
			// WARNING: work only with ipv4 addresses
			memcpy(&soaddr, _openParams._address.data(), sizeof(sockaddr));
			soaddr.sa_family = AF_INET;
			if (::srt_connect(_socket, &soaddr, sizeof(sockaddr))) {
				_errorDetail.assign("SRT Connect: ")
					+ ::srt_getlasterror_str();
				ERROR(_errorDetail);
				DisconnectActual();
				return false;
			}

			INFO("SRT connect state; ", ::srt_getsockstate(_socket));

			return true;
		}

		bool Disconnect() {
			std::lock_guard<std::mutex> lock(_mutex);
			return DisconnectActual();
		}

		bool DisconnectActual() {
			if (_socket != ::SRT_INVALID_SOCK) {
				::srt_close(_socket);

				INFO("SRT disconnect state; ", ::srt_getsockstate(_socket));

				_socket = ::SRT_INVALID_SOCK;
			}

			return true;
		}

		int Write(shared_ptr<Buffer>& pBuffer)
		{
			std::lock_guard<std::mutex> lock(_mutex);

			UInt8* p = pBuffer->data();
			const size_t psize = pBuffer->size();

			if (_socket == ::SRT_INVALID_SOCK) {
				if ((false)) {
					DEBUG("SRT: Drop packet while NOT CONNECTED")
				}
				return psize;
			}

			SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
			switch(state) {
				case ::SRTS_CONNECTED: {
					// No-op
				}
				break;
				default: {
					if ((false)) {
						DEBUG("SRT: Drop packet on state ", state)
					}
					return psize;
				}
				break;
			}

			for (size_t i = 0; i < psize;) {
				size_t chunk = min<size_t>(psize - i, (size_t)1316);
				if (::srt_sendmsg(_socket,
						(const char*)(p + i), chunk, -1, true) < 0)
					WARN("SRT: send error; ", ::srt_getlasterror_str())
				i += chunk;
			}

			return psize;
		}

		bool run(Exception&, const volatile bool& requestStop) {

			_mutex.lock();

			int epollid = ::srt_epoll_create();
			if (epollid < 0) {
				ERROR("Error initializing UDT epoll set;",
					::srt_getlasterror_str());
			}

			while(!requestStop && epollid >= 0) {
				::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
				if (state == ::SRTS_BROKEN || state == ::SRTS_NONEXIST
						|| state == ::SRTS_CLOSED) {
					INFO("Reconnect socket");
					if (_socket != ::SRT_INVALID_SOCK) {
						DEBUG("Remove socket from poll; ", (int)_socket);
						::srt_epoll_remove_usock(epollid, _socket);
					}

					DisconnectActual();
					if (!ConnectActual()) {

						ERROR("Error issuing connect");

						// Wait a bit and try again
						_mutex.unlock();
						Sleep(reconnectPeriodMS);
						_mutex.lock();
						continue;
					}

					FATAL_CHECK(_socket != ::SRT_INVALID_SOCK)
					int modes = SRT_EPOLL_IN;
					if (::srt_epoll_add_usock(epollid, _socket, &modes) != 0) {
						ERROR("Error adding socket to poll set; ",
							::srt_getlasterror_str());
					}
				}

				_mutex.unlock();
				const int socksToPoll = 10;
				int rfdn = socksToPoll;
				::SRTSOCKET rfds[socksToPoll];
				int rc = ::srt_epoll_wait(epollid, &rfds[0], &rfdn, nullptr, nullptr,
					epollWaitTimoutMS, nullptr, nullptr, nullptr, nullptr);

				if (rc <= 0) {
					// Let the system breath just in case
					Sleep(0);

					_mutex.lock();
					continue;
				}
				_mutex.lock();

				FATAL_CHECK(rfdn <= socksToPoll)

				for (int i = 0; i < rfdn; i++) {
					::SRTSOCKET socket = rfds[i];
					state = ::srt_getsockstate(socket);
					switch(state) {
						case ::SRTS_CONNECTED: {
							// Discard incoming data
							static char buf[1500];
							while (::srt_recvmsg(socket, &buf[0], sizeof(buf)) > 0)
								continue;
						}
						break;
						case ::SRTS_NONEXIST:
						case ::SRTS_BROKEN:
						case ::SRTS_CLOSING:
						case ::SRTS_CLOSED: {
							DEBUG("Remove socket from poll (on poll event); ", socket);
							::srt_epoll_remove_usock(epollid, socket);
						}
						break;
						default: {
							WARN("Unexpected event on ",  socket, "state ", state);
						}
						break;
					}
				}
			}

			// TODO: debug this
			if (epollid > 0)
				::srt_epoll_release(epollid);

			_mutex.unlock();
			return true;
		}

		const char* GetSRTStateString() {
			if (!_errorDetail.empty())
				return "InError";
			if (srt_getsockstate(_socket) == SRTS_CONNECTED) {
				// TODO: check encryption status?
				return "Connected";
			}

			return "Connecting";
		}

		void GetStatsJson(std::string& response) {
			std::ostringstream ostr;
	
			if (!_errorDetail.empty()) {
				ostr \
					<< "{\"State\":\"InError\"," \
					<< "\"ErrorDetail\"= \"" << _errorDetail \
					<< "\"}" << endl;
				response = ostr.str();
				return;
			}
			
			ostr << "{";
			SRT_TRACEBSTATS mon;
			memset(&mon, 0, sizeof(mon));
			if (!srt_bstats(_socket, &mon, false)) {
				ostr << "\"time\":" << mon.msTimeStamp << ",";
				ostr << "\"window\":{";
				ostr << "\"flow\":" << mon.pktFlowWindow << ",";
				ostr << "\"congestion\":" << mon.pktCongestionWindow << ",";
				ostr << "\"flight\":" << mon.pktFlightSize;
				ostr << "},";
				ostr << "\"link\":{";
				ostr << "\"rtt\":" << mon.msRTT << ",";
				ostr << "\"bandwidth\":" << mon.mbpsBandwidth << ",";
				ostr << "\"maxBandwidth\":" << mon.mbpsMaxBW;
				ostr << "},";
				ostr << "\"send\":{";
				ostr << "\"packets\":" << mon.pktSent << ",";
				ostr << "\"packetsLost\":" << mon.pktSndLoss << ",";
				ostr << "\"packetsDropped\":" << mon.pktSndDrop << ",";
				ostr << "\"packetsRetransmitted\":" << mon.pktRetrans << ",";
				ostr << "\"bytes\":" << mon.byteSent << ",";
				ostr << "\"bytesDropped\":" << mon.byteSndDrop << ",";
				ostr << "\"mbitRate\":" << mon.mbpsSendRate;
				ostr << "},";
				ostr << "\"recv\": {";
				ostr << "\"packets\":" << mon.pktRecv << ",";
				ostr << "\"packetsLost\":" << mon.pktRcvLoss << ",";
				ostr << "\"packetsDopped\":" << mon.pktRcvDrop << ",";
				ostr << "\"packetsRetransmitted\":" << mon.pktRcvRetrans << ",";
				ostr << "\"packetsBelated\":" << mon.pktRcvBelated << ",";
				ostr << "\"bytes\":" << mon.byteRecv << ",";
				ostr << "\"bytesLost\":" << mon.byteRcvLoss << ",";
				ostr << "\"bytesDropped\":" << mon.byteRcvDrop << ",";
				ostr << "\"mbitRate\":" << mon.mbpsRecvRate;
				ostr << "},";
			}
			ostr << "\"State\":\"" << GetSRTStateString() << "\"}" << endl;
			response = ostr.str();
		}
private:

		static void logCallback(void* opaque, int level, const char* file,
				int line, const char* area, const char* message)
		{
			INFO("L:", level, "|", file, "|", line, "|", area, "|", message)
		}
};

SRTOut::SRTOut(const Parameters& configs): App(configs), _client(nullptr)
{
}

SRTOut::~SRTOut() {
}

SRTOut::Client::Client(Mona::Client& client, const Mona::Parameters& configs)
: App::Client(client), _first(true), _pPublication(NULL),
	_videoCodecSent(false), _audioCodecSent(false),
	_srtPimpl(new SRTOut::Client::OpenSrtPIMPL()) {

	FATAL_CHECK(_srtPimpl.get() != nullptr);

	const std::string &url = configs.getString("outputUrl");
	_srtPimpl->Open(url);

	_onAudio = [this](UInt16 track, const Media::Audio::Tag& tag, const Packet& packet) {
		shared<Buffer> pBuffer;

		// AAC codecs to be sent in first
		if (!_audioCodecSent) {
			if (tag.codec == Media::Audio::CODEC_AAC && tag.isConfig) {

				INFO("AAC codec infos saved")
				_audioCodec.set(std::move(packet));
			}
			if (!_audioCodec)
				return;

			_audioCodecSent = true;
			INFO("AAC codec infos sent")
			Media::Audio::Tag configTag(tag);
			configTag.isConfig = true;
			configTag.time = tag.time;
			if (!tag.isConfig && !writePayload(0, writeFrame(pBuffer, configTag, _audioCodec)))
				return;
		}

		if (!writePayload(0, writeFrame(pBuffer, tag, packet))) 
			return;
	};
	_onVideo = [this](UInt16 track, const Media::Video::Tag& tag, const Packet& packet) {
		shared<Buffer> pBuffer;

		// Video codecs to be sent in first
		if (!_videoCodecSent) {

			Packet sps, pps;
			bool isAVCConfig(tag.codec == Media::Video::CODEC_H264 && tag.frame == Media::Video::FRAME_CONFIG && AVC::ParseVideoConfig(packet, sps, pps));
			if (isAVCConfig) {
				INFO("Video codec infos saved")
				_videoCodec.set(std::move(packet));
			}
			if (!_videoCodec)
				return;

			if (tag.frame != Media::Video::FRAME_KEY) {
				DEBUG("Video frame dropped to wait first key frame")
				return;
			}

			_videoCodecSent = true;
			INFO("Video codec infos sent")
			Media::Video::Tag configTag(tag);
			configTag.frame = Media::Video::FRAME_CONFIG;
			configTag.time = tag.time;
			if (!isAVCConfig && !writePayload(0, writeFrame(pBuffer, configTag, _videoCodec)))
				return;
		}
		// Send Regularly the codec infos (TODO: Add at timer?)
		else if (tag.codec == Media::Video::CODEC_H264 && tag.frame == Media::Video::FRAME_KEY) {
			DEBUG("Sending codec infos")
			Media::Video::Tag configTag(tag);
			configTag.frame = Media::Video::FRAME_CONFIG;
			configTag.time = tag.time;
			if (!writePayload(0, writeFrame(pBuffer, configTag, _videoCodec)))
				return;
		}

		if (!writePayload(0, writeFrame(pBuffer, tag, packet)))
			return;
	};
	_onEnd = [this]() {
		resetSRT();
	};
	INFO("A new publish client is connecting from ", client.address);
}

SRTOut::Client::~Client() {
	INFO("Client from ", client.address, " is disconnecting...")

	_srtPimpl->Close();

	resetSRT();
}

bool SRTOut::Client::onPublish(Exception& ex, Publication& publication) {
	INFO("Client from ", client.address, " is trying to publish ", publication.name())

	if (_pPublication) {
		WARN("Client is already publishing, request ignored")
		return false;
	}

	// Init parameters
	publication.onAudio = _onAudio;
	publication.onVideo = _onVideo;
	publication.onEnd = _onEnd;
	_pPublication = &publication;

	return true;
}

void SRTOut::Client::onUnpublish(Publication& publication) {
	INFO("Client from ", client.address, " has closed publication ", publication.name(), ", stopping the injection...")

	resetSRT();
}

bool SRTOut::Client::onInvocation(Mona::Exception& ex, const std::string& name, Mona::DataReader& arguments, Mona::UInt8 responseType) {
	const ::std::string &method = client.properties().getString("type");
	DEBUG("Client: ", name," call from ",client.protocol," to ",
		client.path.empty() ? "/" : client.path, " type ", responseType,
		" method ", method.empty() ? "UNKNOWN" : method)
	
	return true;
}

bool SRTOut::Client::onHTTPRequest(const std::string& method,
		const std::string& name, const std::string& body, std::string& response) {
	DEBUG("SRTOut::Client::onHTTPRequest");

	if (method != "GET")
		return false;

	if (name == "stats") {
		_srtPimpl->GetStatsJson(response);
	} else if (name == "status") {
		response = "{ \"State\": \"Connecting\" }";
	}

	return !response.empty();
}

void SRTOut::Client::resetSRT() {

	if (_pPublication) {
		_pPublication->onAudio = nullptr;
		_pPublication->onVideo = nullptr;
		_pPublication->onEnd = nullptr;
		_pPublication = NULL;
	}

	_tsWriter.endMedia([](const Packet& packet) {}); // reset the ts writer
	_videoCodec.reset();
	_audioCodec.reset();
	_first = false;
}

template <>
void SRTOut::Client::writeMedia<Media::Video::Tag>(Mona::BinaryWriter& writer, const Media::Video::Tag& tag, const Mona::Packet& packet) {
	_tsWriter.writeVideo(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
}

template <>
void SRTOut::Client::writeMedia<Media::Audio::Tag>(Mona::BinaryWriter& writer, const Media::Audio::Tag& tag, const Mona::Packet& packet) {
	_tsWriter.writeAudio(0, tag, packet, [&writer](const Packet& output) { writer.write(output); });
}

bool SRTOut::Client::writePayload(UInt16 context, shared_ptr<Buffer>& pBuffer) {

	int res = _srtPimpl->Write(pBuffer);

	return (res == (int)pBuffer->size());
}

App::Client* SRTOut::newClient(Mona::Exception& ex, Mona::Client& client, Mona::DataReader& parameters, Mona::DataWriter& response) {
	DEBUG("SRTOut::newClient for path ", client.path.empty() ? "/" : client.path);

	// NOTE: We don't really want more than one client in our use case.
	//       The list is there for future expansion.
	if (_client != nullptr) {
		ERROR("SRTOut already has a client!")
		return nullptr;
	}

	_client = new Client(client, *this);

	return _client;
}

void SRTOut::closeClients() {
	DEBUG("SRTOut::closeClients");

	SRTOut::Client* p = _client;
	if (p)
		p->client.writer().close();
}

void SRTOut::deleteClient(App::Client* pClient) {
	// WARNING: the can be called from closeClients via disconnect callback
	delete _client;
	_client = nullptr;
}

bool SRTOut::onHTTPRequest(const std::string& method,
		const std::string& name, const std::string& body, std::string& response) {
	DEBUG("SRTOut::onHTTPRequest");

	if (method != "GET")
		return false;

	if (_client == nullptr) {
		response = "{ \"State\": \"NotConnected\" }";
		return true;
	}

	return _client->onHTTPRequest(method, name, body, response);
}
