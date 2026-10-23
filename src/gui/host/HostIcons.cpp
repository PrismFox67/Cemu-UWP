#include "gui/host/HostIcons.h"

#include "Cafe/TitleList/TitleList.h"
#include "Cafe/Filesystem/fsc.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostTheme.h"

#include <mutex>
#include <thread>

namespace
{
	std::mutex s_mutex; // protects everything below
	std::unordered_map<TitleId, std::unique_ptr<HostIcons::Icon>> s_icons;
	std::unordered_set<TitleId> s_attempted;
	std::vector<TitleId> s_queue;
	std::unordered_map<TitleId, std::unique_ptr<HostIcons::Icon>> s_covers;
	std::unordered_set<TitleId> s_coverAttempted;
	std::vector<TitleId> s_coverQueue; // fetched after all icons, downloads are slow
	uint64 s_nextCoverKey = 0xB0C0000000000000ull; // texture keys that are no title ID
	std::thread s_worker;
	std::atomic_bool s_stop = false;
	bool s_running = false;

	// Wii U icons are 128x128 TGAs, uncompressed (type 2) or RLE (type 10), 24 or 32 bit
	bool _DecodeTGA(const std::vector<uint8>& data, HostIcons::Icon& out)
	{
		if (data.size() < 18)
			return false;
		const uint8 idLength = data[0];
		const uint8 colorMapType = data[1];
		const uint8 imageType = data[2];
		const int width = data[12] | (data[13] << 8);
		const int height = data[14] | (data[15] << 8);
		const int bpp = data[16];
		const bool topDown = (data[17] & 0x20) != 0;
		if (colorMapType != 0 || (imageType != 2 && imageType != 10) || (bpp != 24 && bpp != 32) || width <= 0 || height <= 0 || width > 1024 || height > 1024)
			return false;
		const int bytesPerPixel = bpp / 8;
		const size_t pixelCount = (size_t)width * height;
		std::vector<uint8> bgra(pixelCount * 4, 255);
		size_t src = 18 + idLength;
		auto readPixel = [&](size_t pixel) -> bool {
			if (src + bytesPerPixel > data.size())
				return false;
			for (int c = 0; c < bytesPerPixel; c++)
				bgra[pixel * 4 + c] = data[src + c];
			src += bytesPerPixel;
			return true;
		};
		if (imageType == 2)
		{
			for (size_t i = 0; i < pixelCount; i++)
			{
				if (!readPixel(i))
					return false;
			}
		}
		else
		{
			size_t i = 0;
			while (i < pixelCount)
			{
				if (src >= data.size())
					return false;
				const uint8 packet = data[src++];
				const size_t count = (packet & 0x7F) + 1;
				if (i + count > pixelCount)
					return false;
				if (packet & 0x80)
				{
					if (!readPixel(i))
						return false;
					for (size_t k = 1; k < count; k++)
						memcpy(&bgra[(i + k) * 4], &bgra[i * 4], 4);
				}
				else
				{
					for (size_t k = 0; k < count; k++)
					{
						if (!readPixel(i + k))
							return false;
					}
				}
				i += count;
			}
		}
		out.width = width;
		out.height = height;
		out.rgba.resize(pixelCount * 4);
		for (int y = 0; y < height; y++)
		{
			const int srcRow = topDown ? y : height - 1 - y;
			for (int x = 0; x < width; x++)
			{
				const uint8* p = &bgra[((size_t)srcRow * width + x) * 4];
				uint8* d = &out.rgba[((size_t)y * width + x) * 4];
				d[0] = p[2];
				d[1] = p[1];
				d[2] = p[0];
				d[3] = p[3];
			}
		}
		return true;
	}

	std::unique_ptr<HostIcons::Icon> _LoadIcon(TitleId titleId)
	{
		TitleInfo titleInfo;
		if (!CafeTitleList::GetFirstByTitleId(titleId, titleInfo))
			return nullptr;
		const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
		if (!titleInfo.Mount(mountPath, "", FSC_PRIORITY_BASE))
			return nullptr;
		auto tga = fsc_extractFile((mountPath + "/meta/iconTex.tga").c_str());
		if (!tga)
		{
			tga = fsc_extractFile((mountPath + "/meta/iconTex.tga.gz").c_str());
			if (tga)
				tga = zlibDecompress(*tga, 70 * 1024);
		}
		titleInfo.Unmount(mountPath);
		auto icon = std::make_unique<HostIcons::Icon>();
		if (!tga || !_DecodeTGA(*tga, *icon))
		{
			cemuLog_log(LogType::Force, "Host: no icon for title {:016x}", titleId);
			return nullptr;
		}
		return icon;
	}

	fs::path _CoverFolder()
	{
		return ActiveSettings::GetUserDataPath("covers");
	}

	fs::path _CoverPath(TitleId titleId)
	{
		return _CoverFolder() / fmt::format("{:016x}.jpg", titleId);
	}

	// GameTDB's ID of a Wii U title: the last 4 characters of meta.xml's product code (WUP-P-AMKE) and the last 2 of the
	// company code (0001), e.g. AMKE01
	std::string _GameTdbId(const std::string& productCode, const std::string& companyCode)
	{
		if (productCode.size() < 4 || companyCode.size() < 2)
			return {};
		std::string id = productCode.substr(productCode.size() - 4) + companyCode.substr(companyCode.size() - 2);
		for (char c : id)
			if (!std::isalnum((unsigned char)c))
				return {};
		return id;
	}

	// GameTDB's region folders to try: the title's region (Europe in the console language), then the others
	std::vector<std::string> _GameTdbRegions(CafeConsoleRegion region)
	{
		std::vector<std::string> regions;
		auto add = [&](const char* r) {
			if (std::find(regions.begin(), regions.end(), r) == regions.end())
				regions.emplace_back(r);
		};
		if (HAS_FLAG(region, CafeConsoleRegion::USA))
			add("US");
		if (HAS_FLAG(region, CafeConsoleRegion::EUR))
		{
			switch (GetConfig().console_language.GetValue())
			{
			case CafeConsoleLanguage::FR: add("FR"); break;
			case CafeConsoleLanguage::DE: add("DE"); break;
			case CafeConsoleLanguage::ES: add("ES"); break;
			case CafeConsoleLanguage::IT: add("IT"); break;
			case CafeConsoleLanguage::NL: add("NL"); break;
			case CafeConsoleLanguage::PT: add("PT"); break;
			case CafeConsoleLanguage::RU: add("RU"); break;
			default: break;
			}
			add("EN");
		}
		if (HAS_FLAG(region, CafeConsoleRegion::JPN))
			add("JA");
		add("US");
		add("EN");
		add("JA");
		return regions;
	}

	bool _DownloadCover(TitleId titleId, const fs::path& path)
	{
		TitleInfo titleInfo;
		if (!CafeTitleList::GetFirstByTitleId(titleId, titleInfo) || !titleInfo.GetMetaInfo())
			return false;
		const ParsedMetaXml* meta = titleInfo.GetMetaInfo();
		const std::string id = _GameTdbId(meta->GetProductCode(), meta->GetCompanyCode());
		if (id.empty())
			return false;
		// coverHQ is 768x1080, coverM 350x500 for titles without an HQ scan
		for (const char* size : { "coverHQ", "coverM" })
		{
			for (const std::string& region : _GameTdbRegions(meta->GetRegion()))
			{
				if (s_stop)
					return false;
				std::vector<uint8> data;
				std::string error;
				const std::string url = fmt::format("https://art.gametdb.com/wiiu/{}/{}/{}.jpg", size, region, id);
				if (!HostPlatform::HttpGet(url, data, error))
				{
					// 404 = no scan in this region, anything else (offline) won't get better with more requests
					if (error.find("404") == std::string::npos)
					{
						cemuLog_log(LogType::Force, "Host: box art for {} not downloaded: {}", id, error);
						return false;
					}
					continue;
				}
				if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8)
					continue;
				std::error_code ec;
				fs::create_directories(path.parent_path(), ec);
				const fs::path temp = path.string() + ".part";
				{
					std::ofstream file(temp, std::ios::binary | std::ios::trunc);
					file.write((const char*)data.data(), (std::streamsize)data.size());
					if (!file)
						return false;
				}
				fs::rename(temp, path, ec);
				cemuLog_log(LogType::Force, "Host: box art for {} ({}/{}) downloaded from GameTDB", id, size, region);
				return !ec;
			}
		}
		cemuLog_log(LogType::Force, "Host: GameTDB has no box art for {}", id);
		return false;
	}

	std::unique_ptr<HostIcons::Icon> _LoadCover(TitleId titleId)
	{
		const fs::path path = _CoverPath(titleId);
		std::error_code ec;
		if (!fs::exists(path, ec) && !_DownloadCover(titleId, path))
			return nullptr;
		auto cover = std::make_unique<HostIcons::Icon>();
		if (!HostTheme::DecodeImageFile(path, cover->rgba, cover->width, cover->height))
		{
			fs::remove(path, ec); // damaged, download it again next time
			return nullptr;
		}
		return cover;
	}

	void _Worker()
	{
		while (!s_stop)
		{
			TitleId titleId;
			bool cover = false;
			{
				std::unique_lock _l(s_mutex);
				if (!s_queue.empty())
				{
					titleId = s_queue.front();
					s_queue.erase(s_queue.begin());
				}
				else if (!s_coverQueue.empty())
				{
					titleId = s_coverQueue.front();
					s_coverQueue.erase(s_coverQueue.begin());
					cover = true;
				}
				else
				{
					s_running = false;
					return;
				}
			}
			if (cover)
			{
				auto image = _LoadCover(titleId);
				std::unique_lock _l(s_mutex);
				if (image)
				{
					image->textureKey = s_nextCoverKey++;
					s_covers[titleId] = std::move(image);
				}
				continue;
			}
			auto icon = _LoadIcon(titleId);
			std::unique_lock _l(s_mutex);
			if (icon)
			{
				icon->textureKey = titleId;
				s_icons[titleId] = std::move(icon);
			}
		}
		std::unique_lock _l(s_mutex);
		s_running = false;
	}
}

void HostIcons::Request(const std::vector<TitleId>& titleIds)
{
	std::unique_lock _l(s_mutex);
	for (TitleId titleId : titleIds)
	{
		if (s_attempted.insert(titleId).second)
			s_queue.emplace_back(titleId);
		if (GetConfig().host_box_art.GetValue() && s_coverAttempted.insert(titleId).second)
			s_coverQueue.emplace_back(titleId);
	}
	if ((s_queue.empty() && s_coverQueue.empty()) || s_running)
		return;
	if (s_worker.joinable())
	{
		_l.unlock();
		s_worker.join(); // finished, s_running is false
		_l.lock();
		if (s_running || (s_queue.empty() && s_coverQueue.empty()))
			return;
	}
	s_stop = false;
	s_running = true;
	s_worker = std::thread(_Worker);
}

const HostIcons::Icon* HostIcons::Get(TitleId titleId)
{
	std::unique_lock _l(s_mutex);
	const auto it = s_icons.find(titleId);
	return it != s_icons.end() ? it->second.get() : nullptr;
}

const HostIcons::Icon* HostIcons::GetCover(TitleId titleId)
{
	if (!GetConfig().host_box_art.GetValue())
		return nullptr;
	std::unique_lock _l(s_mutex);
	const auto it = s_covers.find(titleId);
	return it != s_covers.end() ? it->second.get() : nullptr;
}

void HostIcons::RefreshCovers()
{
	Stop();
	std::unique_lock _l(s_mutex);
	s_covers.clear(); // the texture provider keys change with the next load
	s_coverAttempted.clear();
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(_CoverFolder(), ec))
		fs::remove(entry.path(), ec);
}

void HostIcons::Stop()
{
	s_stop = true;
	if (s_worker.joinable())
		s_worker.join();
	std::unique_lock _l(s_mutex);
	// what wasn't loaded is tried again next time the list is shown
	for (TitleId titleId : s_queue)
		s_attempted.erase(titleId);
	s_queue.clear();
	for (TitleId titleId : s_coverQueue)
		s_coverAttempted.erase(titleId);
	s_coverQueue.clear();
	s_running = false;
}
