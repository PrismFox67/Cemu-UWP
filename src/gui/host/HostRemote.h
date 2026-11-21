#pragma once

// #936 phone remote: a small HTTP server on the local network. The launcher shows a QR code for
// http://<console IP>:<port>/?k=<token>; the page shows the running game and changes display settings live, so the TV
// isn't covered by a menu. Off by default and not remembered across runs; the token is new for every start of the server.

namespace HostRemote
{
	// starts or stops the server. Start returns false (and GetError says why) if the port can't be opened
	bool Start();
	void Stop();
	bool IsRunning();
	std::string GetError();

	// http://<IP>:<port>/?k=<token>, empty while stopped
	std::string GetUrl();

	// the page and the API without the network part, for tests: method "GET"/"POST", target incl. query string.
	// Returns the HTTP status, fills contentType and body
	int HandleRequest(const std::string& method, const std::string& target, std::string& contentType, std::string& body);
}
