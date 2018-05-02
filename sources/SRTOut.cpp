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

#include <sstream>

using namespace Mona;
using namespace std;

static const int64_t epollWaitTimoutMS = 250;
static const int64_t reconnectPeriodMS = 1000;

class SRTOut::Client::OpenSrtPIMPL : private Thread {
	private:
		::SRTSOCKET _socket;
		bool _started;
		string _host;
		std::mutex _mutex;
		std::string _errorDetail;
	public:
		OpenSrtPIMPL() :
			_socket(::SRT_INVALID_SOCK), _started(false), Thread("SRTOut") {
		}
		~OpenSrtPIMPL() {
			Close();
		}

		bool Open(const string& host) {
			if (_started) {
				ERROR("SRT Open: Already open, please close first")
				return false;
			}

			if (::srt_startup()) {
				ERROR("SRT Open: Error starting SRT library")
				return false;
			}
			_started = true;
			_host = host;

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
				_host.clear();
			}
		}

		bool Connect() {
			std::lock_guard<std::mutex> lock(_mutex);
			return ConnectActual();
		}

		bool ConnectActual() {
			SocketAddress addr;

			Exception ex;
			if (!addr.set(ex, _host) || addr.family() != IPAddress::IPv4) {
				ERROR("SRT Open: can't resolve target host, ", _host)
				return false;
			}
			
			if (_socket != ::SRT_INVALID_SOCK) {
				ERROR("Already connected, please disconnect first")
				return false;
			}

			_socket = ::srt_socket(AF_INET, SOCK_DGRAM, 0);
			if (_socket == ::SRT_INVALID_SOCK ) {
				ERROR("SRT create socket: ", ::srt_getlasterror_str());
				return false;
			}

			bool block = false;
			int rc = ::srt_setsockopt(_socket, 0, SRTO_SNDSYN, &block, sizeof(block));
			if (rc != 0)
			{
				ERROR("SRT SRTO_SNDSYN: ", ::srt_getlasterror_str());
				DisconnectActual();
				return false;
			}
			rc = ::srt_setsockopt(_socket, 0, SRTO_RCVSYN, &block, sizeof(block));
			if (rc != 0)
			{
				ERROR("SRT SRTO_RCVSYN: ", ::srt_getlasterror_str());
				DisconnectActual();
				return false;
			}

			int opt = 1;
			::srt_setsockflag(_socket, ::SRTO_SENDER, &opt, sizeof opt);

			::SRT_SOCKSTATUS state = ::srt_getsockstate(_socket);
			if (state != SRTS_INIT) {
				ERROR("SRT Connect: socket is in bad state; ", state)
				DisconnectActual();
				return false;
			}

			INFO("Connecting to ", addr.host(), " port ", addr.port())

			// SRT support only IPV4 so we convert to a sockaddr_in
			sockaddr soaddr;
			memcpy(&soaddr, addr.data(), sizeof(sockaddr)); // WARN: work only with ipv4 addresses
			soaddr.sa_family = AF_INET;
			if (::srt_connect(_socket, &soaddr, sizeof(sockaddr))) {
				ERROR("SRT Connect: ", ::srt_getlasterror_str());
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

		const char* GetSRTStateString()
		{
			if (!_errorDetail.empty())
				return "InError";
			if (srt_getsockstate(_socket) == SRTS_CONNECTED)
				return "Connected";

			return "Connecting";
		}

		void GetStatsJson(std::string& response) {
			std::ostringstream ostr;
	
			if (!_errorDetail.empty()) {
				ostr \
					<< "{\"State\":\"InError\"," \
					<< "\"ErrorDetail\"= \"" << _errorDetail \
					<< "\"}" << endl;
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
