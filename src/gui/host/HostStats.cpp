#include "gui/host/HostStats.h"
#include "gui/host/HostPlatform.h"
#include "Cafe/CafeSystem.h"
#include "config/CemuConfig.h"
#include "Common/version.h"
#include "Common/cpu_features.h"
#include "util/helpers/helpers.h"

#include <rapidjson/document.h>
#include <random>

namespace HostStats
{
	// the deployed stats worker; settings.xml <StatsEndpoint> overrides it (PC tests against a local server)
	constexpr const char* kStatsEndpoint = "https://cemu-uwp-stats.cemu-uwp.workers.dev";

	std::mutex s_mutex;
	std::condition_variable s_wake;
	std::thread s_thread;
	bool s_running = false;
	std::unordered_map<uint64, Counts> s_counts;
	uint32 s_online = 0;

	std::string _Endpoint()
	{
		std::string e = GetConfig().stats_endpoint.GetValue();
		if (e.empty())
			e = kStatsEndpoint;
		while (!e.empty() && e.back() == '/')
			e.pop_back();
		return e;
	}

	std::string _Console()
	{
		const std::string cpu = g_CPUFeatures.GetCPUName();
		if (cpu.find("Series S") != std::string::npos)
			return "seriesS";
		if (cpu.find("Series X") != std::string::npos)
			return "seriesX";
		return "pc";
	}

	std::string _NewId()
	{
		std::random_device rd;
		return fmt::format("{:08x}{:08x}{:08x}{:08x}", rd(), rd(), rd(), rd());
	}

	// #959: deletes everything the server has for this id, on a short-lived thread (never blocks the UI)
	void _Forget(std::string iid)
	{
		if (iid.size() != 32 || _Endpoint().empty())
			return;
		std::thread([iid, url = _Endpoint() + "/v1/forget"] {
			SetThreadName("HostStatsForget");
			std::string error;
			const bool ok = HostPlatform::HttpPostJson(url, fmt::format(R"({{"iid":"{}"}})", iid), error, 5000);
			cemuLog_log(LogType::Force, "Stats: delete request for id {}... {}", iid.substr(0, 4), ok ? "done" : "failed (" + error + ")");
		}).detach();
	}

	bool IsAvailable()
	{
		return !_Endpoint().empty();
	}

	// stats_consent: "" (default) or "yes" = on, "no" = turned off (in Settings or by an earlier test build's prompt)
	bool _IsOff()
	{
		return GetConfig().stats_consent.GetValue() == "no";
	}

	bool NeedsNotice()
	{
		return !_IsOff() && GetConfig().stats_notice_version.GetValue() < kNoticeVersion;
	}

	void DismissNotice()
	{
		auto& c = GetConfig();
		c.stats_notice_version = kNoticeVersion;
		if (!_IsOff() && c.stats_iid.GetValue().size() != 32)
			c.stats_iid = _NewId();
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Stats: notice dismissed, sharing {}", IsEnabled() ? fmt::format("on, id {}...", GetIdPrefix()) : std::string("off"));
		s_wake.notify_all();
	}

	bool IsEnabled()
	{
		const auto& c = GetConfig();
		return !_IsOff() && c.stats_notice_version.GetValue() >= kNoticeVersion && c.stats_iid.GetValue().size() == 32;
	}

	void SetConsent(bool yes)
	{
		auto& c = GetConfig();
		c.stats_consent = std::string(yes ? "yes" : "no");
		c.stats_notice_version = kNoticeVersion; // chosen in Settings: no notice needed any more
		if (yes && c.stats_iid.GetValue().size() != 32)
			c.stats_iid = _NewId();
		if (!yes)
		{
			_Forget(c.stats_iid.GetValue()); // turning it off also deletes what was sent
			c.stats_iid = std::string();
		}
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Stats: sharing {}{}", yes ? "on, id " : "off", yes ? GetIdPrefix() + "..." : "");
		s_wake.notify_all();
	}

	void ResetId()
	{
		auto& c = GetConfig();
		if (!IsEnabled())
			return;
		_Forget(c.stats_iid.GetValue());
		c.stats_iid = _NewId();
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Stats: stats deleted, new id {}...", GetIdPrefix());
	}

	std::string GetIdPrefix()
	{
		return GetConfig().stats_iid.GetValue().substr(0, 4);
	}

	std::string BuildHeartbeat(uint64 titleId)
	{
		std::string version = BUILD_VERSION_STRING;
		std::string clean;
		for (char ch : version)
			if (isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '+' || ch == '_')
				clean += ch;
		clean = clean.substr(0, 32);
		return fmt::format(R"({{"iid":"{}","tid":"{}","ver":"{}","console":"{}"}})", GetConfig().stats_iid.GetValue(),
			titleId ? fmt::format("{:016x}", titleId) : std::string(), clean.empty() ? "unknown" : clean, _Console());
	}

	void _SendHeartbeat(uint64 titleId)
	{
		if (!IsEnabled())
			return;
		std::string error;
		if (!HostPlatform::HttpPostJson(_Endpoint() + "/v1/hb", BuildHeartbeat(titleId), error, 5000))
			cemuLog_log(LogType::Force, "Stats: heartbeat not sent ({})", error);
	}

	void _FetchCounts()
	{
		std::vector<uint8> data;
		std::string error;
		if (!HostPlatform::HttpGet(_Endpoint() + "/v1/counts", data, error))
			return;
		rapidjson::Document doc;
		doc.Parse((const char*)data.data(), data.size());
		if (doc.HasParseError() || !doc.IsObject())
			return;
		std::unordered_map<uint64, Counts> counts;
		const uint32 online = doc.HasMember("online") && doc["online"].IsUint() ? doc["online"].GetUint() : 0;
		for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
		{
			if (!it->value.IsObject() || !it->value.HasMember("now") || !it->value.HasMember("week") || !it->value["now"].IsUint() || !it->value["week"].IsUint())
				continue;
			uint64 tid = 0;
			try { tid = std::stoull(it->name.GetString(), nullptr, 16); } catch (...) { continue; }
			counts[tid] = { it->value["now"].GetUint(), it->value["week"].GetUint() };
		}
		std::lock_guard lock(s_mutex);
		s_counts = std::move(counts);
		s_online = online;
	}

	void _Loop()
	{
		SetThreadName("HostStats");
		using clock = std::chrono::steady_clock;
		constexpr auto kInterval = std::chrono::minutes(5);
		bool wasEnabled = false;
		uint64 lastTitle = ~0ull;
		auto nextHeartbeat = clock::now();
		auto nextCounts = clock::now();
		std::unique_lock lock(s_mutex);
		while (s_running)
		{
			lock.unlock();
			const uint64 title = CafeSystem::IsTitleRunning() ? (uint64)CafeSystem::GetForegroundTitleId() : 0;
			const auto now = clock::now();
			if (title != lastTitle || IsEnabled() != wasEnabled)
			{
				lastTitle = title;
				wasEnabled = IsEnabled();
				nextHeartbeat = now; // a game just started / the launcher is back / sharing just turned on: count it right away
			}
			if (now >= nextHeartbeat)
			{
				nextHeartbeat = now + kInterval;
				_SendHeartbeat(title); // 0 = on the launcher, still counts as online
			}
			if (!title && now >= nextCounts) // the game list only shows on the launcher
			{
				nextCounts = now + kInterval;
				_FetchCounts();
			}
			lock.lock();
			s_wake.wait_for(lock, std::chrono::seconds(5), [] { return !s_running; });
		}
	}

	void Start()
	{
		if (!IsAvailable())
		{
			cemuLog_log(LogType::Force, "Stats: no endpoint set, off");
			return;
		}
		std::lock_guard lock(s_mutex);
		if (s_running)
			return;
		s_running = true;
		s_thread = std::thread(_Loop);
		cemuLog_log(LogType::Force, "Stats: {} (counts from {})", IsEnabled() ? fmt::format("sharing on, id {}...", GetIdPrefix()) : std::string("sharing off"), _Endpoint());
	}

	void Stop()
	{
		{
			std::lock_guard lock(s_mutex);
			if (!s_running)
				return;
			s_running = false;
		}
		s_wake.notify_all();
		if (s_thread.joinable())
			s_thread.join();
	}

	std::optional<Counts> GetCounts(uint64 titleId)
	{
		std::lock_guard lock(s_mutex);
		const auto it = s_counts.find(titleId);
		if (it == s_counts.end() || it->second.now == 0)
			return std::nullopt;
		return it->second;
	}

	uint32 GetOnline()
	{
		std::lock_guard lock(s_mutex);
		return s_online;
	}
}
