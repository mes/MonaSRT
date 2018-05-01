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

#pragma once

#include "Mona/Mona.h"
#include "Mona/Server.h"
#include "Mona/TerminateSignal.h"
#include "App.h"

struct SRTIn;
struct SRTOut;
namespace Mona {

struct SRTOpenParams {
	enum modeT {
		MODE_NOTSET = 0,
		MODE_CALLER = 1,
		MODE_SERVER = 2
	};
	modeT _mode;
	SocketAddress _address;
	::std::string _password;
	enum keyLenT {
		KEY_LEN_NOTSET = 0,
		KEY_LEN_96     = 12,
		KEY_LEN_128    = 16,
		KEY_LEN_256    = 32
	};
	keyLenT _keylen;

	SRTOpenParams() :
		_mode(MODE_NOTSET),
		_address(),
		_password(),
		_keylen(KEY_LEN_NOTSET)
	{
		// No-op
	}
	bool SetFromURL(const ::std::string& url);
};

// struct RouteDesc {
// 	::std::string _name;
// 
// 	enum typeT {
// 		TYPE_NOTSET       = 0,
// 		TYPE_SRTTS        = 1,
// 		TYPE_UDPTS        = 2,
// 		TYPE_RTMPSERVER   = 3
// 	};
// 
// 	typeT _ingressType;
// 	::std::string ingressURL;
// 	::std::string egressURL;
// };

struct MonaSRT : Server {
	MonaSRT(const std::string& wwwPath, UInt16 cores, TerminateSignal& terminateSignal) :
		Server(cores), _wwwPath(wwwPath), _terminateSignal(terminateSignal), _srtIn(nullptr) { }

	virtual ~MonaSRT() { stop(); }


protected:

	//// Server Events /////

	void onStart();
	void onStop();
	void manage();

	//// Client Events /////
	SocketAddress& onHandshake(const std::string& path, const std::string& protocol, const SocketAddress& address, const Parameters& properties, SocketAddress& redirection);
	void onConnection(Exception& ex, Client& client, DataReader& parameters, DataWriter& response);
	void onDisconnection(Client& client);

	void onAddressChanged(Client& client, const SocketAddress& oldAddress);
	bool onInvocation(Exception& ex, Client& client, const std::string& name, DataReader& arguments, UInt8 responseType);
	bool onFileAccess(Exception& ex, File::Mode mode, Path& file, DataReader& arguments, DataWriter& properties, Client* pClient);

	//// Publication Events /////

	bool onPublish(Exception& ex, Publication& publication, Client* pClient);
	void onUnpublish(Publication& publication, Client* pClient);

	bool onSubscribe(Exception& ex, const Subscription& subscription, const Publication& publication, Client* pClient);
	void onUnsubscribe(const Subscription& subscription, const Publication& publication, Client* pClient);

	TerminateSignal&			_terminateSignal;
	typedef std::map<std::string,App*> AppsMapT;
	AppsMapT	_appsByName;
	AppsMapT	_appsById;

	SRTIn*						_srtIn;
	std::string					_wwwPath;	
};

} // namespace Mona
