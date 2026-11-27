#include "gui/host/HostTools.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostUI.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Common/version.h"
#include "util/helpers/helpers.h"

#include <rapidjson/document.h>
#include <openssl/sha.h>
#include <zip.h>

#include <atomic>
#include <functional>
#include <fstream>
#include <mutex>
#include <thread>

#include <Windows.h>

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

	// copies a file that may be open for writing (log.txt): fs::copy_file can fail on those, reading it works.
	// The data is flushed to the device before returning: the target is usually a USB drive, and without the flush
	// a copy made shortly before the app is closed or the drive is pulled ends up as the right size but all zero bytes
	bool _CopyFile(const fs::path& from, const fs::path& to)
	{
		const std::string data = _ReadFile(from);
		std::error_code ec;
		if (data.empty() && !fs::exists(from, ec))
			return false;
		HANDLE file = CreateFile2(to.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return false;
		bool ok = true;
		size_t offset = 0;
		while (ok && offset < data.size())
		{
			DWORD written = 0;
			const DWORD chunk = (DWORD)std::min<size_t>(data.size() - offset, 1u << 24);
			ok = WriteFile(file, data.data() + offset, chunk, &written, nullptr) && written == chunk;
			offset += written;
		}
		ok = FlushFileBuffers(file) && ok;
		CloseHandle(file);
		return ok;
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
	const std::string log = _ReadFile(ActiveSettings::GetUserDataPath("log_previous.txt"));
	LastSessionProblem problem;
	// a crash log is definitive, also when the marker's last line says 'suspended' (#289: a relaunch crash was reported as
	// 'closed by the console while suspended')
	if (log.find("Crashlog for") != std::string::npos)
	{
		problem.title = "Cemu crashed last time";
		problem.text = fmt::format("{} crashed. The crash details are in the log.", game);
		cemuLog_log(LogType::Force, "Host: the last session crashed ({})", game);
		return problem;
	}
	// the last line tells whether the app was suspended (the console can close a suspended app, e.g. when another game
	// starts). That isn't a problem with Cemu
	const size_t lastSuspend = marker.rfind("suspended\n");
	const size_t lastResume = marker.rfind("resumed\n");
	if (lastSuspend != std::string::npos && (lastResume == std::string::npos || lastResume < lastSuspend))
	{
		cemuLog_log(LogType::Force, "Host: the last session was closed by the console while suspended ({})", game);
		return std::nullopt;
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
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
	// PC tests (#938): a folder that stands in for the USB drive
	if (const char* testRoot = std::getenv("CEMU_HOST_TEST_EXPORT_ROOT"))
		return _utf8ToPath(testRoot);
#endif
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
	if (!fallback.empty())
		return fallback;
	// #891: the Settings page calls this every frame; the drive probe below writes a file, so scan at most every 5 s and
	// log only when the answer changes (a tester's log had 9244 'exporting to D:/' lines in 2.5 minutes)
	static std::mutex s_scanMutex;
	static std::chrono::steady_clock::time_point s_lastScan{};
	static fs::path s_lastResult;
	static bool s_logged = false;
	std::scoped_lock scanLock(s_scanMutex);
	const auto now = std::chrono::steady_clock::now();
	if (s_lastScan != std::chrono::steady_clock::time_point{} && now - s_lastScan < std::chrono::seconds(5))
		return s_lastResult;
	s_lastScan = now;
	fs::path found;
	// GitHub #12: no game folders (games installed into mlc01) - use the first writable drive D: to Z: that isn't the
	// app's own, which on the Xbox is the USB drive
	for (wchar_t letter = L'D'; letter <= L'Z' && found.empty(); letter++)
	{
		const fs::path root = fs::path(std::wstring{ letter, L':', L'\\' });
		if (root == ownRoot)
			continue;
		std::error_code ec;
		if (!fs::is_directory(root, ec))
			continue;
		const fs::path probe = root / ".cemu_write_test";
		{
			std::unique_ptr<FileStream> f(FileStream::createFile2(probe));
			if (!f)
				continue;
		}
		fs::remove(probe, ec);
		found = root;
	}
	if (found != s_lastResult || !s_logged)
	{
		if (!found.empty())
			cemuLog_log(LogType::Force, "Host: no game folder drive, exporting to {}", _pathToUtf8(found));
		s_logged = true;
	}
	s_lastResult = found;
	return found;
}

void HostTools::StartSaveLogs()
{
	const fs::path root = GetExportRoot();
	if (root.empty())
	{
		HostUI::PushMessage("No USB drive", "Plug in a USB drive (formatted NTFS or exFAT), the logs are saved to it.");
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
		HostUI::PushMessage("No USB drive", "Plug in a USB drive (formatted NTFS or exFAT), the backup is saved to it.");
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
			// own error code: a failure on one file must not end the iteration (the loop stops when ec is set)
			std::error_code fileEc;
			if (!it->is_regular_file(fileEc))
				continue;
			const fs::path relative = it->path().lexically_relative(source);
			if (fileEc || relative.empty())
			{
				failed++;
				continue;
			}
			const fs::path target = folder / "mlc01" / "usr" / "save" / relative;
			fs::create_directories(target.parent_path(), fileEc);
			if (_CopyFile(it->path(), target))
				files++;
			else
				failed++;
		}
		if (ec)
			cemuLog_log(LogType::Force, "Host: save backup stopped early: {}", ec.message());
		cemuLog_log(LogType::Force, "Host: backed up {} save file(s) to {} ({} failed)", files, _pathToUtf8(folder), failed);
		if (files == 0)
			HostUI::PushMessage("Backup failed", fmt::format("Cemu couldn't write to {}", _pathToUtf8(folder)));
		else
			HostUI::PushMessage("Saves backed up", fmt::format("Copied {} files to {}{}\n\nTo restore, copy the mlc01 folder from the backup over Cemu's mlc01 folder.",
				files, _pathToUtf8(folder), failed ? fmt::format(" ({} files couldn't be copied)", failed) : std::string()));
	});
}

namespace
{
	std::atomic_bool s_saveFolderMissing = false;
}

bool HostTools::IsSaveFolderMissing()
{
	return s_saveFolderMissing;
}

void HostTools::SetSaveFolderMissing()
{
	s_saveFolderMissing = true;
}

void HostTools::StartSetSaveFolder(fs::path folder)
{
	if (folder.empty())
	{
		GetConfig().mlc_path = std::string();
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Host: save folder reset to the app's own storage");
		HostUI::PushMessage("Save folder", "Cemu uses its own storage for saves again after a restart. Saves in the old save folder stay there.");
		return;
	}
	_RunTask("Copying saves...", [folder]() {
		const fs::path source = ActiveSettings::GetMlcPath("usr/save");
		const fs::path targetSave = folder / "usr" / "save";
		std::error_code ec;
		fs::create_directories(targetSave, ec);
		if (!fs::is_directory(targetSave, ec))
		{
			HostUI::PushMessage("Save folder not set", fmt::format("Cemu couldn't create {}", _pathToUtf8(targetSave)));
			return;
		}
		size_t files = 0, kept = 0, failed = 0;
		std::error_code canonEc1, canonEc2;
		const bool sameFolder = fs::weakly_canonical(source, canonEc1) == fs::weakly_canonical(targetSave, canonEc2);
		if (fs::is_directory(source, ec) && !sameFolder)
		{
			// #925: a game's save folder (usr/save/<high>/<low>) is copied as a whole or not at all. Mixing files of two
			// different saves of one game (the old one at the target, newer ones from a session that used Cemu's own
			// storage) can make the game call its save corrupted; a game whose save is already at the target keeps that one
			auto copyTitle = [&](const fs::path& titleDir, const fs::path& targetTitle) {
				std::error_code walkEc;
				for (auto it = fs::recursive_directory_iterator(titleDir, walkEc); !walkEc && it != fs::recursive_directory_iterator(); it.increment(walkEc))
				{
					std::error_code fileEc;
					if (!it->is_regular_file(fileEc))
						continue;
					const fs::path relative = it->path().lexically_relative(titleDir);
					if (fileEc || relative.empty())
					{
						failed++;
						continue;
					}
					const fs::path target = targetTitle / relative;
					fs::create_directories(target.parent_path(), fileEc);
					if (_CopyFile(it->path(), target))
						files++;
					else
						failed++;
				}
				if (walkEc)
					failed++; // the listing stopped early, not every save was copied
			};
			ec.clear();
			for (auto hi = fs::directory_iterator(source, ec); !ec && hi != fs::directory_iterator(); hi.increment(ec))
			{
				std::error_code dirEc;
				if (!hi->is_directory(dirEc))
					continue;
				for (auto lo = fs::directory_iterator(hi->path(), dirEc); !dirEc && lo != fs::directory_iterator(); lo.increment(dirEc))
				{
					std::error_code loEc;
					if (!lo->is_directory(loEc))
						continue;
					const fs::path targetTitle = targetSave / hi->path().filename() / lo->path().filename();
					if (fs::exists(targetTitle, loEc))
					{
						kept++;
						continue;
					}
					copyTitle(lo->path(), targetTitle);
				}
				if (dirEc)
					failed++;
			}
			if (ec)
				failed++;
		}
		if (failed)
		{
			cemuLog_log(LogType::Force, "Host: save folder {} not set, {} save file(s) couldn't be copied", _pathToUtf8(folder), failed);
			HostUI::PushMessage("Save folder not set", fmt::format("{} save files couldn't be copied to {}. Nothing was changed.", failed, _pathToUtf8(folder)));
			return;
		}
		GetConfig().mlc_path = _pathToUtf8(folder);
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Host: save folder set to {}: copied {} save file(s) from {}, {} game(s) already had a save there (kept as is)", _pathToUtf8(folder), files, _pathToUtf8(source), kept);
		HostUI::PushMessage("Save folder set", fmt::format("Restart Cemu to use {}.\n\nCopied {} save files there. {} games already had a save there, those were kept as they are. Your saves in Cemu's own storage stay as they are.\n\n"
			"Updates and DLC installed into Cemu's own storage aren't copied: keep them as folders next to the game, or install them again.", _pathToUtf8(folder), files, kept));
	});
}

namespace
{
	std::atomic_bool s_dataFolderMissing = false;
}

bool HostTools::IsDataFolderMissing()
{
	return s_dataFolderMissing;
}

void HostTools::SetDataFolderMissing()
{
	s_dataFolderMissing = true;
}

void HostTools::StartSetDataFolder(fs::path folder)
{
	if (folder.empty())
	{
		GetConfig().host_data_path = std::string();
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Host: Cemu data folder reset to the app's own storage");
		HostUI::PushMessage("Cemu data folder", "Cemu uses its own storage for shader caches, graphic packs and figures again after a restart. "
			"The copies on the drive stay there; what was added there since (new shader caches, figures) isn't copied back.");
		return;
	}
	_RunTask("Copying Cemu data...", [folder]() {
		std::error_code ec;
		fs::create_directories(folder, ec);
		if (!fs::is_directory(folder, ec))
		{
			HostUI::PushMessage("Cemu data folder not set", fmt::format("Cemu couldn't create {}", _pathToUtf8(folder)));
			return;
		}
		std::vector<std::pair<fs::path, fs::path>> trees; // current location -> target
		trees.emplace_back(ActiveSettings::GetCachePath("shaderCache"), folder / "shaderCache");
		for (const char* name : ActiveSettings::kExternalDataFolders)
			trees.emplace_back(ActiveSettings::GetUserDataPath(name), folder / name);
		// count first, so the progress shows how far it is
		uint64 totalBytes = 0;
		size_t totalFiles = 0;
		for (const auto& [source, target] : trees)
		{
			std::error_code canon1, canon2;
			if (!fs::is_directory(source, ec) || fs::weakly_canonical(source, canon1) == fs::weakly_canonical(target, canon2))
				continue;
			for (auto it = fs::recursive_directory_iterator(source, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				std::error_code fileEc;
				if (it->is_regular_file(fileEc))
				{
					totalFiles++;
					totalBytes += it->file_size(fileEc);
				}
			}
			ec.clear();
		}
		size_t files = 0, kept = 0, failed = 0, done = 0;
		uint64 doneBytes = 0;
		for (const auto& [source, target] : trees)
		{
			std::error_code canon1, canon2;
			if (!fs::is_directory(source, ec) || fs::weakly_canonical(source, canon1) == fs::weakly_canonical(target, canon2))
				continue;
			for (auto it = fs::recursive_directory_iterator(source, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				std::error_code fileEc;
				if (!it->is_regular_file(fileEc))
					continue;
				const uint64 size = it->file_size(fileEc);
				const fs::path relative = it->path().lexically_relative(source);
				const fs::path to = target / relative;
				done++;
				doneBytes += size;
				if (done % 16 == 1)
				{
					std::unique_lock _l(s_mutex);
					s_busyText = fmt::format("Copying Cemu data... {} of {} files, {} of {} MB", done, totalFiles, doneBytes >> 20, totalBytes >> 20);
				}
				if (fs::exists(to, fileEc))
				{
					kept++;
					continue;
				}
				fs::create_directories(to.parent_path(), fileEc);
				if (_CopyFile(it->path(), to))
					files++;
				else
					failed++;
			}
			if (ec)
			{
				failed++;
				ec.clear();
			}
		}
		if (failed)
		{
			cemuLog_log(LogType::Force, "Host: Cemu data folder {} not set, {} file(s) couldn't be copied", _pathToUtf8(folder), failed);
			HostUI::PushMessage("Cemu data folder not set", fmt::format("{} files couldn't be copied to {} (drive full?). Nothing was changed.", failed, _pathToUtf8(folder)));
			return;
		}
		GetConfig().host_data_path = _pathToUtf8(folder);
		GetConfigHandle().Save();
		cemuLog_log(LogType::Force, "Host: Cemu data folder set to {}: copied {} file(s) ({} MB), {} already there", _pathToUtf8(folder), files, doneBytes >> 20, kept);
		HostUI::PushMessage("Cemu data folder set", fmt::format("Restart Cemu to use {}.\n\nCopied {} files ({} MB), {} were already there and kept. "
			"The copies in Cemu's own storage stay as they are.\n\nShader caches are written while you play: a slow USB stick can make the first runs of a game stutter more.",
			_pathToUtf8(folder), files, doneBytes >> 20, kept));
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

namespace { uint32 _ReleasePackageBuild(const rapidjson::Document& doc); } // #966, below with the updater

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
		const uint32 remoteBuild = _ReleasePackageBuild(doc);
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
		// #966 a test build isn't on d3d12-backend, so the compare says 'ahead' even for an older ci-latest: the package
		// version (0.1.<CI run>) decides when both are known
		const uint32 ownBuild = HostPlatform::GetPackageBuild();
		if (ownBuild && remoteBuild && remoteBuild <= ownBuild)
		{
			cemuLog_log(LogType::Force, "Host: ci-latest package 0.1.{} isn't newer than this one (0.1.{}), no update offered", remoteBuild, ownBuild);
			return;
		}
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

// #925 save backups
namespace
{
	fs::path _SaveBackupRoot()
	{
		return ActiveSettings::GetMlcPath().parent_path() / "CemuBackups" / "auto";
	}

	fs::path _TitleSavePath(uint64 titleId)
	{
		return ActiveSettings::GetMlcPath(fmt::format("usr/save/{:08x}/{:08x}", (uint32)(titleId >> 32), (uint32)titleId));
	}

	// copies a folder tree, false if any file failed
	bool _CopyTree(const fs::path& from, const fs::path& to, uint64* bytes = nullptr)
	{
		std::error_code ec;
		bool ok = true;
		for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			std::error_code fileEc;
			if (!it->is_regular_file(fileEc))
				continue;
			const fs::path target = to / it->path().lexically_relative(from);
			fs::create_directories(target.parent_path(), fileEc);
			if (!_CopyFile(it->path(), target))
				ok = false;
			else if (bytes)
				*bytes += it->file_size(fileEc);
		}
		return ok && !ec;
	}

	uint64 _TreeSize(const fs::path& dir)
	{
		std::error_code ec;
		uint64 size = 0;
		for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			std::error_code fileEc;
			if (it->is_regular_file(fileEc))
				size += it->file_size(fileEc);
		}
		return size;
	}

	// kind 0: the backups made at game start (names start with the time), 1: saves replaced by a restore ('replaced-...')
	std::vector<fs::path> _BackupsOf(uint64 titleId, int kind = 0)
	{
		std::vector<fs::path> list;
		std::error_code ec;
		const fs::path dir = _SaveBackupRoot() / fmt::format("{:016x}", titleId);
		for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			std::error_code dirEc;
			const std::string name = _pathToUtf8(it->path().filename());
			const bool replaced = name.rfind("replaced-", 0) == 0;
			if (it->is_directory(dirEc) && replaced == (kind == 1))
				list.push_back(it->path());
		}
		std::sort(list.begin(), list.end()); // names are [replaced-]yyyymmdd-hhmmss
		return list;
	}

	std::string _BackupStamp()
	{
		const std::time_t t = std::time(nullptr);
		std::tm tm{};
		localtime_s(&tm, &t);
		return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	}

	// copies the current save into a new backup and keeps the newest 'keep'
	bool _MakeBackup(uint64 titleId, int kind, size_t keep)
	{
		const fs::path save = _TitleSavePath(titleId);
		std::error_code ec;
		if (!fs::is_directory(save, ec))
			return false;
		const uint64 size = _TreeSize(save);
		if (size > 64ull * 1024 * 1024)
		{
			cemuLog_log(LogType::Force, "Host: save backup of {:016x} skipped ({} MB is over 64 MB)", titleId, size / (1024 * 1024));
			return false;
		}
		const fs::path target = _SaveBackupRoot() / fmt::format("{:016x}", titleId) / ((kind == 1 ? "replaced-" : "") + _BackupStamp());
		if (!_CopyTree(save, target))
		{
			cemuLog_log(LogType::Force, "Host: save backup of {:016x} to {} failed", titleId, _pathToUtf8(target));
			fs::remove_all(target, ec);
			return false;
		}
		std::vector<fs::path> list = _BackupsOf(titleId, kind);
		while (list.size() > keep)
		{
			fs::remove_all(list.front(), ec);
			list.erase(list.begin());
		}
		cemuLog_log(LogType::Force, "Host: save of {:016x} backed up to {} ({} KB, {} kept)", titleId, _pathToUtf8(target), size / 1024, list.size());
		return true;
	}

	// copies 'from' next to the game's save and swaps it in; the caller backs the current save up first
	bool _ReplaceSave(uint64 titleId, const fs::path& from, const char* failTitle)
	{
		const fs::path save = _TitleSavePath(titleId);
		std::error_code ec;
		const fs::path temp = save.parent_path() / (_pathToUtf8(save.filename()) + ".restore");
		fs::remove_all(temp, ec);
		fs::create_directories(save.parent_path(), ec);
		if (!_CopyTree(from, temp))
		{
			fs::remove_all(temp, ec);
			cemuLog_log(LogType::Force, "Host: copying save {} for {:016x} failed", _pathToUtf8(from), titleId);
			HostUI::PushMessage(failTitle, "The save couldn't be copied. Your current save was not changed.");
			return false;
		}
		ec.clear();
		fs::remove_all(save, ec);
		ec.clear();
		fs::rename(temp, save, ec);
		if (ec)
		{
			cemuLog_log(LogType::Force, "Host: putting the save of {:016x} in place failed: {}", titleId, ec.message());
			HostUI::PushMessage(failTitle, fmt::format("Moving the save into place failed ({}). The previous save is in CemuBackups/auto.", ec.message()));
			return false;
		}
		return true;
	}
}

void HostTools::BackupTitleSave(uint64 titleId)
{
	_MakeBackup(titleId, 0, 3);
}

std::vector<HostTools::SaveBackup> HostTools::ListSaveBackups()
{
	std::vector<SaveBackup> result;
	std::error_code ec;
	for (auto it = fs::directory_iterator(_SaveBackupRoot(), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
	{
		uint64 titleId = 0;
		const std::string name = _pathToUtf8(it->path().filename());
		if (name.size() != 16 || std::sscanf(name.c_str(), "%llx", &titleId) != 1)
			continue;
		const std::vector<fs::path> list = _BackupsOf(titleId);
		if (!list.empty())
			result.push_back({ titleId, _pathToUtf8(list.back().filename()), list.size() });
	}
	return result;
}

void HostTools::StartRestoreSaveBackup(uint64 titleId)
{
	_RunTask("Restoring save...", [titleId]() {
		const std::vector<fs::path> list = _BackupsOf(titleId);
		if (list.empty())
			return;
		const fs::path newest = list.back();
		// keep the save being replaced apart from the start backups (the last 2), so restoring again still picks the start backup
		_MakeBackup(titleId, 1, 2);
		if (!_ReplaceSave(titleId, newest, "Save not restored"))
			return;
		cemuLog_log(LogType::Force, "Host: save of {:016x} restored from {}", titleId, _pathToUtf8(newest));
		HostUI::PushMessage("Save restored", fmt::format("The save from {} is back. The save you had before is kept as a backup too.", _pathToUtf8(newest.filename())));
	});
}

// #938 save manager
namespace
{
	std::string _SafeFolderName(std::string name)
	{
		for (char& ch : name)
			if (std::strchr("<>:\"/\\|?*", ch) || (unsigned char)ch < 0x20)
				ch = '_';
		while (!name.empty() && (name.back() == '.' || name.back() == ' '))
			name.pop_back();
		return name.empty() ? std::string("Game") : name;
	}

	// a usable save folder: has user/ or meta/
	bool _LooksLikeSave(const fs::path& dir)
	{
		std::error_code ec;
		return fs::is_directory(dir / "user", ec) || fs::is_directory(dir / "meta", ec);
	}
}

bool HostTools::HasTitleSave(uint64 titleId)
{
	std::error_code ec;
	return fs::is_directory(_TitleSavePath(titleId), ec);
}

void HostTools::StartExportSave(uint64 titleId, std::string gameName)
{
	const fs::path root = GetExportRoot();
	if (root.empty())
	{
		HostUI::PushMessage("No USB drive", "Plug in a USB drive (formatted NTFS or exFAT), the save is copied to it.");
		return;
	}
	_RunTask("Exporting save...", [root, titleId, gameName]() {
		const fs::path save = _TitleSavePath(titleId);
		std::error_code ec;
		if (!fs::is_directory(save, ec))
		{
			HostUI::PushMessage("No save", "This game has no save data yet.");
			return;
		}
		const std::string folderName = fmt::format("{} ({:016x})", _SafeFolderName(gameName), titleId);
		const fs::path target = root / "CemuSaves" / folderName;
		const fs::path temp = root / "CemuSaves" / (folderName + " new");
		const fs::path previous = root / "CemuSaves" / (folderName + " previous");
		fs::remove_all(temp, ec);
		if (!_CopyTree(save, temp))
		{
			fs::remove_all(temp, ec);
			cemuLog_log(LogType::Force, "Host: exporting the save of {:016x} to {} failed", titleId, _pathToUtf8(temp));
			HostUI::PushMessage("Save not exported", fmt::format("Cemu couldn't write to {}", _pathToUtf8(root / "CemuSaves")));
			return;
		}
		// the export before this one stays as '... previous'
		if (fs::exists(target, ec))
		{
			fs::remove_all(previous, ec);
			fs::rename(target, previous, ec);
		}
		ec.clear();
		fs::rename(temp, target, ec);
		if (ec)
		{
			HostUI::PushMessage("Save not exported", fmt::format("Renaming the copy failed ({}). It is in {}", ec.message(), _pathToUtf8(temp)));
			return;
		}
		cemuLog_log(LogType::Force, "Host: save of {:016x} exported to {}", titleId, _pathToUtf8(target));
		HostUI::PushMessage("Save exported", fmt::format("Copied to {}\n\nImport it under Settings on another console, or for desktop Cemu copy the folder's contents to mlc01/usr/save/{:08x}/{:08x}.",
			_pathToUtf8(target), (uint32)(titleId >> 32), (uint32)titleId));
	});
}

std::vector<HostTools::ImportableSave> HostTools::ListImportableSaves(uint64 titleId)
{
	std::vector<ImportableSave> result;
	const fs::path root = GetExportRoot();
	if (root.empty())
		return result;
	std::error_code ec;
	auto add = [&](const fs::path& dir) {
		std::error_code dirEc;
		if (_LooksLikeSave(dir) && std::none_of(result.begin(), result.end(), [&](const ImportableSave& s) { return fs::equivalent(s.path, dir, dirEc); }))
			result.push_back({ _pathToUtf8(dir.lexically_relative(root)), dir });
	};
	// exports: CemuSaves/<name> (<title id>)[ previous]
	const std::string idText = fmt::format("({:016x})", titleId);
	for (auto it = fs::directory_iterator(root / "CemuSaves", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
	{
		const std::string name = _pathToUtf8(it->path().filename());
		if (name.find(idText) != std::string::npos && !name.ends_with(" new"))
			add(it->path());
	}
	// mlc01-style folders: <drive>/mlc01, <drive>/Cemu/mlc01 (desktop Cemu) and the 'Back up saves' folders CemuBackups/saves_*/mlc01
	const fs::path saveRel = fs::path("usr") / "save" / fmt::format("{:08x}", (uint32)(titleId >> 32)) / fmt::format("{:08x}", (uint32)titleId);
	std::vector<fs::path> mlcs = { root / "mlc01", root / "Cemu" / "mlc01" };
	ec.clear();
	for (auto it = fs::directory_iterator(root / "CemuBackups", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		if (_pathToUtf8(it->path().filename()).rfind("saves_", 0) == 0)
			mlcs.push_back(it->path() / "mlc01");
	for (const fs::path& mlc : mlcs)
		add(mlc / saveRel);
	return result;
}

void HostTools::StartImportSave(uint64 titleId, fs::path from)
{
	_RunTask("Importing save...", [titleId, from]() {
		if (!_LooksLikeSave(from))
		{
			HostUI::PushMessage("Save not imported", "That folder has no save data (no user or meta folder).");
			return;
		}
		// never replace a save without a copy of it
		if (HostTools::HasTitleSave(titleId) && !_MakeBackup(titleId, 1, 2))
		{
			HostUI::PushMessage("Save not imported", "Your current save couldn't be backed up first (over 64 MB or no space), so it was left as it is.");
			return;
		}
		if (!_ReplaceSave(titleId, from, "Save not imported"))
			return;
		cemuLog_log(LogType::Force, "Host: save of {:016x} imported from {}", titleId, _pathToUtf8(from));
		HostUI::PushMessage("Save imported", "Done. The save you had before is kept in CemuBackups/auto as 'replaced-<time>'.");
	});
}

// #947 EXPERIMENTAL in-app updater
namespace
{
	constexpr const char* kUpdateAsset = "cemu-uwp-x64.zip";

	// #966 N from '... (Xbox package 0.1.N.0) ...' in the ci-latest release text, 0 if not there
	uint32 _ReleasePackageBuild(const rapidjson::Document& doc)
	{
		if (!doc.HasMember("body") || !doc["body"].IsString())
			return 0;
		const std::string body = doc["body"].GetString();
		const size_t at = body.find("Xbox package 0.1.");
		return at == std::string::npos ? 0 : (uint32)std::strtoul(body.c_str() + at + 17, nullptr, 10);
	}

	std::string _Sha256Hex(const std::vector<uint8>& data)
	{
		uint8 md[SHA256_DIGEST_LENGTH];
		SHA256(data.data(), data.size(), md);
		std::string hex;
		for (uint8 b : md)
			hex += fmt::format("{:02x}", b);
		return hex;
	}

	// unpacks every file of the zip below target, false (and error) on the first failure
	bool _ExtractZip(const std::vector<uint8>& zipData, const fs::path& target, std::string& error)
	{
		zip_error_t zerr;
		zip_error_init(&zerr);
		zip_source_t* src = zip_source_buffer_create(zipData.data(), zipData.size(), 0, &zerr);
		zip_t* za = src ? zip_open_from_source(src, ZIP_RDONLY, &zerr) : nullptr;
		if (!za)
		{
			error = zip_error_strerror(&zerr);
			if (src)
				zip_source_free(src);
			zip_error_fini(&zerr);
			return false;
		}
		bool ok = true;
		const zip_int64_t count = zip_get_num_entries(za, 0);
		std::vector<uint8> buffer;
		for (zip_int64_t i = 0; i < count && ok; i++)
		{
			zip_stat_t st;
			if (zip_stat_index(za, i, 0, &st) != 0 || !(st.valid & ZIP_STAT_NAME) || !(st.valid & ZIP_STAT_SIZE))
				continue;
			const std::string name = st.name;
			if (name.empty() || name.back() == '/' || name.back() == '\\')
				continue;
			const fs::path rel = _utf8ToPath(name).lexically_normal();
			if (rel.empty() || rel.has_root_path() || *rel.begin() == "..")
			{
				error = "bad path in the zip: " + name;
				ok = false;
				break;
			}
			// #966 debug symbols aren't needed to install, and writing the .appxsym failed on Xbox ('Run as Game' temp)
			std::string ext = _pathToUtf8(rel.extension());
			std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
			if (ext == ".appxsym" || ext == ".pdb")
				continue;
			const bool needed = ext == ".msix" || ext == ".appx" || ext == ".msixbundle" || ext == ".appxbundle" || ext == ".cer";
			const fs::path out = target / rel;
			std::error_code ec;
			fs::create_directories(out.parent_path(), ec);
			zip_file_t* zf = zip_fopen_index(za, i, 0);
			if (!zf)
			{
				error = "can't read " + name;
				ok = false;
				break;
			}
			buffer.resize((size_t)st.size);
			const zip_int64_t read = st.size ? zip_fread(zf, buffer.data(), st.size) : 0;
			zip_fclose(zf);
			std::ofstream file(out, std::ios::binary | std::ios::trunc);
			if (read != (zip_int64_t)st.size || !file.write((const char*)buffer.data(), buffer.size()))
			{
				cemuLog_log(LogType::Force, "Updater: can't write {} ({})", _pathToUtf8(out), needed ? "needed, stopping" : "not needed, skipped");
				if (needed)
				{
					error = "can't write " + _pathToUtf8(out);
					ok = false;
				}
			}
		}
		zip_close(za);
		zip_error_fini(&zerr);
		return ok;
	}
}

void HostTools::StartUpdate()
{
	_RunTask("Update: asking GitHub...", []() {
		auto setStatus = [](std::string text) {
			std::unique_lock _l(s_mutex);
			s_busyText = std::move(text);
		};
		auto fail = [](const std::string& step, const std::string& error) {
			cemuLog_log(LogType::Force, "Updater: {} failed: {}", step, error);
			HostUI::PushMessage("Update failed", fmt::format("{} failed:\n{}\n\nDownload {} from {} on a PC instead.", step, error, kUpdateAsset, kDownloadUrl));
		};
		cemuLog_log(LogType::Force, "Updater: start, this build is {}", BUILD_VERSION_STRING);
		std::vector<uint8> data;
		std::string error;
		if (!HostPlatform::HttpGet(fmt::format("https://api.github.com/repos/{}/releases/tags/ci-latest", kRepo), data, error))
			return fail("Asking GitHub for the release", error);
		rapidjson::Document doc;
		doc.Parse((const char*)data.data(), data.size());
		if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("assets") || !doc["assets"].IsArray())
			return fail("Reading the release", "unexpected answer");
		const std::string build = doc.HasMember("name") && doc["name"].IsString() ? doc["name"].GetString() : "ci-latest";
		const uint32 remoteBuild = _ReleasePackageBuild(doc), ownBuild = HostPlatform::GetPackageBuild();
		cemuLog_log(LogType::Force, "Updater: installed package 0.1.{}, ci-latest package 0.1.{}", ownBuild, remoteBuild);
		if (ownBuild && remoteBuild && remoteBuild <= ownBuild)
		{
			HostUI::PushMessage("No update", fmt::format("The installed build (0.1.{}) is the same as or newer than ci-latest (0.1.{}).", ownBuild, remoteBuild));
			return;
		}
		std::string url, digest;
		uint64 size = 0;
		for (const auto& asset : doc["assets"].GetArray())
		{
			if (!asset.IsObject() || !asset.HasMember("name") || !asset["name"].IsString() || std::string(asset["name"].GetString()) != kUpdateAsset)
				continue;
			if (asset.HasMember("browser_download_url") && asset["browser_download_url"].IsString())
				url = asset["browser_download_url"].GetString();
			if (asset.HasMember("size") && asset["size"].IsUint64())
				size = asset["size"].GetUint64();
			// "sha256:<hex>", GitHub adds it to assets uploaded since mid 2025
			if (asset.HasMember("digest") && asset["digest"].IsString())
				digest = asset["digest"].GetString();
		}
		if (url.empty())
			return fail("Finding the package", fmt::format("{} isn't in the release", kUpdateAsset));
		cemuLog_log(LogType::Force, "Updater: {} = {} ({} bytes, digest '{}')", build, url, size, digest);

		auto lastShown = std::chrono::steady_clock::now();
		const bool downloaded = HostPlatform::HttpGet(url, data, error, [&](uint64 received, uint64 total) {
			const auto now = std::chrono::steady_clock::now();
			if (now - lastShown < std::chrono::milliseconds(250))
				return;
			lastShown = now;
			if (total == 0)
				total = size;
			setStatus(total ? fmt::format("Update: downloading {}% ({} / {} MB)...", received * 100 / total, received >> 20, total >> 20)
				: fmt::format("Update: downloading {} MB...", received >> 20));
		});
		if (!downloaded)
			return fail("Download", error);
		cemuLog_log(LogType::Force, "Updater: downloaded {} bytes", data.size());
		if (size != 0 && data.size() != size)
			return fail("Download", fmt::format("got {} bytes, the release says {}", data.size(), size));
		setStatus("Update: checking...");
		const std::string sha = _Sha256Hex(data);
		if (digest.rfind("sha256:", 0) == 0)
		{
			if (digest.substr(7) != sha)
				return fail("Checking the download", fmt::format("SHA-256 {} doesn't match {}", sha, digest));
			cemuLog_log(LogType::Force, "Updater: SHA-256 matches ({})", sha);
		}
		else
			cemuLog_log(LogType::Force, "Updater: the release has no digest, size checked only (SHA-256 {})", sha);

		setStatus("Update: unpacking...");
		std::error_code ec;
		fs::path folder = fs::temp_directory_path(ec);
		folder = ec ? ActiveSettings::GetUserDataPath("CemuUpdate") : folder / "CemuUpdate";
		fs::remove_all(folder, ec);
		if (!_ExtractZip(data, folder, error))
			return fail("Unpacking", error);
		data.clear();
		data.shrink_to_fit();
		fs::path package;
		std::vector<fs::path> dependencies;
		for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		{
			std::string ext = _pathToUtf8(it->path().extension());
			std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return (char)std::tolower((unsigned char)c); });
			const std::string rel = _pathToUtf8(it->path().lexically_relative(folder));
			if ((ext == ".msix" || ext == ".appx") && rel.find("Dependencies") != std::string::npos)
			{
				if (rel.find("x64") != std::string::npos)
					dependencies.push_back(it->path());
			}
			else if (ext == ".msix" && package.empty())
				package = it->path();
		}
		if (package.empty())
			return fail("Unpacking", "no .msix in the zip");
		cemuLog_log(LogType::Force, "Updater: package {} + {} dependency package(s) in {}", _pathToUtf8(package.filename()), dependencies.size(), _pathToUtf8(folder));

		// a copy on the USB drive first: the fallback if the install is refused, nothing has to be downloaded again
		fs::path usbCopy;
		const fs::path root = HostTools::GetExportRoot();
		if (!root.empty())
		{
			setStatus("Update: copying the package to the USB drive...");
			const fs::path target = root / "CemuUpdate";
			fs::remove_all(target, ec);
			if (_CopyTree(folder, target))
				usbCopy = target;
			cemuLog_log(LogType::Force, "Updater: copy to {} {}", _pathToUtf8(target), usbCopy.empty() ? "failed" : "done");
		}

		setStatus("Update: installing (Cemu closes when it's done)...");
		cemuLog_log(LogType::Force, "Updater: installing {}", _pathToUtf8(package));
		if (HostPlatform::InstallPackageUpdate(package, dependencies, error))
		{
			cemuLog_log(LogType::Force, "Updater: install finished");
			HostUI::PushMessage("Update installed", fmt::format("{} is installed. Close Cemu and start it again.", build));
			return;
		}
		cemuLog_log(LogType::Force, "Updater: install failed: {}", error);
		const fs::path keptAt = usbCopy.empty() ? folder : usbCopy;
		HostUI::PushMessage("Update downloaded - install it with the Device Portal",
			fmt::format("Cemu couldn't install the update itself:\n{}\n\nThe package is in {}.\n{}Open the Xbox Device Portal on a PC, Add, pick {}{}. "
				"Installed as an update, your settings and saves are kept.",
				error, _pathToUtf8(keptAt), usbCopy.empty() ? "" : "Plug the USB drive into the PC. ", _pathToUtf8(package.filename()),
				dependencies.empty() ? "" : " and add the files in Dependencies/x64 as dependencies"));
	});
}
