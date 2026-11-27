#pragma once

// #954 phone companion: answers DanP's iOS/Android companion app (reimplemented from its wire format, no code taken
// from his UwpImGuiFrontend library). The app is a DSU server (UDP 26760); we act as a DSU client towards it and, on the
// same socket, send the extension packets: 0x140000 state every second, 0x180005 launch status, 0x180006 the launcher's
// selected game. The app can ask to launch a game (0x180004), select one (0x180006) and press arrow keys (0x120000).
// The game list (/catalogue) and box art/icons (/media/<type>) are served over HTTP on TCP 26761 (DSU port + 1).
// The app has no way to send a token, so: off by default, not remembered across runs, only answers the phone's IP
// (UDP) and private-network addresses (HTTP). Log lines start with "Companion: ".

#include <optional>

namespace HostCompanion
{
	constexpr uint16 kDsuPort = 26760;
	constexpr uint16 kHttpPort = kDsuPort + 1;

	struct Entry
	{
		uint64 titleId = 0;
		std::string name;
		std::string region;
		std::string file; // file/folder name of the game
		uint32 version = 0;
	};

	// phoneIp: the address of the phone running the app
	bool Start(const std::string& phoneIp);
	void Stop();
	bool IsRunning();
	std::string GetStatus(); // a line for the settings screen ("connected", error, ...)

	// launcher -> app. Thread safe
	void SetCatalogue(std::vector<Entry> entries);
	void SetSelection(uint64 titleId);
	// app -> launcher, polled by the UI thread
	std::optional<uint64> TakeLaunch(); // title id the app asked to start
	std::optional<uint64> TakeSelection(); // title id the app highlighted
	enum Nav : uint32 { kNavLeft = 1, kNavRight = 2, kNavUp = 4, kNavDown = 8 };
	uint32 TakeNav(); // arrow keys newly pressed since the last call

	// the protocol without the network part, for tests
	uint32 GetRevision();
	std::vector<std::vector<uint8>> HandlePacket(const uint8* data, size_t size); // returns the replies
	std::vector<uint8> BuildStatePacket();
	int HandleHttp(const std::string& target, std::string& contentType, std::string& body);
	bool IsPrivateAddress(const std::string& ip);
}
