#pragma once

// Game art for the launcher's game list: each title's own meta/iconTex.tga (the icon the Wii U menu shows) and its box
// art from GameTDB (art.gametdb.com, the 768x1080 coverHQ scans), downloaded once per title and kept in
// <user data>/covers. Both load on a worker thread, icons first; covers that can't be fetched (offline, unknown title)
// are tried again on the next launch.

#include "Cafe/TitleList/TitleId.h"

namespace HostIcons
{
	struct Icon
	{
		std::vector<uint8> rgba; // RGBA8, width * height * 4
		int width = 0;
		int height = 0;
		uint64 textureKey = 0; // for HostUI's texture provider, unique per loaded image
	};

	// loads the icons of these titles that aren't loaded yet, in the background
	void Request(const std::vector<TitleId>& titleIds);
	// the icon of a title, nullptr if it isn't loaded (yet) or the title has none. The pointer stays valid
	const Icon* Get(TitleId titleId);
	// the box art of a title, nullptr if it isn't downloaded (yet), turned off or GameTDB has none
	const Icon* GetCover(TitleId titleId);
	// deletes the downloaded box art, the next Request downloads it again
	void RefreshCovers();
	// stops the worker (before a game starts: it mounts titles)
	void Stop();
}
