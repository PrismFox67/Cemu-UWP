#include "gui/host/HostIcons.h"

#include "Cafe/TitleList/TitleList.h"
#include "Cafe/Filesystem/fsc.h"
#include "util/helpers/helpers.h"

#include <mutex>
#include <thread>

namespace
{
	std::mutex s_mutex; // protects everything below
	std::unordered_map<TitleId, std::unique_ptr<HostIcons::Icon>> s_icons;
	std::unordered_set<TitleId> s_attempted;
	std::vector<TitleId> s_queue;
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

	void _Worker()
	{
		while (!s_stop)
		{
			TitleId titleId;
			{
				std::unique_lock _l(s_mutex);
				if (s_queue.empty())
				{
					s_running = false;
					return;
				}
				titleId = s_queue.front();
				s_queue.erase(s_queue.begin());
			}
			auto icon = _LoadIcon(titleId);
			std::unique_lock _l(s_mutex);
			if (icon)
				s_icons[titleId] = std::move(icon);
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
	}
	if (s_queue.empty() || s_running)
		return;
	if (s_worker.joinable())
	{
		_l.unlock();
		s_worker.join(); // finished, s_running is false
		_l.lock();
		if (s_running || s_queue.empty())
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
	s_running = false;
}
