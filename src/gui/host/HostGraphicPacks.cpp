#include "gui/host/HostGraphicPacks.h"
#include "gui/host/HostPlatform.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"

#include <boost/algorithm/string.hpp>

#include <zip.h>
#include <rapidjson/document.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
	constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest";

	std::mutex s_mutex;
	HostGraphicPacks::Status s_status;
	std::atomic_bool s_busy = false;
	std::atomic_bool s_reloadPending = false;

	void _SetStatus(HostGraphicPacks::State state, std::string text, float progress = -1.0f)
	{
		std::unique_lock _l(s_mutex);
		s_status.state = state;
		s_status.text = std::move(text);
		s_status.progress = progress;
	}

	void _SetProgress(float progress)
	{
		std::unique_lock _l(s_mutex);
		s_status.progress = progress;
	}

	fs::path _DownloadFolder()
	{
		return ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks");
	}

	// empties downloadedGraphicPacks (deleting the folder itself and recreating it right away fails at times on Windows)
	void _ClearDownloadFolder()
	{
		std::error_code ec;
		const fs::path path = _DownloadFolder();
		for (fs::directory_iterator it(path, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
			fs::remove_all(it->path(), ec);
		fs::create_directories(path, ec);
	}

	bool _Extract(const std::vector<uint8>& zipData, std::string& error)
	{
		zip_error_t zerr;
		zip_error_init(&zerr);
		zip_source_t* src = zip_source_buffer_create(zipData.data(), zipData.size(), 0, &zerr);
		if (!src)
		{
			error = zip_error_strerror(&zerr);
			zip_error_fini(&zerr);
			return false;
		}
		zip_t* za = zip_open_from_source(src, ZIP_RDONLY, &zerr);
		if (!za)
		{
			error = zip_error_strerror(&zerr);
			zip_source_free(src);
			zip_error_fini(&zerr);
			return false;
		}
		_ClearDownloadFolder();
		const fs::path base = _DownloadFolder();
		const zip_int64_t count = zip_get_num_entries(za, 0);
		std::vector<uint8> buffer;
		sint32 written = 0;
		for (zip_int64_t i = 0; i < count; i++)
		{
			_SetProgress((float)i / (float)std::max<zip_int64_t>(1, count));
			zip_stat_t sb;
			zip_stat_init(&sb);
			if (zip_stat_index(za, i, 0, &sb) != 0 || !sb.name || sb.name[0] == '\0')
				continue;
			const std::string_view name(sb.name);
			if (name.find("..") != std::string_view::npos || name.front() == '/' || name.front() == '\\' || name.find(':') != std::string_view::npos)
				continue; // don't write outside the folder
			const fs::path path = base / _utf8ToPath(name);
			std::error_code ec;
			if (name.back() == '/')
			{
				fs::create_directories(path, ec);
				continue;
			}
			if (sb.size == 0 || sb.size > 128 * 1024 * 1024)
				continue;
			zip_file_t* file = zip_fopen_index(za, i, 0);
			if (!file)
				continue;
			buffer.resize((size_t)sb.size);
			const bool ok = zip_fread(file, buffer.data(), sb.size) == (zip_int64_t)sb.size;
			zip_fclose(file);
			if (!ok)
				continue;
			fs::create_directories(path.parent_path(), ec);
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (out.write((const char*)buffer.data(), buffer.size()))
				written++;
		}
		zip_discard(za);
		zip_error_fini(&zerr);
		if (written == 0)
		{
			error = "the archive contained no files";
			return false;
		}
		return true;
	}

	void _UpdateThread()
	{
		SetThreadName("GfxPackUpdate");
		using HostGraphicPacks::State;
		_SetStatus(State::Checking, "Checking for the latest community graphic packs...");
		std::vector<uint8> data;
		std::string error;
		if (!HostPlatform::HttpGet(kLatestReleaseUrl, data, error))
		{
			_SetStatus(State::Failed, fmt::format("Could not reach GitHub: {}", error));
			s_busy = false;
			return;
		}
		rapidjson::Document doc;
		doc.Parse((const char*)data.data(), data.size());
		std::string version, url;
		if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("name") && doc["name"].IsString() && doc.HasMember("assets") && doc["assets"].IsArray())
		{
			version = doc["name"].GetString();
			for (auto& asset : doc["assets"].GetArray())
			{
				if (asset.IsObject() && asset.HasMember("browser_download_url") && asset["browser_download_url"].IsString())
				{
					url = asset["browser_download_url"].GetString();
					break;
				}
			}
		}
		if (version.empty() || url.empty())
		{
			_SetStatus(State::Failed, "GitHub's reply didn't contain a graphic pack release");
			s_busy = false;
			return;
		}
		if (boost::iequals(version, HostGraphicPacks::GetInstalledVersion()))
		{
			_SetStatus(State::Done, fmt::format("Graphic packs are up to date ({})", version));
			s_busy = false;
			return;
		}

		_SetStatus(State::Downloading, fmt::format("Downloading {}...", version), 0.0f);
		if (!HostPlatform::HttpGet(url, data, error, [](uint64 received, uint64 total) { _SetProgress(total ? (float)((double)received / (double)total) : -1.0f); }))
		{
			_SetStatus(State::Failed, fmt::format("Download failed: {}", error));
			s_busy = false;
			return;
		}

		_SetStatus(State::Extracting, fmt::format("Installing {}...", version), 0.0f);
		if (!_Extract(data, error))
		{
			_SetStatus(State::Failed, fmt::format("Could not install the graphic packs: {}", error));
			s_busy = false;
			return;
		}
		{
			std::ofstream versionFile(_DownloadFolder() / "version.txt", std::ios::trunc);
			versionFile << version;
		}
		cemuLog_log(LogType::Force, "Installed community graphic packs {}", version);
		_SetStatus(State::Done, fmt::format("Installed graphic packs {}", version));
		s_reloadPending = true;
		s_busy = false;
	}
}

void HostGraphicPacks::StartUpdate()
{
	bool expected = false;
	if (!s_busy.compare_exchange_strong(expected, true))
		return;
	std::thread(_UpdateThread).detach();
}

HostGraphicPacks::Status HostGraphicPacks::GetStatus()
{
	std::unique_lock _l(s_mutex);
	return s_status;
}

bool HostGraphicPacks::IsBusy()
{
	return s_busy;
}

std::string HostGraphicPacks::GetInstalledVersion()
{
	std::ifstream file(_DownloadFolder() / "version.txt");
	std::string version;
	if (file)
		std::getline(file, version);
	boost::trim(version);
	return version;
}

void HostGraphicPacks::Update()
{
	if (!s_reloadPending || CafeSystem::IsTitleRunning())
		return;
	s_reloadPending = false;
	// keep the selections made so far, LoadAll() reads them back from the config
	SaveToConfig();
	GraphicPack2::ClearGraphicPacks();
	GraphicPack2::LoadAll();
}

// same as GraphicPacksWindow2::SaveStateToConfig
void HostGraphicPacks::SaveToConfig()
{
	auto& data = GetConfigHandle().data();
	data.graphic_pack_entries.clear();
	for (const auto& gp : GraphicPack2::GetGraphicPacks())
	{
		const fs::path filename = _utf8ToPath(gp->GetNormalizedPathString());
		if (gp->IsEnabled())
		{
			auto& entry = data.graphic_pack_entries[filename];
			for (const auto& preset : gp->GetActivePresets())
				entry.try_emplace(preset->category, preset->name);
		}
		else if (gp->IsDefaultEnabled())
		{
			data.graphic_pack_entries[filename].try_emplace("_disabled", "false");
		}
	}
	GetConfigHandle().Save();
}
