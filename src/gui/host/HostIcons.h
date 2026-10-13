#pragma once

// Game icons (box art) for the launcher's game list: each title's own meta/iconTex.tga, the icon the Wii U menu shows.
// Loaded on a worker thread, nothing is downloaded.

#include "Cafe/TitleList/TitleId.h"

namespace HostIcons
{
	struct Icon
	{
		std::vector<uint8> rgba; // RGBA8, width * height * 4
		int width = 0;
		int height = 0;
	};

	// loads the icons of these titles that aren't loaded yet, in the background
	void Request(const std::vector<TitleId>& titleIds);
	// the icon of a title, nullptr if it isn't loaded (yet) or the title has none. The pointer stays valid
	const Icon* Get(TitleId titleId);
	// stops the worker (before a game starts: it mounts titles)
	void Stop();
}
