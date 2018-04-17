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

#include "SRTIn.h"

#include "MonaSRT.h"
#include "SRTOut.h"
#include "Mona/Logs.h"

using namespace std;

namespace Mona {


//// External Publish ////



//// Server Events /////
void MonaSRT::onStart() {

	_applications["/srt"] = new SRTOut(*this);
	if (getBoolean<false>("SRT")) {
		_srtIn = new SRTIn(*this, *this);
		_srtIn->load();
	}
}

void MonaSRT::manage() {

	// manage application!
	for (auto& it : _applications)
		it.second->manage();
}

void MonaSRT::onStop() {
	
	// delete applications
	for (auto& it : _applications)
		delete it.second;
	_applications.clear();

	if (_srtIn) {
		delete _srtIn;
		_srtIn = nullptr;
	}

	// unblock ctrl+c waiting
	_terminateSignal.set();
}

//// Client Events /////
SocketAddress& MonaSRT::onHandshake(const string& path, const string& protocol, const SocketAddress& address, const Parameters& properties, SocketAddress& redirection) {
	DEBUG(protocol, " ", address, " handshake to ", path.empty() ? "/" : path);
	const auto& it(_applications.find(path));
	return it == _applications.end() ? redirection : it->second->onHandshake(protocol, address, properties, redirection);
}

void MonaSRT::onConnection(Exception& ex, Client& client, DataReader& parameters, DataWriter& response) {
	DEBUG(client.protocol, " ", client.address, " connects to ", client.path.empty() ? "/" : client.path)

	const auto& it(_applications.find(client.path));
	if (it == _applications.end())
		return;
	client.setCustomData<App::Client>(it->second->newClient(ex,client,parameters,response));
}

void MonaSRT::onDisconnection(Client& client) {
	DEBUG(client.protocol, " ", client.address, " disconnects from ", client.path.empty() ? "/" : client.path);
	if (client.hasCustomData()) {
		delete client.getCustomData<App::Client>();
		client.setCustomData<App::Client>(NULL);
	}
}

void MonaSRT::onAddressChanged(Client& client, const SocketAddress& oldAddress) {
	if (client.hasCustomData())
		client.getCustomData<App::Client>()->onAddressChanged(oldAddress);
}

bool MonaSRT::onInvocation(Exception& ex, Client& client, const string& name, DataReader& arguments, UInt8 responseType) {
	// on client message, returns "false" if "name" message is unknown
	DEBUG(name," call from ",client.protocol," to ",client.path.empty() ? "/" : client.path)
	if (client.hasCustomData())
		return client.getCustomData<App::Client>()->onInvocation(ex, name, arguments,responseType);
	return true;
} 


bool MonaSRT::onFileAccess(Exception& ex, File::Mode mode, Path& file, DataReader& arguments, DataWriter& properties, Client* pClient) {
	// on client file access, returns "false" if acess if forbiden
	if(pClient) {
		DEBUG(file.name(), " file access from ", pClient->protocol, " to ", pClient->path.empty() ? "/" : pClient->path);
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onFileAccess(ex, mode, file, arguments, properties);
	} else
		DEBUG(file.name(), " file access to ", file.parent().empty() ? "/" : file.parent());
	// arguments.read(properties); to test HTTP page properties (HTTP parsing!)
	return true;
}


//// Publication Events /////

bool MonaSRT::onPublish(Exception& ex, Publication& publication, Client* pClient) {
	if (pClient) {
		NOTE("Client publish ", publication.name());
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onPublish(ex, publication);
	} else
		NOTE("Publish ",publication.name())

	return true; // "true" to allow, "false" to forbid
}

void MonaSRT::onUnpublish(Publication& publication, Client* pClient) {
	if (pClient) {
		NOTE("Client unpublish ", publication.name());
		if(pClient->hasCustomData())
			pClient->getCustomData<App::Client>()->onUnpublish(publication);
	} else
		NOTE("Unpublish ",publication.name())
}

bool MonaSRT::onSubscribe(Exception& ex, const Subscription& subscription, const Publication& publication, Client* pClient) {
	if (pClient) {

		INFO(pClient->protocol, " ", pClient->address, " subscribe to ", publication.name());
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onSubscribe(ex, subscription, publication);
	} else
		INFO("Subscribe to ", publication.name());
	return true; // "true" to allow, "false" to forbid
} 

void MonaSRT::onUnsubscribe(const Subscription& subscription, const Publication& publication, Client* pClient) {
	if (pClient) {
		INFO(pClient->protocol, " ", pClient->address, " unsubscribe to ", publication.name());
		if (pClient->hasCustomData())
			return pClient->getCustomData<App::Client>()->onUnsubscribe(subscription, publication);
	} else
		INFO("Unsubscribe to ", publication.name());
}

} // namespace Mona
