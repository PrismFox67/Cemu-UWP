#include "gui/host/HostTools.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostUI.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Common/version.h"
#include "util/helpers/helpers.h"

#include <rapidjson/document.h>

#include <atomic>
#include <functional>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
	constexpr const char* kRepo = "PrismFox67/Cemu-UWP";
	constexpr const char* kDownloadUrl = "https://github.com/PrismFox67/Cemu-UWP/releases/tag/ci-latest";

	std::mutex s_mutex; // protects the strings below
	std::atomic_bool s_busy = false;
	std::string s_busyText;
	std::string s_newerBuild;

	fs::path _MarkerPath()
	{
		return ActiveSettings::GetUserDataPath("session_running.txt");
	}

	std::string _Timestamp()
	{
		const std::time_t now = std::time(nullptr);
		std::tm local{};
		localtime_s(&local, &now);
		char buffer[32];
		std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &local);
		return buffer;
	}

	std::string _ReadFile(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

	// copies a file that may be open for writing (log.txt): fs::copy_file can fail on those, reading it works
	bool _CopyFile(const fs::path& from, const fs::path& to)
	{
		std::ifstream in(from, std::ios::binary);
		if (!in)
			return false;
		std::ofstream out(to, std::ios::binary | std::ios::trunc);
		out << in.rdbuf();
		return (bool)out;
	}

	void _RunTask(std::string busyText, std::function<void()> task)
	{
		if (s_busy.exchange(true))
			return;
		{
			std::unique_lock _l(s_mutex);
			s_busyText = std::move(busyText);
		}
		std::thread([task = std::move(task)]() {
			SetThreadName("HostTools");
			task();
			s_busy = false;
		}).detach();
	}
}

void HostTools::MarkGameStarted(const std::string& gameName)
{
	std::ofstream(_MarkerPath(), std::ios::trunc) << "game " << gameName << "\n";
}

void HostTools::MarkGameStopped()
{
	std::error_code ec;
	fs::remove(_MarkerPath(), ec);
}

void HostTools::MarkSuspended(bool suspended)
{
	std::error_code ec;
	if (!fs::exists(_MarkerPath(), ec))
		return;
	std::ofstream(_MarkerPath(), std::ios::app) << (suspended ? "suspended\n" : "resumed\n");
}

std::optional<HostTools::LastSessionProblem> HostTools::CheckLastSession()
{
	std::error_code ec;
	if (!fs::exists(_MarkerPath(), ec))
		return std::nullopt;
	std::string marker = _ReadFile(_MarkerPath());
	std::erase(marker, '\r'); // written in text mode, Windows line endings
	fs::remove(_MarkerPath(), ec);
	std::string game = marker.substr(0, marker.find('\n'));
	if (game.rfind("game ", 0) == 0)
		game = game.substr(5);
	// the last line tells whether the app was suspended (the console can close a suspended app, e.g. when another game
	// starts). That isn't a problem with Cemu
	const size_t lastSuspend = marker.rfind("suspended\n");
	const size_t lastResume = marker.rfind("resumed\n");
	if (lastSuspend != std::string::npos && (lastResume == std::string::npos || lastResume < lastSuspend))
	{
		cemuLog_log(LogType::Force, "Host: the last session was closed by the console while suspended ({})", game);
		return std::nullopt;
	}

	const std::string log = _ReadFile(ActiveSettings::GetUserDataPath("log_previous.txt"));
	LastSessionProblem problem;
	if (log.find("Crashlog for") != std::string::npos)
	{
		problem.title = "Cemu crashed last time";
		problem.text = fmt::format("{} crashed. The crash details are in the log.", game);
		cemuLog_log(LogType::Force, "Host: the last session crashed ({})", game);
		return problem;
	}
	// the memory lines: "Host: memory 4300 MB of 5120 MB (83%)" and "texture memory pressure high"
	int lastPercent = -1;
	for (size_t pos = log.find("Host: memory "); pos != std::string::npos; pos = log.find("Host: memory ", pos + 1))
	{
		const size_t open = log.find('(', pos);
		const size_t end = log.find('\n', pos);
		if (open != std::string::npos && open < end)
			lastPercent = std::atoi(log.c_str() + open + 1);
	}
	const size_t lastHigh = log.rfind("texture memory pressure high");
	const size_t lastOff = log.rfind("texture memory pressure off");
	const bool pressureHigh = lastHigh != std::string::npos && (lastOff == std::string::npos || lastOff < lastHigh);
	if (pressureHigh || lastPercent >= 85)
	{
		problem.title = "Cemu ran out of memory last time";
		problem.text = fmt::format("{} closed without a crash report while memory was almost full{}. The Xbox closes apps that use too much "
			"memory. A lower resolution (Graphic packs: 1440p instead of 4K) usually fixes it.",
			game, lastPercent >= 0 ? fmt::format(" ({}% of the app's limit)", lastPercent) : std::string());
	}
	else
	{
		problem.title = "Cemu closed unexpectedly last time";
		problem.text = fmt::format("{} stopped without a crash report{}. It may have frozen, or it was closed from the Xbox menu while the "
			"game was running.", game, lastPercent >= 0 ? fmt::format(" (memory was at {}%)", lastPercent) : std::string());
	}
	cemuLog_log(LogType::Force, "Host: last session ended unexpectedly ({}), memory {}%", game, lastPercent);
	return problem;
}

fs::path HostTools::GetExportRoot()
{
	const fs::path ownRoot = ActiveSettings::GetUserDataPath().root_path();
	fs::path fallback;
	for (const auto& gamePath : GetConfig().game_paths)
	{
		const fs::path root = _utf8ToPath(gamePath).root_path();
		if (root.empty())
			continue;
		if (root != ownRoot)
			return root;
		if (fallback.empty())
			fallback = root;
	}
	return fallback;
}

void HostTools::StartSaveLogs()
{
	const fs::path root = GetExportRoot();
	if (root.empty())
	{
		HostUI::PushMessage("No game drive", "Add a game folder on your USB drive first (Settings > Game folders), the logs are saved to that drive.");
		return;
	}
	_RunTask("Saving logs...", [root]() {
		const fs::path folder = root / "CemuLogs" / _Timestamp();
		std::error_code ec;
		fs::create_directories(folder, ec);
		int copied = 0;
		for (const char* name : { "log.txt", "log_previous.txt" })
		{
			const fs::path source = ActiveSettings::GetUserDataPath(name);
			if (fs::exists(source, ec) && _CopyFile(source, folder / name))
				copied++;
		}
		if (copied == 0)
			HostUI::PushMessage("Couldn't save the logs", fmt::format("Cemu couldn't write to {}", _pathToUtf8(folder)));
		else
		{
			cemuLog_log(LogType::Force, "Host: saved {} log file(s) to {}", copied, _pathToUtf8(folder));
			HostUI::PushMessage("Logs saved", fmt::format("Saved to {}\n\nAttach log.txt and log_previous.txt to your bug report. After a crash and a restart, the crash is in log_previous.txt.", _pathToUtf8(folder)));
		}
	});
}

void HostTools::StartBackupSaves()
{
	const fs::path root = GetExportRoot();
	if (root.empty())
	{
		HostUI::PushMessage("No game drive", "Add a game folder on your USB drive first (Settings > Game folders), the backup is saved to that drive.");
		return;
	}
	_RunTask("Backing up saves...", [root]() {
		const fs::path source = ActiveSettings::GetMlcPath("usr/save");
		const fs::path folder = root / "CemuBackups" / ("saves_" + _Timestamp());
		std::error_code ec;
		if (!fs::is_directory(source, ec))
		{
			HostUI::PushMessage("No saves yet", "There is no save data to back up.");
			return;
		}
		size_t files = 0, failed = 0;
		for (auto it = fs::recursive_directory_iterator(source, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			if (!it->is_regular_file(ec))
				continue;
			const fs::path target = folder / "mlc01" / "usr" / "save" / fs::relative(it->path(), source, ec);
			fs::create_directories(target.parent_path(), ec);
			if (_CopyFile(it->path(), target))
				files++;
			else
				failed++;
		}
		cemuLog_log(LogType::Force, "Host: backed up {} save file(s) to {} ({} failed)", files, _pathToUtf8(folder), failed);
		if (files == 0)
			HostUI::PushMessage("Backup failed", fmt::format("Cemu couldn't write to {}", _pathToUtf8(folder)));
		else
			HostUI::PushMessage("Saves backed up", fmt::format("Copied {} files to {}{}\n\nTo restore, copy the mlc01 folder from the backup over Cemu's mlc01 folder.",
				files, _pathToUtf8(folder), failed ? fmt::format(" ({} files couldn't be copied)", failed) : std::string()));
	});
}

bool HostTools::IsBusy()
{
	return s_busy;
}

std::string HostTools::GetBusyText()
{
	std::unique_lock _l(s_mutex);
	return s_busyText;
}

void HostTools::StartUpdateCheck()
{
	std::thread([]() {
		SetThreadName("UpdateCheck");
		const std::string own = BUILD_VERSION_STRING; // the commit (short hash) this build was made from
		std::vector<uint8> data;
		std::string error;
		if (!HostPlatform::HttpGet(fmt::format("https://api.github.com/repos/{}/releases/tags/ci-latest", kRepo), data, error))
		{
			cemuLog_log(LogType::Force, "Host: update check failed: {}", error);
			return;
		}
		rapidjson::Document doc;
		doc.Parse((const char*)data.data(), data.size());
		if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name") || !doc["name"].IsString())
			return;
		// "CI build (d3d12-backend @ 1a2b3c4)"
		const std::string title = doc["name"].GetString();
		const size_t at = title.find("@ ");
		if (at == std::string::npos)
			return;
		std::string latest = title.substr(at + 2);
		latest = latest.substr(0, latest.find_first_of(") "));
		if (latest.empty() || own.empty() || own.rfind(latest, 0) == 0 || latest.rfind(own, 0) == 0)
		{
			cemuLog_log(LogType::Force, "Host: this is the latest build ({})", latest);
			return;
		}
		// only offer it if it really is newer, not an older build than a locally built one
		if (!HostPlatform::HttpGet(fmt::format("https://api.github.com/repos/{}/compare/{}...{}", kRepo, own, latest), data, error))
			return;
		doc.Parse((const char*)data.data(), data.size());
		if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("ahead_by") || !doc["ahead_by"].IsInt())
			return;
		const int ahead = doc["ahead_by"].GetInt();
		cemuLog_log(LogType::Force, "Host: build {} is {} commit(s) ahead of this one ({})", latest, ahead, own);
		if (ahead <= 0)
			return;
		std::unique_lock _l(s_mutex);
		s_newerBuild = fmt::format("{} ({} change{} since this build)", latest, ahead, ahead == 1 ? "" : "s");
	}).detach();
}

std::string HostTools::GetNewerBuild()
{
	std::unique_lock _l(s_mutex);
	return s_newerBuild;
}

const char* HostTools::GetDownloadUrl()
{
	return kDownloadUrl;
}
