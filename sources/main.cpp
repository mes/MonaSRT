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

#include "Mona/Server.h"
#include "Mona/ServerApplication.h"
#include "MonaSRT.h"
#include "MonaSRTVersion.h"

const int defaultHTTPRangeStart = 4900;
const int defaultHTTPRangeEnd = defaultHTTPRangeStart + 20;

using namespace std;
using namespace Mona;

struct ServerApp : ServerApplication  {

	ServerApp () : ServerApplication() {
		setBoolean("HTTP", false);
		setBoolean("HTTPS", false);
		setBoolean("WS", false);
		setBoolean("WSS", false);
		setBoolean("RTMFP", false);
		if ((false)) {
			setString("logs.maxsize", "50000000");
		}
	}

	const char* defineVersion() { return MONASRT_VERSION_STRING; }

///// MAIN
	int main(TerminateSignal& terminateSignal) {

		// find a we can bind to in the range
		Mona::unique<Socket> socket(new Socket(Socket::TYPE_STREAM));
		FATAL_CHECK(socket.get() != nullptr);
		
		UInt16 port = 0;
		for (UInt16 p = defaultHTTPRangeStart; p <= defaultHTTPRangeEnd; p++) {
			DEBUG("Trying port ", p);

			Exception ex;
			if (socket->bind(ex, SocketAddress(IPAddress::Loopback(), p))) {
				INFO("Will use port ", p, " for control endpoint");
				port = p;
				break;
			}
		}
		socket.reset();

		if (!port) {
			ERROR("Failed to find port for control channel (",
				defaultHTTPRangeStart, " - ", defaultHTTPRangeEnd, ")");
			return Application::EXIT_UNAVAILABLE;
		}

		setBoolean("HTTP", true);
		setNumber("HTTP.port", port);
		setString("HTTP.host", "127.0.0.1");

		// starts the server
		MonaSRT server(file().parent()+"www", getNumber<UInt32>("cores"), terminateSignal);

		server.start(*this);

		terminateSignal.wait();
		// Stop the server
		server.stop();

		return Application::EXIT_OK;
	}

	void defineOptions(Exception& ex, Options& options)
	{
		options.add(ex, "config", "c", "Specify JSON configuration.")
			.argument("<JSONconfig>")
			.handler([this](Exception& ex, const string& value) { 
				setString("json.config", value);
				return true; });

		ServerApplication::defineOptions(ex, options);
	}
private:
	std::string _target;
};

int main(int argc, const char* argv[]) {
	return ServerApp().run(argc, argv);
}
