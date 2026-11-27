#pragma once

// #958/#963 anonymous play stats (EXPERIMENTAL, test builds). On by default (opt-out in Settings > Privacy): a
// heartbeat {random install id, title id or '' on the launcher, Cemu version, console type} goes to the stats endpoint
// every 5 minutes, but only after the one-time notice was dismissed. Everyone fetches the read-only counts ('N people
// online', 'N playing now'); that request carries no id. Never blocks the UI or a game, silent on failure.
// See docs/PRIVACY.md.

#include <optional>

namespace HostStats
{
	constexpr int kNoticeVersion = 1; // bump when what is sent changes -> the notice is shown again
	constexpr const char* kPrivacyUrl = "https://github.com/PrismFox67/Cemu-UWP/blob/d3d12-backend/docs/PRIVACY.md";

	bool IsAvailable(); // an endpoint is set
	bool NeedsNotice(); // the one-time notice wasn't dismissed for this kNoticeVersion (and sharing isn't off)
	void DismissNotice();
	bool IsEnabled(); // not turned off (an earlier test build's 'No' stays off) and the notice was seen
	void SetConsent(bool yes); // Settings toggle. Off also asks the server to delete everything sent with the id
	void ResetId(); // 'Delete my stats and reset ID': deletes the old id's data on the server, makes a new id
	std::string GetIdPrefix(); // first 4 characters, for the UI and the log

	void Start(); // worker thread
	void Stop();

	struct Counts { uint32 now = 0, week = 0; };
	std::optional<Counts> GetCounts(uint64 titleId); // nullopt if unknown / unreachable / nobody playing now
	uint32 GetOnline(); // people online now (launcher or game), 0 if unknown / unreachable

	// tests
	std::string BuildHeartbeat(uint64 titleId); // the JSON body
}
