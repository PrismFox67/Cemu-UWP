#pragma once

// #958 opt-in anonymous play stats (EXPERIMENTAL, test builds). With consent, a heartbeat {random install id, title id,
// Cemu version, console type} goes to the stats endpoint once when the launcher starts and every 5 minutes while a
// game runs. Everyone (consent or not) fetches the read-only per-game counts for the game list; that request carries
// no id. Dormant while no endpoint is set. Never blocks the UI or a game, silent on failure. See docs/PRIVACY.md.

#include <optional>

namespace HostStats
{
	constexpr int kConsentVersion = 1; // bump when what is sent changes -> everyone who said yes is asked again
	constexpr const char* kPrivacyUrl = "https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/PRIVACY.md";

	bool IsAvailable(); // an endpoint is set
	bool NeedsConsentPrompt(); // never asked, or said yes to an older version. "No" is never asked again
	bool IsEnabled();
	void SetConsent(bool yes); // off also asks the server to delete everything sent with the id
	void ResetId(); // 'Delete my stats and reset ID': deletes the old id's data on the server, makes a new id
	std::string GetIdPrefix(); // first 4 characters, for the UI and the log

	void Start(); // worker thread
	void Stop();

	struct Counts { uint32 now = 0, week = 0; };
	std::optional<Counts> GetCounts(uint64 titleId); // nullopt if unknown / unreachable / zero

	// tests
	std::string BuildHeartbeat(uint64 titleId); // the JSON body
}
