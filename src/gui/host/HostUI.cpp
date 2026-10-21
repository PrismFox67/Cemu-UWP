#include "gui/host/HostUI.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostControllers.h"
#include "gui/host/HostGamepad.h"
#include "gui/host/HostGraphicPacks.h"
#include "gui/host/HostIcons.h"
#include "gui/host/HostTools.h"
#include "gui/host/HostTitleInstaller.h"
#include "gui/host/HostTheme.h"

#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/Filesystem/FST/KeyCache.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Common/version.h"

#include <imgui.h>

#include <mutex>
#include <chrono>
#include <cmath>
#include <fstream>
#include <unordered_set>
#include <boost/algorithm/string.hpp>

namespace
{
	struct GameEntry
	{
		std::string name;
		TitleId titleId;
		uint16 version; // of the update if one is installed
		bool hasUpdate = false;
		bool hasDLC = false;
		fs::path path;
		std::string location;
	};

	struct Message
	{
		std::string title;
		std::string text;
	};

	ImGuiContext* s_context = nullptr;
	fs::path s_fontFile;
	float s_scale = 1.0f;
	float s_builtScale = 0.0f;
	ImFont* s_fontNormal = nullptr;
	ImFont* s_fontLarge = nullptr;

	std::mutex s_mutex; // protects everything below that is touched from other threads
	std::vector<Message> s_messages;
	double s_fps = 0.0;
	bool s_refreshRequested = true;

	std::vector<GameEntry> s_games;
	bool s_wasScanning = false;
	int s_selectedGame = 0;
	enum Tab
	{
		kTabGames,
		kTabGraphicPacks,
		kTabControllers,
		kTabSettings,
		kTabAbout,
		kTabCount,
	};
	int s_tab = kTabGames;
	char s_newPathBuffer[1024]{};
	std::vector<std::function<void()>> s_deferred; // run after the frame (picker callbacks etc.)
	std::optional<fs::path> s_pendingLaunch; // set by the file picker
	std::function<void*(uint64, const uint8*, int, int)> s_textureProvider;
	std::optional<Message> s_lastSessionProblem; // shown above the game list
	bool s_updateDismissed = false;
	// first launch: a setup screen (game folder, keys, graphic packs) instead of the tabs until it's finished once
	bool s_setupChecked = false;
	bool s_showSetup = false;

	// folders scanned for games without being added, when they exist (like danprice142/Cemu-UWP): the Xbox's Dev Mode
	// D:\DevelopmentFiles (which RetroArch users already use) and the usual layout on a USB drive E:
	void _AddDefaultScanPaths()
	{
		auto& configured = GetConfig().game_paths;
		for (const wchar_t* folder : { L"D:/DevelopmentFiles/Games/wiiu", L"D:/DevelopmentFiles/Games/WiiU", L"E:/Games/wiiu", L"E:/Games/WiiU", L"E:/WiiU/games" })
		{
			const fs::path path(folder);
			std::error_code ec;
			if (!fs::is_directory(path, ec))
				continue;
			const std::string utf8 = _pathToUtf8(path);
			if (std::any_of(configured.begin(), configured.end(), [&](const std::string& p) { return boost::iequals(p, utf8); }))
				continue;
			CafeTitleList::AddScanPath(path);
			cemuLog_log(LogType::Force, "Host: scanning the default game folder {}", utf8);
		}
	}

	fs::path _SetupMarkerPath()
	{
		return ActiveSettings::GetUserDataPath("setup_done.txt");
	}

	void _RebuildFonts()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();
		const float normalSize = std::round(26.0f * s_scale);
		const float largeSize = std::round(40.0f * s_scale);
		std::error_code ec;
		if (!s_fontFile.empty() && fs::exists(s_fontFile, ec))
		{
			const std::string path = _pathToUtf8(s_fontFile);
			s_fontNormal = io.Fonts->AddFontFromFileTTF(path.c_str(), normalSize);
			s_fontLarge = io.Fonts->AddFontFromFileTTF(path.c_str(), largeSize);
		}
		if (!s_fontNormal || !s_fontLarge)
		{
			// the embedded font is a bitmap font, scale it up instead
			ImFontConfig cfg;
			cfg.SizePixels = 13.0f * std::max(1.0f, std::round(2.0f * s_scale));
			s_fontNormal = io.Fonts->AddFontDefault(&cfg);
			cfg.SizePixels = 13.0f * std::max(1.0f, std::round(3.0f * s_scale));
			s_fontLarge = io.Fonts->AddFontDefault(&cfg);
		}
		s_builtScale = s_scale;
	}

	void _ApplyStyle()
	{
		ImGuiStyle style;
		ImGui::StyleColorsDark(&style);
		style.WindowRounding = 0.0f;
		style.FrameRounding = 6.0f;
		style.GrabRounding = 6.0f;
		style.TabRounding = 6.0f;
		style.FramePadding = ImVec2(14.0f, 10.0f);
		style.ItemSpacing = ImVec2(14.0f, 12.0f);
		style.WindowPadding = ImVec2(40.0f, 30.0f);
		style.ScrollbarSize = 22.0f;
		style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
		style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.36f, 0.62f, 0.55f);
		style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.44f, 0.76f, 0.80f);
		style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.50f, 0.86f, 1.00f);
		style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.95f, 0.75f, 0.20f, 1.00f);
		style.ScaleAllSizes(s_scale);
		ImGui::GetStyle() = style;
	}

	void _RefreshGameList()
	{
		std::vector<GameEntry> games;
		auto list = CafeTitleList::AcquireInternalList();
		for (TitleInfo* info : list)
		{
			if (!info || !info->IsValid())
				continue;
			const auto type = TitleIdParser(info->GetAppTitleId()).GetType();
			if (type != TitleIdParser::TITLE_TYPE::BASE_TITLE && type != TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO && type != TitleIdParser::TITLE_TYPE::HOMEBREW)
				continue;
			GameEntry e;
			e.titleId = info->GetAppTitleId();
			e.version = info->GetAppTitleVersion();
			e.name = info->GetMetaTitleName();
			if (e.name.empty())
				e.name = fmt::format("{:016x}", e.titleId);
			e.path = info->GetPath();
			e.location = info->GetPrintPath();
			// the same title can be found in several locations, keep the first
			if (std::none_of(games.begin(), games.end(), [&](const GameEntry& g) { return g.titleId == e.titleId; }))
				games.emplace_back(std::move(e));
		}
		CafeTitleList::ReleaseInternalList();
		// the update and DLC are found separately (GetGameInfo locks the list, so after releasing it)
		for (GameEntry& e : games)
		{
			GameInfo2 info = CafeTitleList::GetGameInfo(e.titleId);
			e.hasUpdate = info.HasUpdate();
			e.hasDLC = info.HasAOC();
			if (e.hasUpdate)
				e.version = info.GetVersion();
		}
		std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) { return boost::algorithm::ilexicographical_compare(a.name, b.name); });
		std::vector<TitleId> titleIds;
		for (const GameEntry& e : games)
			titleIds.emplace_back(e.titleId);
		HostIcons::Request(titleIds);
		s_games = std::move(games);
		s_selectedGame = std::clamp(s_selectedGame, 0, std::max(0, (int)s_games.size() - 1));
	}

	void _AddGamePath(const fs::path& path)
	{
		if (path.empty())
			return;
		auto& config = GetConfig();
		const std::string utf8 = _pathToUtf8(path);
		if (std::find(config.game_paths.begin(), config.game_paths.end(), utf8) != config.game_paths.end())
			return;
		config.game_paths.emplace_back(utf8);
		GetConfigHandle().Save();
		CafeTitleList::ClearScanPaths();
		for (auto& it : config.game_paths)
			CafeTitleList::AddScanPath(_utf8ToPath(it));
		_AddDefaultScanPaths();
		CafeTitleList::Refresh();
		HostUI::RequestGameListRefresh();
	}

	// the 32 digit disc keys in a keys.txt, with the same rules as KeyCache_Prepare: '#' or ';' starts a comment, spaces,
	// tabs, '-' and '_' are ignored
	std::vector<std::string> _ReadKeys(const fs::path& file)
	{
		std::vector<std::string> keys;
		std::ifstream in(file);
		std::string line;
		while (std::getline(in, line))
		{
			line = line.substr(0, line.find_first_of("#;"));
			std::erase_if(line, [](char c) { return c == ' ' || c == '\t' || c == '-' || c == '_' || c == '\r'; });
			if (line.size() == 32 && std::all_of(line.begin(), line.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; }))
			{
				boost::algorithm::to_lower(line);
				keys.emplace_back(line);
			}
		}
		return keys;
	}

	// looks for keys.txt in the game folders and the folders above them (a drive often has it next to the games folder)
	// and adds the keys Cemu doesn't have yet to its own keys.txt
	void _ImportKeys()
	{
		const fs::path target = ActiveSettings::GetUserDataPath("keys.txt");
		std::vector<fs::path> sources;
		std::error_code ec;
		for (const auto& gamePath : GetConfig().game_paths)
		{
			for (fs::path dir = _utf8ToPath(gamePath); !dir.empty(); dir = dir.parent_path())
			{
				const fs::path candidate = dir / "keys.txt";
				if (fs::is_regular_file(candidate, ec) && !fs::equivalent(candidate, target, ec) &&
					std::find(sources.begin(), sources.end(), candidate) == sources.end())
					sources.emplace_back(candidate);
				if (dir == dir.root_path() || dir.parent_path() == dir)
					break;
			}
		}
		if (sources.empty())
		{
			HostUI::PushMessage("No keys.txt found", "Put keys.txt in one of your game folders (or a folder above it, like the root of the drive) and try again.");
			return;
		}
		const std::vector<std::string> existingList = _ReadKeys(target);
		std::unordered_set<std::string> known(existingList.begin(), existingList.end());
		std::ofstream out(target, std::ios::app);
		if (!out)
		{
			HostUI::PushMessage("Couldn't import keys", fmt::format("Cemu can't write {}", _pathToUtf8(target)));
			return;
		}
		size_t added = 0, total = 0;
		for (const auto& source : sources)
		{
			bool wroteHeader = false;
			for (const auto& key : _ReadKeys(source))
			{
				total++;
				if (!known.insert(key).second)
					continue;
				if (!wroteHeader)
				{
					out << "\n# imported from " << _pathToUtf8(source) << "\n";
					wroteHeader = true;
				}
				out << key << "\n";
				added++;
			}
		}
		out.close();
		KeyCache_Reload();
		cemuLog_log(LogType::Force, "Host: imported {} new disc key(s) from {} keys.txt file(s)", added, sources.size());
		std::string from;
		for (const auto& source : sources)
			from += "\n" + _pathToUtf8(source);
		std::string summary;
		if (added == 0)
			summary = fmt::format("All {} key(s) were already imported, nothing to add.", total);
		else if (added == total)
			summary = fmt::format("Added {} key(s).", added);
		else
			summary = fmt::format("Added {} new key(s), {} were already imported.", added, total - added);
		HostUI::PushMessage("keys.txt imported", fmt::format("{}\n\nRead from:{}", summary, from));
		if (added > 0)
		{
			CafeTitleList::Refresh(); // disc images that couldn't be read without their key show up now
			HostUI::RequestGameListRefresh();
		}
	}

	void _RemoveGamePath(size_t index)
	{
		auto& config = GetConfig();
		if (index >= config.game_paths.size())
			return;
		config.game_paths.erase(config.game_paths.begin() + index);
		GetConfigHandle().Save();
		CafeTitleList::ClearScanPaths();
		for (auto& it : config.game_paths)
			CafeTitleList::AddScanPath(_utf8ToPath(it));
		_AddDefaultScanPaths();
		CafeTitleList::Refresh();
		HostUI::RequestGameListRefresh();
	}

	// "save logs" / "back up saves" buttons, disabled while one of them runs
	void _DrawToolButtons(bool logs, bool saves)
	{
		const bool busy = HostTools::IsBusy();
		ImGui::BeginDisabled(busy);
		if (logs && ImGui::Button("Save logs to USB"))
			s_deferred.emplace_back([]() { HostTools::StartSaveLogs(); });
		if (logs && saves)
			ImGui::SameLine();
		if (saves && ImGui::Button("Back up saves to USB"))
			s_deferred.emplace_back([]() { HostTools::StartBackupSaves(); });
		ImGui::EndDisabled();
		if (busy)
		{
			ImGui::SameLine();
			ImGui::TextUnformatted(HostTools::GetBusyText().c_str());
		}
	}

	// above the game list: the last session's crash and a newer build
	void _DrawNotices()
	{
		if (s_lastSessionProblem)
		{
			ImGui::PushFont(s_fontLarge);
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.45f, 1.0f), "%s", s_lastSessionProblem->title.c_str());
			ImGui::PopFont();
			ImGui::TextWrapped("%s", s_lastSessionProblem->text.c_str());
			ImGui::TextWrapped("To report it, save the logs to your USB drive and attach them to a bug report on GitHub.");
			_DrawToolButtons(true, false);
			ImGui::SameLine();
			if (ImGui::Button("Dismiss"))
				s_lastSessionProblem.reset();
			ImGui::Separator();
		}
		const std::string newer = HostTools::GetNewerBuild();
		if (!newer.empty() && !s_updateDismissed && s_tab != kTabGames)
		{
			ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "A newer build is available: %s", newer.c_str());
			ImGui::TextDisabled("Download cemu-uwp-x64.zip from %s   (View hides this)", HostTools::GetDownloadUrl());
			ImGui::SameLine();
			if (ImGui::SmallButton("Hide"))
				s_updateDismissed = true;
		}
	}

	// game carousel: one row of cover cards like a console dashboard (Dolphin UWP style). Left/right (D-pad, stick, arrow
	// keys) moves the selection, the row glides to it; A / Enter starts the game. ImGui's own navigation is off while the
	// carousel has the input, the cards aren't ImGui items
	bool s_carouselActive = false; // read at the start of the next frame
	float s_carouselScroll = 0.0f; // eased towards s_selectedGame
	std::chrono::steady_clock::time_point s_carouselLastFrame;

	bool _CarouselKey(ImGuiKey gamepad, ImGuiKey stick, ImGuiKey keyboard)
	{
		return ImGui::IsKeyPressed(gamepad, true) || ImGui::IsKeyPressed(stick, true) || ImGui::IsKeyPressed(keyboard, true);
	}

	// a Wii U case: spine on the left, the blue Wii U header band, the title's art filling the front (cropped from the
	// square icon until scraped covers exist), a soft shadow. Proportions of the real case (about 0.72:1)
	constexpr float kCaseAspect = 0.72f;

	void _DrawCard(ImDrawList* drawList, const GameEntry& game, ImVec2 min, float width, bool selected, float alpha)
	{
		const float height = width / kCaseAspect;
		const ImVec2 max(min.x + width, min.y + height);
		const float r = 4.0f * s_scale;
		const ImU32 a = (ImU32)(alpha * 255.0f) << IM_COL32_A_SHIFT;
		auto rgb = [&](int cr, int cg, int cb) { return IM_COL32(cr, cg, cb, 0) | a; };
		// shadow
		drawList->AddRectFilled(ImVec2(min.x + width * 0.04f, min.y + height * 0.03f), ImVec2(max.x + width * 0.05f, max.y + height * 0.03f), IM_COL32(0, 30, 70, (int)(60 * alpha)), r * 2.0f);
		// case
		drawList->AddRectFilled(min, max, rgb(232, 238, 245), r);
		const float spine = width * 0.07f;
		drawList->AddRectFilled(min, ImVec2(min.x + spine, max.y), rgb(196, 206, 220), r, ImDrawFlags_RoundCornersLeft);
		const ImVec2 frontMin(min.x + spine, min.y), frontMax = max;
		const float band = height * 0.085f;
		drawList->AddRectFilled(frontMin, ImVec2(frontMax.x, frontMin.y + band), rgb(0, 140, 215), r, ImDrawFlags_RoundCornersTopRight);
		{
			const char* label = "Wii U";
			const float size = band * 0.62f;
			const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
			drawList->AddText(ImGui::GetFont(), size, ImVec2(frontMin.x + (frontMax.x - frontMin.x - ts.x) * 0.5f, frontMin.y + (band - ts.y) * 0.5f), rgb(255, 255, 255), label);
		}
		const ImVec2 artMin(frontMin.x, frontMin.y + band), artMax(frontMax.x, frontMax.y - height * 0.03f);
		void* texture = nullptr;
		if (const HostIcons::Icon* icon = HostIcons::Get(game.titleId); icon && s_textureProvider)
			texture = s_textureProvider(game.titleId, icon->rgba.data(), icon->width, icon->height);
		if (texture)
		{
			// crop the square icon to the portrait front
			const float aspect = (artMax.x - artMin.x) / (artMax.y - artMin.y);
			const float crop = (1.0f - aspect) * 0.5f;
			drawList->AddImage(texture, artMin, artMax, ImVec2(crop, 0.0f), ImVec2(1.0f - crop, 1.0f), IM_COL32(255, 255, 255, 0) | a);
		}
		else
		{
			drawList->AddRectFilled(artMin, artMax, rgb(60, 70, 90));
			std::string initials;
			for (size_t i = 0; i < game.name.size() && initials.size() < 2; i++)
				if (std::isalnum((unsigned char)game.name[i]) && (i == 0 || game.name[i - 1] == ' '))
					initials += (char)std::toupper((unsigned char)game.name[i]);
			ImGui::PushFont(s_fontLarge);
			const ImVec2 ts = ImGui::CalcTextSize(initials.c_str());
			drawList->AddText(ImVec2((artMin.x + artMax.x - ts.x) * 0.5f, (artMin.y + artMax.y - ts.y) * 0.5f), rgb(200, 210, 225), initials.c_str());
			ImGui::PopFont();
		}
		// a light sheen on the plastic
		drawList->AddRectFilledMultiColor(frontMin, ImVec2(frontMin.x + (frontMax.x - frontMin.x) * 0.35f, frontMax.y), IM_COL32(255, 255, 255, (int)(40 * alpha)), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, (int)(40 * alpha)));
		if (selected)
			drawList->AddRect(ImVec2(min.x - 3.0f * s_scale, min.y - 3.0f * s_scale), ImVec2(max.x + 3.0f * s_scale, max.y + 3.0f * s_scale), IM_COL32(255, 255, 255, 255), r + 2.0f, 0, 3.5f * s_scale);
		// title under the case, two lines at most
		const float textW = width + 20.0f * s_scale;
		const ImVec4 clip(min.x - 10.0f * s_scale, max.y + 8.0f * s_scale, min.x - 10.0f * s_scale + textW, max.y + 8.0f * s_scale + ImGui::GetTextLineHeight() * 2.2f);
		drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.9f, ImVec2(clip.x, clip.y), (HostTheme::PanelText() & 0x00FFFFFF) | ((ImU32)((selected ? 1.0f : 0.8f) * alpha * 255.0f) << IM_COL32_A_SHIFT), game.name.c_str(), nullptr, textW, &clip);
	}

	std::optional<fs::path> _DrawGamesTab()
	{
		std::optional<fs::path> launch;
		// the crash notice has buttons that need ImGui's navigation, the update notice is hidden with View
		const bool notices = s_lastSessionProblem.has_value();
		if (!HostTools::GetNewerBuild().empty() && !s_updateDismissed && ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false))
			s_updateDismissed = true;
		_DrawNotices();
		if (CafeTitleList::IsScanning())
			ImGui::TextDisabled("Scanning game folders...");
		if (s_games.empty())
		{
			s_carouselActive = false;
			ImGui::TextWrapped("No games found.");
			ImGui::Spacing();
			ImGui::TextWrapped("Add a folder that contains your games (WUD, WUX, WUA, RPX or extracted folders) under Settings, or install titles into the mlc folder:");
			ImGui::TextWrapped("%s", _pathToUtf8(ActiveSettings::GetMlcPath()).c_str());
		}
		else
		{
			// the notices have buttons, ImGui's navigation reaches them only while the carousel lets go
			s_carouselActive = !notices;
			const int count = (int)s_games.size();
			s_selectedGame = std::clamp(s_selectedGame, 0, count - 1);
			if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
			{
				if (_CarouselKey(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow) && s_selectedGame > 0)
					s_selectedGame--;
				if (_CarouselKey(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow) && s_selectedGame < count - 1)
					s_selectedGame++;
				if (s_carouselActive && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)))
					launch = s_games[s_selectedGame].path;
				if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))
				{
					CafeTitleList::Refresh();
					HostUI::RequestGameListRefresh();
				}
			}

			// ease the row towards the selection (frame rate independent)
			const auto now = std::chrono::steady_clock::now();
			const float dt = std::clamp(std::chrono::duration<float>(now - s_carouselLastFrame).count(), 0.0f, 0.1f);
			s_carouselLastFrame = now;
			s_carouselScroll += ((float)s_selectedGame - s_carouselScroll) * (1.0f - std::exp(-dt * 12.0f));
			if (std::abs((float)s_selectedGame - s_carouselScroll) < 0.001f)
				s_carouselScroll = (float)s_selectedGame;

			ImVec2 area = ImGui::GetContentRegionAvail();
			if (!s_carouselActive)
				area.y -= ImGui::GetFrameHeightWithSpacing(); // the buttons below
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const float footer = ImGui::GetTextLineHeightWithSpacing() * 1.2f; // details of the selected game
			const float infoHeight = ImGui::GetTextLineHeightWithSpacing() * 2.6f; // titles under the cases
			// case size from the space, the selected one 20% larger
			const float cardWidth = std::min(area.x / 5.6f, (area.y - footer - infoHeight) * kCaseAspect / 1.22f);
			const float spacing = cardWidth * 0.18f;
			const float centerX = origin.x + area.x * 0.5f;
			const float rowCenterY = origin.y + (area.y - footer - infoHeight) * 0.5f + 6.0f * s_scale;
			for (int i = 0; i < count; i++)
			{
				const float offset = (float)i - s_carouselScroll;
				const float x = centerX + offset * (cardWidth + spacing);
				if (x + cardWidth < origin.x - cardWidth || x - cardWidth > origin.x + area.x + cardWidth)
					continue;
				const float closeness = std::max(0.0f, 1.0f - std::abs(offset)); // 1 for the selected card
				const float width = cardWidth * (1.0f + 0.2f * closeness);
				const float height = width / kCaseAspect;
				const float alpha = std::clamp(1.0f - std::abs(offset) * 0.12f, 0.45f, 1.0f);
				const ImVec2 min(x - width * 0.5f, rowCenterY - height * 0.5f);
				_DrawCard(drawList, s_games[i], min, width, i == s_selectedGame, alpha);
				// mouse/touch: a click selects, a click on the selected card starts it
				const ImVec2 max(min.x + width, min.y + height);
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(min, max) && ImGui::IsWindowHovered())
				{
					if (i == s_selectedGame)
						launch = s_games[i].path;
					s_selectedGame = i;
				}
			}

			// details of the selected game at the bottom of the panel
			const GameEntry& game = s_games[s_selectedGame];
			const std::string details = fmt::format("{}   {:016x}   v{}{}{}   {} / {}", game.name, game.titleId, game.version, game.hasUpdate ? " (update)" : "", game.hasDLC ? " + DLC" : "", s_selectedGame + 1, count);
			const ImVec2 detailsSize = ImGui::CalcTextSize(details.c_str());
			drawList->AddText(ImVec2(centerX - detailsSize.x * 0.5f, origin.y + area.y - footer), HostTheme::PanelTextDim(), details.c_str());
			ImGui::Dummy(area);
		}
		if (!s_carouselActive)
		{
			if (ImGui::Button("Open file..."))
			{
				HostPlatform::PickFile([](const fs::path& path) {
					if (!path.empty())
						s_pendingLaunch = path;
				});
			}
			ImGui::SameLine();
			if (ImGui::Button("Refresh"))
			{
				CafeTitleList::Refresh();
				HostUI::RequestGameListRefresh();
			}
		}
		return launch;
	}

	// enable state and presets of one pack. Changes are saved right away and apply when the game starts
	void _DrawGraphicPack(const std::shared_ptr<GraphicPack2>& pack)
	{
		bool enabled = pack->IsEnabled();
		if (ImGui::Checkbox(pack->GetVirtualPath().c_str(), &enabled))
		{
			pack->SetEnabled(enabled);
			HostGraphicPacks::SaveToConfig();
		}
		if (ImGui::IsItemHovered() && !pack->GetDescription().empty())
			ImGui::SetTooltip("%s", pack->GetDescription().c_str());
		if (!enabled || pack->GetPresets().empty())
			return;

		ImGui::Indent(40.0f * s_scale);
		std::vector<std::string> order;
		auto categories = pack->GetCategorizedPresets(order);
		for (const std::string& category : order)
		{
			const auto& presets = categories[category];
			if (std::none_of(presets.begin(), presets.end(), [](const GraphicPack2::PresetPtr& p) { return p->visible; }))
				continue;
			const std::string active = pack->GetActivePreset(category);
			ImGui::PushID(category.c_str());
			ImGui::SetNextItemWidth(520.0f * s_scale);
			const std::string label = category.empty() ? std::string("Preset") : category;
			if (ImGui::BeginCombo(label.c_str(), active.c_str()))
			{
				for (const auto& preset : presets)
				{
					if (!preset->visible)
						continue;
					if (ImGui::Selectable(preset->name.c_str(), preset->name == active))
					{
						// presets can hide or show other presets, the combos are rebuilt next frame
						pack->SetActivePreset(category, preset->name);
						HostGraphicPacks::SaveToConfig();
					}
					if (preset->name == active)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::PopID();
		}
		ImGui::Unindent(40.0f * s_scale);
	}

	void _DrawGraphicPacksTab()
	{
		const auto status = HostGraphicPacks::GetStatus();
		const bool busy = HostGraphicPacks::IsBusy();
		const std::string installed = HostGraphicPacks::GetInstalledVersion();
		ImGui::TextWrapped("Community graphic packs: %s", installed.empty() ? "not downloaded" : installed.c_str());
		ImGui::BeginDisabled(busy);
		if (ImGui::Button(installed.empty() ? "Download community graphic packs" : "Check for updates"))
			HostGraphicPacks::StartUpdate();
		ImGui::EndDisabled();
		if (status.state != HostGraphicPacks::State::Idle)
		{
			ImGui::SameLine();
			if (status.state == HostGraphicPacks::State::Failed)
				ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status.text.c_str());
			else
				ImGui::TextUnformatted(status.text.c_str());
			if (busy && status.progress >= 0.0f)
				ImGui::ProgressBar(status.progress, ImVec2(-1.0f, 0.0f));
		}

		if (s_games.empty())
		{
			ImGui::TextWrapped("Add a game first, the packs of the game selected under Games are listed here.");
			return;
		}
		const GameEntry& game = s_games[std::clamp(s_selectedGame, 0, (int)s_games.size() - 1)];
		std::vector<std::shared_ptr<GraphicPack2>> packs;
		for (const auto& pack : GraphicPack2::GetGraphicPacks())
		{
			if (pack->ContainsTitleId(game.titleId))
				packs.emplace_back(pack);
		}
		std::sort(packs.begin(), packs.end(), [](const auto& a, const auto& b) { return boost::algorithm::ilexicographical_compare(a->GetVirtualPath(), b->GetVirtualPath()); });

		ImGui::SeparatorText(fmt::format("{} ({} packs)", game.name, packs.size()).c_str());
		if (packs.empty())
		{
			ImGui::TextWrapped(installed.empty() ? "Download the community graphic packs to get resolution, FPS and mod packs for this game." : "There are no graphic packs for this game.");
			ImGui::TextWrapped("Own packs go into: %s", _pathToUtf8(ActiveSettings::GetUserDataPath("graphicPacks")).c_str());
			return;
		}
		ImGui::TextDisabled("Applied when the game starts. Resolution packs render the game at a higher resolution (e.g. 3840x2160 for 4K).");
		if (ImGui::BeginChild("##packs", ImVec2(0.0f, 0.0f), true))
		{
			for (size_t i = 0; i < packs.size(); i++)
			{
				ImGui::PushID((int)i);
				_DrawGraphicPack(packs[i]);
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	// the game folder list with Remove buttons, "Add folder..." and a path field (Settings and the setup screen)
	void _DrawGameFolderControls()
	{
		auto& config = GetConfig();
		for (size_t i = 0; i < config.game_paths.size(); i++)
		{
			ImGui::PushID((int)i);
			if (ImGui::Button("Remove"))
			{
				const size_t index = i;
				s_deferred.emplace_back([index]() { _RemoveGamePath(index); });
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(config.game_paths[i].c_str());
			ImGui::PopID();
		}
		if (ImGui::Button("Add folder..."))
		{
			HostPlatform::PickFolder([](const fs::path& path) {
				if (!path.empty())
					s_deferred.emplace_back([path]() { _AddGamePath(path); });
			});
		}
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
		const bool entered = ImGui::InputTextWithHint("##newpath", "or type a path, e.g. E:\\WiiU\\games", s_newPathBuffer, sizeof(s_newPathBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
		if (ImGui::IsItemActivated())
			HostPlatform::ShowTextInput(true);
		if (ImGui::IsItemDeactivated())
			HostPlatform::ShowTextInput(false);
		ImGui::SameLine();
		if ((ImGui::Button("Add path") || entered) && s_newPathBuffer[0])
		{
			const fs::path path = _utf8ToPath(s_newPathBuffer);
			s_newPathBuffer[0] = '\0';
			s_deferred.emplace_back([path]() { _AddGamePath(path); });
		}
	}

	void _FinishSetup()
	{
		std::ofstream(_SetupMarkerPath(), std::ios::trunc) << "first-time setup finished\n";
		s_showSetup = false;
		s_tab = kTabGames;
	}

	// first launch: the three things a new tester has to do before a game works well
	void _DrawSetup()
	{
		ImGui::PushFont(s_fontLarge);
		ImGui::TextUnformatted("Welcome! Three steps to get started");
		ImGui::PopFont();
		ImGui::TextDisabled("You can change all of this later under Settings and Graphic packs.");

		ImGui::SeparatorText("1. Your games");
		ImGui::TextWrapped("Add the folder or USB drive with your games (decrypted dumps, .wua, or .wud/.wux disc images). Keep updates and DLC "
			"as their own folders next to the game, they're found automatically.");
		_DrawGameFolderControls();
		if (CafeTitleList::IsScanning())
			ImGui::TextDisabled("Scanning game folders...");
		else if (!GetConfig().game_paths.empty())
			ImGui::TextColored(s_games.empty() ? ImVec4(1.0f, 0.6f, 0.45f, 1.0f) : ImVec4(0.4f, 0.85f, 0.4f, 1.0f), s_games.empty() ? "No games found in these folders yet" : "%d game(s) found", (int)s_games.size());

		ImGui::SeparatorText("2. Disc keys (only for .wud / .wux)");
		ImGui::TextWrapped("Disc images need their key. Put keys.txt in your game folder or at the root of the drive, then import it. Skip this for decrypted dumps.");
		if (ImGui::Button("Import keys.txt from game folders"))
			s_deferred.emplace_back([]() { _ImportKeys(); });

		ImGui::SeparatorText("3. Community graphic packs");
		ImGui::TextWrapped("Resolution packs (1440p / 4K) and fixes for games that crash without them, like New Super Mario Bros. U.");
		const auto status = HostGraphicPacks::GetStatus();
		const bool busy = HostGraphicPacks::IsBusy();
		const std::string installed = HostGraphicPacks::GetInstalledVersion();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button(installed.empty() ? "Download community graphic packs" : "Check for updates"))
			HostGraphicPacks::StartUpdate();
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (status.state == HostGraphicPacks::State::Failed)
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status.text.c_str());
		else if (status.state != HostGraphicPacks::State::Idle)
			ImGui::TextUnformatted(status.text.c_str());
		else if (!installed.empty())
			ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "Installed (%s)", installed.c_str());
		if (busy && status.progress >= 0.0f)
			ImGui::ProgressBar(status.progress, ImVec2(-1.0f, 0.0f));

		ImGui::Spacing();
		ImGui::Separator();
		if (ImGui::Button("Done"))
			s_deferred.emplace_back([]() { _FinishSetup(); });
		ImGui::SameLine();
		ImGui::TextDisabled("Tip: for Mario Kart 8 set Graphic packs > TV Resolution to 2560x1440.");
	}

	// install a game, update or DLC folder into the mlc (games that ask for a system or title update, e.g. BOTW)
	std::optional<HostTitleInstaller::InstallPlan> s_installPlan;
	std::string s_installError;
	bool s_installRefreshPending = false;

	void _DrawInstallSection()
	{
		using namespace HostTitleInstaller;
		const Snapshot snapshot = GetSnapshot();
		if (snapshot.state == State::Installing || snapshot.state == State::Cancelling)
		{
			const float progress = snapshot.requiredBytes ? (float)((double)snapshot.copiedBytes / (double)snapshot.requiredBytes) : 0.0f;
			ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), fmt::format("{} / {} files", snapshot.copiedFiles, snapshot.fileCount).c_str());
			ImGui::TextDisabled("%s", snapshot.currentFile.c_str());
			if (snapshot.state == State::Installing && ImGui::Button("Cancel install"))
				RequestCancel();
			s_installRefreshPending = true;
			return;
		}
		if (s_installRefreshPending)
		{
			// finished: the new content shows in the game list (update/DLC versions on the cards)
			s_installRefreshPending = false;
			CafeTitleList::Refresh();
			HostUI::RequestGameListRefresh();
		}
		if (snapshot.state != State::Idle && !snapshot.message.empty())
			ImGui::TextColored(snapshot.state == State::Succeeded ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f) : ImVec4(1.0f, 0.6f, 0.45f, 1.0f), "%s", snapshot.message.c_str());
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
		// CEMU_HOST_TEST_INSTALL=<folder>: as if that folder was picked (automated test on a PC, the dialog can't be driven)
		static bool s_testChecked = false;
		if (!s_testChecked)
		{
			s_testChecked = true;
			char* v = nullptr;
			size_t n = 0;
			if (_dupenv_s(&v, &n, "CEMU_HOST_TEST_INSTALL") == 0 && v)
			{
				PlanResult result = BuildPlan(_utf8ToPath(v), ActiveSettings::GetMlcPath());
				s_installPlan = result.plan;
				s_installError = result.error;
				free(v);
			}
		}
#endif
		if (!s_installPlan)
		{
			ImGui::TextWrapped("Copies a game, update or DLC folder (the one with code, content and meta in it) into Cemu's mlc folder, where games find their updates and DLC.");
			if (ImGui::Button("Install update / DLC from folder..."))
			{
				HostPlatform::PickFolder([](const fs::path& path) {
					if (path.empty())
						return;
					PlanResult result = BuildPlan(path, ActiveSettings::GetMlcPath());
					s_installPlan = result.plan;
					s_installError = result.error;
				});
			}
			if (!s_installError.empty())
				ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.45f, 1.0f), "%s", s_installError.c_str());
			return;
		}
		const InstallPlan& plan = *s_installPlan;
		ImGui::Text("%s: %s", std::string(ContentTypeName(plan.type)).c_str(), plan.titleName.c_str());
		ImGui::TextDisabled("Title %016llx  v%u  %.1f MB, %llu files", (unsigned long long)plan.titleId, plan.version, plan.requiredBytes / 1048576.0, (unsigned long long)plan.fileCount);
		const char* existing = nullptr;
		switch (plan.existingState)
		{
		case ExistingState::Upgrade: existing = "Replaces the installed older version"; break;
		case ExistingState::SameVersion: existing = "This version is already installed, it will be replaced"; break;
		case ExistingState::Downgrade: existing = "A NEWER version is installed, this replaces it with an older one"; break;
		case ExistingState::DifferentType: existing = "Something else is installed at this title ID, it will be replaced"; break;
		case ExistingState::Invalid: existing = "A damaged install is at this title ID, it will be replaced"; break;
		default: break;
		}
		if (existing)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.45f, 1.0f), "%s (installed: v%u)", existing, plan.existingVersion);
		if (plan.availableBytes && plan.availableBytes < plan.requiredBytes)
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.45f, 1.0f), "Not enough space: %.1f MB free", plan.availableBytes / 1048576.0);
		ImGui::TextDisabled("To %s", _pathToUtf8(plan.targetPath).c_str());
		if (ImGui::Button("Install"))
		{
			Start(plan);
			s_installPlan.reset();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			s_installPlan.reset();
	}

	void _DrawSettingsTab()
	{
		auto& config = GetConfig();
		bool changed = false;
		ImGui::SeparatorText("Game folders");
		_DrawGameFolderControls();

		ImGui::SeparatorText("Install updates and DLC");
		_DrawInstallSection();

		ImGui::SeparatorText("Look");
		{
			int panel = HostTheme::IsLightPanel() ? 0 : 1;
			if (ImGui::RadioButton("Light panel", panel == 0))
				HostTheme::SetLightPanel(true);
			ImGui::SameLine();
			if (ImGui::RadioButton("Dark panel", panel == 1))
				HostTheme::SetLightPanel(false);
			ImGui::TextDisabled("Background picture: put background.png or background.jpg into %s and restart Cemu", _pathToUtf8(ActiveSettings::GetUserDataPath("theme")).c_str());
		}

		ImGui::SeparatorText("Graphics");
		bool vsync = config.vsync.GetValue() != 0;
		if (ImGui::Checkbox("VSync", &vsync))
		{
			config.vsync = vsync ? 1 : 0;
			changed = true;
		}
		if (vsync)
		{
			bool interpolation = config.frame_interpolation.GetValue();
			ImGui::Indent(40.0f * s_scale);
			if (ImGui::Checkbox("Frame interpolation: 120 FPS from 60 (needs a 120 Hz TV, adds a little lag, can smear)", &interpolation))
			{
				config.frame_interpolation = interpolation;
				changed = true;
			}
			ImGui::Unindent(40.0f * s_scale);
		}
		bool asyncCompile = config.async_compile.GetValue();
		if (ImGui::Checkbox("Compile shaders asynchronously (less stutter, brief glitches)", &asyncCompile))
		{
			config.async_compile = asyncCompile;
			changed = true;
		}
		bool precompile = config.precompile_pipelines.GetValue();
		if (ImGui::Checkbox("Compile pipelines while the game loads (longer loading, less stutter)", &precompile))
		{
			config.precompile_pipelines = precompile;
			changed = true;
		}
		if (precompile)
		{
			bool background = config.precompile_in_background.GetValue();
			ImGui::Indent(40.0f * s_scale);
			// quick start: the loading screen only creates the pipelines earlier sessions needed for the menus and the start
			// of a race, the rest are created in the background
			if (ImGui::Checkbox("Quick start: load menu and race-start pipelines first, the rest in the background", &background))
			{
				config.precompile_in_background = background;
				changed = true;
			}
			ImGui::Unindent(40.0f * s_scale);
		}
		if (asyncCompile)
		{
			const char* waitModes[] = { "Smooth: never wait, new effects can pop in", "Balanced: short waits, rarely misses an effect", "Accurate: always wait, stutters" };
			int waitMode = std::clamp<int>(config.pipeline_wait.GetValue(), 0, 2);
			ImGui::SetNextItemWidth(620.0f * s_scale);
			if (ImGui::Combo("New shaders", &waitMode, waitModes, 3))
			{
				config.pipeline_wait = waitMode;
				changed = true;
			}
		}
		// DXIL (DXC) skips the driver's DXBC translation, which may make the Xbox's per session pipeline creation faster.
		// Each compiler has its own shader cache, so switching recompiles every shader once
#ifdef CEMU_UWP
		const char* compilers[] = { "FXC (slower game start)", "DXC (default, faster game start)" };
#else
		const char* compilers[] = { "FXC (default)", "DXC (experimental)" };
#endif
		int compiler = std::clamp<int>(config.d3d12_shader_compiler.GetValue(), 0, 1);
		ImGui::SetNextItemWidth(400.0f * s_scale);
		if (ImGui::Combo("Shader compiler (next game start)", &compiler, compilers, 2))
		{
			config.d3d12_shader_compiler = compiler;
			changed = true;
		}
		const char* filters[] = { "Bilinear", "Bicubic", "Hermite", "Nearest neighbor" };
		int upscale = std::clamp<int>(config.upscale_filter.GetValue(), 0, 3);
		ImGui::SetNextItemWidth(400.0f * s_scale);
		if (ImGui::Combo("Upscale filter", &upscale, filters, 4))
		{
			config.upscale_filter = upscale;
			changed = true;
		}
		const char* scalings[] = { "Keep aspect ratio", "Stretch" };
		int scaling = std::clamp<int>(config.fullscreen_scaling.GetValue(), 0, 1);
		ImGui::SetNextItemWidth(400.0f * s_scale);
		if (ImGui::Combo("Scaling", &scaling, scalings, 2))
		{
			config.fullscreen_scaling = scaling;
			changed = true;
		}
		bool showFPS = config.overlay.position != ScreenPosition::kDisabled && config.overlay.fps;
		if (ImGui::Checkbox("Show FPS", &showFPS))
		{
			config.overlay.position = showFPS ? ScreenPosition::kTopLeft : ScreenPosition::kDisabled;
			config.overlay.fps = showFPS;
			changed = true;
		}

		ImGui::SeparatorText("Audio");
		// games are told what the console outputs and mix for it (Mario Kart 8 mixes 5.1). On Xbox the console's own
		// audio setting decides what reaches the TV or receiver, a stereo setup gets a downmix
		const char* channelNames[] = { "Mono", "Stereo", "5.1 surround" };
		int channels = std::clamp<int>(config.tv_channels, 0, 2);
		ImGui::SetNextItemWidth(400.0f * s_scale);
		if (ImGui::Combo("Sound output (applies when a game starts)", &channels, channelNames, 3))
		{
			config.tv_channels = (AudioChannels)channels;
			changed = true;
		}
		int volume = std::clamp<int>(config.tv_volume, 0, 100);
		ImGui::SetNextItemWidth(400.0f * s_scale);
		if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%"))
		{
			config.tv_volume = volume;
			changed = true;
		}

		ImGui::SeparatorText("Folders");
		ImGui::TextWrapped("Data (settings, mlc01, shader caches, keys.txt): %s", _pathToUtf8(ActiveSettings::GetUserDataPath()).c_str());
		// WUD/WUX disc images need their disc key
		if (ImGui::Button("Import keys.txt from game folders"))
			s_deferred.emplace_back([]() { _ImportKeys(); });
		ImGui::SameLine();
		ImGui::TextDisabled("needed for WUD/WUX disc images");
		ImGui::TextWrapped("mlc01: %s", _pathToUtf8(ActiveSettings::GetMlcPath()).c_str());

		ImGui::SeparatorText("Logs and backups");
		const fs::path exportRoot = HostTools::GetExportRoot();
		ImGui::TextWrapped("Copies go to %s", exportRoot.empty() ? "the drive of your game folder (add one first)" : _pathToUtf8(exportRoot).c_str());
		_DrawToolButtons(true, true);
		if (ImGui::Button("Run first-time setup again"))
			s_showSetup = true;
		if (changed)
			GetConfigHandle().Save();
	}

	int s_mappingPlayer = -1; // player whose button mapping is shown in the Controllers tab

	void _DrawButtonMapping(int player, const HostControllers::PlayerInfo& info, float columnWidth)
	{
		using namespace HostControllers;
		ImGui::Indent();
		ImGui::TextDisabled("Select a Wii U button, then press the Xbox button for it on controller %d. The button that had it before gets the old one.", info.pad + 1);
		const bool capturingHere = IsCapturing() && GetCapturePlayer() == player;
		if (ImGui::BeginTable("##mapping", 2, ImGuiTableFlags_SizingFixedFit))
		{
			for (const auto& button : GetMappableButtons(info.kind))
			{
				ImGui::PushID((int)button.id);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(button.name);
				ImGui::TableNextColumn();
				std::string label;
				if (capturingHere && GetCaptureButton() == button.id)
					label = fmt::format("Press an Xbox button... ({:.0f})", std::ceil(GetCaptureSecondsLeft()));
				else
				{
					label = GetButtonMappingName(player, button.id);
					if (label.empty())
						label = "(not mapped)";
				}
				if (ImGui::Button(label.c_str(), ImVec2(columnWidth, 0.0f)) && !IsCapturing())
				{
					const uint64 id = button.id;
					if (!info.padConnected)
						HostUI::PushMessage("Controller not connected", fmt::format("Turn on controller {} first, the new button is pressed on it.", info.pad + 1));
					else
						s_deferred.emplace_back([player, id]() { BeginCapture(player, id); });
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (ImGui::Button("Nintendo layout (A<->B, X<->Y)"))
			s_deferred.emplace_back([player]() { UseNintendoLayout(player); });
		ImGui::SameLine();
		if (ImGui::Button("Reset to Xbox layout"))
			s_deferred.emplace_back([player]() { ResetButtonMapping(player); });
		// profile slots: the whole player setup, to keep a layout or move it to another player
		for (int slot = 0; slot < kProfileSlots; slot++)
		{
			ImGui::PushID(slot);
			if (ImGui::Button(fmt::format("Save to profile {}", slot + 1).c_str()))
				s_deferred.emplace_back([player, slot]() { SaveProfile(player, slot); });
			ImGui::SameLine();
			ImGui::BeginDisabled(!HasProfile(slot));
			if (ImGui::Button(fmt::format("Load profile {}", slot + 1).c_str()))
				s_deferred.emplace_back([player, slot]() { LoadProfile(player, slot); });
			ImGui::EndDisabled();
			if (slot + 1 < kProfileSlots)
				ImGui::SameLine(0.0f, 40.0f * s_scale);
			ImGui::PopID();
		}
		ImGui::Unindent();
	}

	void _DrawControllersTab()
	{
		using namespace HostControllers;
		if (IsPairing())
		{
			const int next = GetPairingPlayer();
			ImGui::PushFont(s_fontLarge);
			ImGui::Text("Press A on the controller for player %d", next + 1);
			ImGui::PopFont();
			ImGui::TextWrapped(next > 0 ? "Press Menu (Start) when everyone is paired. Players without a controller are disconnected." : "Each controller becomes the next player when its A button is pressed.");
		}
		else if (ImGui::Button("Pair controllers (press A on each)"))
			s_deferred.emplace_back([]() { BeginPairing(false); });
		ImGui::Spacing();

		const float columnWidth = 360.0f * s_scale;
		for (int player = 0; player < kPlayerCount; player++)
		{
			ImGui::PushID(player);
			const PlayerInfo info = GetPlayer(player);
			const bool pairedNow = IsPairing() && player == GetPairingPlayer();
			ImGui::SeparatorText(fmt::format("Player {}{}", player + 1, pairedNow ? "  < waiting for A" : "").c_str());
			ImGui::BeginDisabled(IsPairing());

			ImGui::SetNextItemWidth(columnWidth);
			if (ImGui::BeginCombo("##kind", KindName(info.kind)))
			{
				for (int k = 0; k < (int)Kind::Count; k++)
				{
					const Kind kind = (Kind)k;
					if (!IsKindAllowed(player, kind))
						continue;
					if (ImGui::Selectable(KindName(kind), kind == info.kind))
					{
						// a newly connected player without a controller gets the one with the same number
						const int pad = info.pad >= 0 ? info.pad : player;
						s_deferred.emplace_back([player, kind, pad]() { SetPlayer(player, kind, pad); });
					}
				}
				ImGui::EndCombo();
			}
			if (info.kind != Kind::None)
			{
				ImGui::SameLine();
				ImGui::SetNextItemWidth(columnWidth);
				const std::string padLabel = info.pad >= 0 ? fmt::format("Controller {}", info.pad + 1) : std::string("No controller");
				if (ImGui::BeginCombo("##pad", padLabel.c_str()))
				{
					for (int pad = -1; pad < HostGamepad::kMaxPads; pad++)
					{
						const bool connected = pad >= 0 && HostGamepad::PollPad(pad).connected;
						const std::string label = pad < 0 ? std::string("No controller") : fmt::format("Controller {}{}", pad + 1, connected ? "" : " (not connected)");
						if (ImGui::Selectable(label.c_str(), pad == info.pad))
						{
							const Kind kind = info.kind;
							s_deferred.emplace_back([player, kind, pad]() { SetPlayer(player, kind, pad); });
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				if (info.pad < 0)
					ImGui::TextDisabled(info.hasKeyboard ? "keyboard only" : "no input");
				else if (info.padConnected)
					ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), info.hasKeyboard ? "connected (+ keyboard)" : "connected");
				else
					ImGui::TextDisabled(info.hasKeyboard ? "turn on controller %d (keyboard works)" : "turn on controller %d", info.pad + 1);
			}
			if (info.kind != Kind::None && info.pad >= 0)
			{
				// stick drift: a worn stick that doesn't return to the center moves the player, a larger deadzone hides it.
				// The live readout shows how far each stick is off center so the deadzone can be set just above the drift
				const HostGamepad::State pad = HostGamepad::PollPad(info.pad);
				for (int stick = 0; stick < 2; stick++)
				{
					const char* stickName = stick == 0 ? "Left" : "Right";
					const int percent = (int)std::lround(info.deadzone[stick] * 100.0f);
					if (stick > 0)
						ImGui::SameLine();
					ImGui::PushID(stick);
					ImGui::SetNextItemWidth(columnWidth);
					if (ImGui::BeginCombo("##deadzone", fmt::format("{} stick deadzone {}%", stickName, percent).c_str()))
					{
						for (int p = 0; p <= 90; p += 5)
						{
							if (ImGui::Selectable(fmt::format("{}%{}", p, p == 15 ? " (default)" : p >= 25 ? " (stick drift)" : "").c_str(), p == percent))
								s_deferred.emplace_back([player, stick, p]() { SetDeadzone(player, stick, p / 100.0f); });
						}
						ImGui::EndCombo();
					}
					ImGui::PopID();
				}
				for (int stick = 0; stick < 2; stick++)
				{
					const float x = stick == 0 ? pad.rawLeftX : pad.rawRightX;
					const float y = stick == 0 ? pad.rawLeftY : pad.rawRightY;
					const float offCenter = std::min(1.0f, std::sqrt(x * x + y * y));
					const bool drifting = offCenter > info.deadzone[stick];
					if (stick > 0)
						ImGui::SameLine(0.0f, 40.0f * s_scale);
					const std::string text = fmt::format("{} stick now {}% off center{}", stick == 0 ? "Left" : "Right", (int)std::lround(offCenter * 100.0f), drifting ? " (moves)" : "");
					if (drifting)
						ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", text.c_str());
					else
						ImGui::TextDisabled("%s", text.c_str());
				}
				// worn sticks no longer reach the edge, a larger range gets full tilt earlier
				const int rangePercent = (int)std::lround(info.range * 100.0f);
				ImGui::SetNextItemWidth(columnWidth);
				if (ImGui::BeginCombo("##range", fmt::format("Stick range {}%", rangePercent).c_str()))
				{
					for (int p = 100; p <= 150; p += 10)
					{
						if (ImGui::Selectable(fmt::format("{}%{}", p, p == 100 ? " (default)" : " (full tilt earlier)").c_str(), p == rangePercent))
							s_deferred.emplace_back([player, p]() { SetStickRange(player, p / 100.0f); });
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				if (info.kind == Kind::GamePad)
				{
					// gyro from a phone app (DSU / cemuhook, e.g. "Motion Source" or "DSU Controller"), the Xbox pad has none
					static char s_motionIp[kPlayerCount][64]{};
					static bool s_motionIpLoaded[kPlayerCount]{};
					if (!s_motionIpLoaded[player])
					{
						s_motionIpLoaded[player] = true;
						strncpy(s_motionIp[player], GetMotionServer(player).c_str(), sizeof(s_motionIp[player]) - 1);
					}
					ImGui::SetNextItemWidth(columnWidth);
					const bool entered = ImGui::InputTextWithHint("##motionip", "Phone motion (DSU) IP", s_motionIp[player], sizeof(s_motionIp[player]), ImGuiInputTextFlags_EnterReturnsTrue);
					if (ImGui::IsItemActivated())
						HostPlatform::ShowTextInput(true);
					if (ImGui::IsItemDeactivated())
						HostPlatform::ShowTextInput(false);
					ImGui::SameLine();
					if (ImGui::Button("Use phone motion") || entered)
					{
						std::string ip = boost::trim_copy(std::string(s_motionIp[player]));
						s_deferred.emplace_back([player, ip]() { SetMotionServer(player, ip); });
					}
					ImGui::SameLine();
					if (GetMotionServer(player).empty())
						ImGui::TextDisabled("off (empty IP + button removes it)");
					else if (IsMotionServerConnected(player))
						ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "phone connected");
					else
						ImGui::TextDisabled("waiting for the phone app");
				}
				const bool open = s_mappingPlayer == player;
				if (ImGui::Button(open ? "Hide button mapping" : "Button mapping / Nintendo layout"))
					s_mappingPlayer = open ? -1 : player;
				if (s_mappingPlayer == player)
					_DrawButtonMapping(player, info, columnWidth);
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::Spacing();
		ImGui::TextDisabled("Buttons use the Xbox button with the same name (Xbox A = Wii U A). For the Nintendo button positions or own mappings open Button mapping.");
	}

	void _DrawAboutTab()
	{
		ImGui::TextUnformatted(BUILD_VERSION_WITH_NAME_STRING);
		ImGui::TextWrapped("Frontend: %s, renderer: Direct3D 12", HostPlatform::GetName());
		ImGui::Spacing();
		ImGui::TextWrapped("Controls");
		ImGui::BulletText("D-pad / left stick: move, A: select, B: back");
		ImGui::BulletText("In game: click both sticks (or hold View + Menu for one second) to open the menu, it has \"Exit to game list\"");
		ImGui::BulletText("Controller 1 is the Wii U GamePad, controllers 2-4 are Pro Controllers for players 2-4");
		ImGui::BulletText("Change this under Controllers, or pick \"Pair controllers\" in the in-game menu");
		ImGui::Spacing();
		ImGui::TextWrapped("Cemu is not affiliated with Nintendo. Wii U is a trademark of Nintendo. Only play games you own.");
	}
}

void HostUI::Initialize(ImGuiContext* context, const fs::path& fontFile, float uiScale)
{
	s_context = context;
	s_fontFile = fontFile;
	s_scale = std::max(0.5f, uiScale);
	HostTheme::Initialize();
	_AddDefaultScanPaths();
	CafeTitleList::Refresh();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
	_RebuildFonts();
	_ApplyStyle();
}

bool HostUI::SetScale(float uiScale)
{
	uiScale = std::max(0.5f, uiScale);
	if (std::abs(uiScale - s_builtScale) < 0.05f)
		return false;
	s_scale = uiScale;
	_RebuildFonts();
	_ApplyStyle();
	return true;
}

namespace
{
	void* _ProvideTexture(uint64 key, const uint8* rgba, int width, int height)
	{
		return s_textureProvider ? s_textureProvider(key, rgba, width, height) : nullptr;
	}

	// a child window inside the panel. framed: a dark rounded area for regular ImGui widgets (readable on the light panel)
	void _BeginPanelContent(ImVec2 min, ImVec2 max, float pad, bool framed)
	{
		ImGui::SetCursorScreenPos(ImVec2(min.x + pad, min.y));
		const ImVec2 size(max.x - min.x - pad * 2.0f, max.y - min.y - pad * 0.6f);
		ImGui::PushStyleColor(ImGuiCol_ChildBg, framed ? ImVec4(0.07f, 0.09f, 0.13f, 0.92f) : ImVec4(0, 0, 0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f * s_scale);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, framed ? ImVec2(16.0f * s_scale, 12.0f * s_scale) : ImVec2(0, 0));
		ImGui::BeginChild("##panelcontent", size, false, framed ? ImGuiWindowFlags_AlwaysUseWindowPadding : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();
	}

	float _ShoulderHintWidth(float h, float unit)
	{
		return h * 0.6f + 8.0f * unit + h * 1.4f;
	}

	// "< (LB)" or "(RB) >": an arrow and a rounded shoulder-button pill; returns the right edge
	float _DrawShoulderHint(ImDrawList* dl, ImVec2 min, float h, const char* label, bool left, float unit)
	{
		const ImU32 col = HostTheme::PanelText();
		const float arrowW = h * 0.6f;
		const float pillW = h * 1.4f;
		const float cy = min.y + h * 0.5f;
		float x = min.x;
		auto arrow = [&](float ax, bool pointLeft) {
			const float s = h * 0.28f;
			if (pointLeft)
				dl->AddTriangleFilled(ImVec2(ax, cy), ImVec2(ax + s, cy - s), ImVec2(ax + s, cy + s), col);
			else
				dl->AddTriangleFilled(ImVec2(ax + arrowW, cy), ImVec2(ax + arrowW - s, cy - s), ImVec2(ax + arrowW - s, cy + s), col);
			dl->AddRectFilled(ImVec2(pointLeft ? ax + s : ax, cy - s * 0.3f), ImVec2(pointLeft ? ax + arrowW : ax + arrowW - s, cy + s * 0.3f), col);
		};
		if (left)
		{
			arrow(x, true);
			x += arrowW + 8.0f * unit;
		}
		const ImVec2 pMin(x, min.y + h * 0.1f), pMax(x + pillW, min.y + h * 0.9f);
		dl->AddRect(pMin, pMax, col, h * 0.4f, 0, 2.0f * unit);
		const ImVec2 ts = ImGui::CalcTextSize(label);
		dl->AddText(ImVec2(x + (pillW - ts.x) * 0.5f, cy - ts.y * 0.5f), col, label);
		x += pillW;
		if (!left)
		{
			x += 8.0f * unit;
			arrow(x, false);
			x += arrowW;
		}
		return x;
	}

	float _HintPillWidth(const char* glyph, const char* text, float h, float unit)
	{
		const float glyphW = std::max(h * 0.72f, ImGui::CalcTextSize(glyph).x + 14.0f * unit);
		return 12.0f * unit + glyphW + 10.0f * unit + ImGui::CalcTextSize(text).x + 16.0f * unit;
	}

	// a translucent pill with a controller button glyph (a coloured circle for face buttons) and a label
	void _DrawHintPill(ImDrawList* dl, ImVec2 min, float h, const char* glyph, ImU32 glyphColor, const char* text, float unit)
	{
		const float w = _HintPillWidth(glyph, text, h, unit);
		dl->AddRectFilled(min, ImVec2(min.x + w, min.y + h), IM_COL32(20, 28, 40, 170), h * 0.5f);
		dl->AddRect(min, ImVec2(min.x + w, min.y + h), IM_COL32(255, 255, 255, 60), h * 0.5f, 0, 1.0f * unit);
		const float cy = min.y + h * 0.5f;
		const float glyphW = std::max(h * 0.72f, ImGui::CalcTextSize(glyph).x + 14.0f * unit);
		const float gx = min.x + 12.0f * unit;
		const ImVec2 gts = ImGui::CalcTextSize(glyph);
		if (glyphColor != 0)
		{
			dl->AddCircleFilled(ImVec2(gx + glyphW * 0.5f, cy), h * 0.36f, IM_COL32(15, 15, 18, 255), 24);
			dl->AddText(ImVec2(gx + (glyphW - gts.x) * 0.5f, cy - gts.y * 0.5f), glyphColor, glyph);
		}
		else
		{
			dl->AddRect(ImVec2(gx, cy - h * 0.32f), ImVec2(gx + glyphW, cy + h * 0.32f), IM_COL32(230, 235, 245, 255), h * 0.3f, 0, 1.5f * unit);
			dl->AddText(ImVec2(gx + (glyphW - gts.x) * 0.5f, cy - gts.y * 0.5f), IM_COL32(230, 235, 245, 255), glyph);
		}
		const ImVec2 ts = ImGui::CalcTextSize(text);
		dl->AddText(ImVec2(gx + glyphW + 10.0f * unit, cy - ts.y * 0.5f), IM_COL32(240, 244, 250, 255), text);
	}
}

std::optional<fs::path> HostUI::DrawLauncher()
{
	HostGraphicPacks::Update();
	{
		std::unique_lock _l(s_mutex);
		const bool scanning = CafeTitleList::IsScanning();
		if (s_refreshRequested || (s_wasScanning && !scanning))
		{
			s_refreshRequested = false;
			_l.unlock();
			_RefreshGameList();
		}
		s_wasScanning = scanning;
	}

	std::optional<fs::path> launch;
	{
		// the game carousel reads the D-pad and the arrow keys itself (s_carouselActive is from the last frame)
		ImGuiIO& io = ImGui::GetIO();
		const ImGuiConfigFlags nav = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
		if (s_carouselActive && s_tab == kTabGames && !s_showSetup)
			io.ConfigFlags &= ~nav;
		else
			io.ConfigFlags |= nav;
	}
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 screenMin = viewport->WorkPos;
	const ImVec2 screenMax(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y);
	HostTheme::DrawBackground(ImGui::GetBackgroundDrawList(), screenMin, screenMax, s_textureProvider ? &_ProvideTexture : nullptr);
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
	if (ImGui::Begin("##launcher", nullptr, flags))
	{
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float W = viewport->WorkSize.x, H = viewport->WorkSize.y;
		const float unit = H / 720.0f;

		// wordmark, top centre
		ImGui::PushFont(s_fontLarge);
		{
			const char* mark = "Cemu";
			const float size = ImGui::GetFontSize() * 1.6f;
			const ImVec2 ts = ImGui::CalcTextSize(mark);
			const ImVec2 pos(screenMin.x + (W - ts.x * 1.6f) * 0.5f, screenMin.y + 22.0f * unit);
			dl->AddText(ImGui::GetFont(), size, ImVec2(pos.x + 2.0f * unit, pos.y + 3.0f * unit), IM_COL32(0, 60, 120, 90), mark);
			dl->AddText(ImGui::GetFont(), size, pos, IM_COL32(255, 255, 255, 255), mark);
		}
		ImGui::PopFont();

		// update badge, top right
		const std::string newer = HostTools::GetNewerBuild();
		if (!newer.empty() && !s_updateDismissed)
		{
			const std::string badge = fmt::format("Update available: {}", newer);
			const ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
			const ImVec2 max(screenMax.x - 20.0f * unit, screenMin.y + 20.0f * unit + ts.y + 12.0f * unit);
			const ImVec2 min(max.x - ts.x - 24.0f * unit, screenMin.y + 20.0f * unit);
			dl->AddRectFilled(min, max, IM_COL32(20, 30, 45, 190), (max.y - min.y) * 0.5f);
			dl->AddText(ImVec2(min.x + 12.0f * unit, min.y + 6.0f * unit), IM_COL32(140, 210, 255, 255), badge.c_str());
			if (ImGui::IsMouseHoveringRect(min, max))
				ImGui::SetTooltip("Download cemu-uwp-x64.zip from %s\nView (Back) hides this", HostTools::GetDownloadUrl());
		}

		// the panel
		const ImVec2 panelMin(screenMin.x + W * 0.04f, screenMin.y + H * 0.16f);
		const ImVec2 panelMax(screenMax.x - W * 0.04f, screenMin.y + H * 0.84f);
		const float panelRounding = 22.0f * unit;
		dl->AddRectFilled(ImVec2(panelMin.x + 4.0f * unit, panelMin.y + 8.0f * unit), ImVec2(panelMax.x + 4.0f * unit, panelMax.y + 8.0f * unit), IM_COL32(0, 40, 90, 40), panelRounding);
		dl->AddRectFilled(panelMin, panelMax, HostTheme::PanelFill(), panelRounding);
		dl->AddRect(panelMin, panelMax, HostTheme::PanelBorder(), panelRounding, 0, 1.5f * unit);

		// the setup screen is shown until it's finished once. Existing setups (games and packs present) skip it
		if (!s_setupChecked)
		{
			s_setupChecked = true;
			std::error_code ec;
			if (!fs::exists(_SetupMarkerPath(), ec))
			{
				if (GetConfig().game_paths.empty() || HostGraphicPacks::GetInstalledVersion().empty())
					s_showSetup = true;
				else
					_FinishSetup();
			}
		}
		const float pad = 26.0f * unit;
		if (s_showSetup && !HostControllers::IsPairing())
		{
			_BeginPanelContent(panelMin, panelMax, pad, true);
			_DrawSetup();
			ImGui::EndChild();
		}
		else
		{
			// LB/RB switch tabs (not while pairing, every button press belongs to the pairing then)
			const bool pairing = HostControllers::IsPairing();
			if (!pairing && (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false)))
				s_tab = (s_tab + kTabCount - 1) % kTabCount;
			if (!pairing && (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)))
				s_tab = (s_tab + 1) % kTabCount;
			if (!pairing && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
				s_tab = kTabSettings;
			if (pairing)
				s_tab = kTabControllers;
			if (!pairing && !newer.empty() && ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false))
				s_updateDismissed = true;

			// category bar: < [LB] previous   CURRENT   next [RB] >
			const char* tabNames[kTabCount] = { "Games", "Graphic packs", "Controllers", "Settings", "About" };
			const float barY = panelMin.y + pad;
			const float barH = 34.0f * unit;
			const int prev = (s_tab + kTabCount - 1) % kTabCount, next = (s_tab + 1) % kTabCount;
			{
				ImGui::PushFont(s_fontLarge);
				const ImVec2 ts = ImGui::CalcTextSize(tabNames[s_tab]);
				dl->AddText(ImVec2(screenMin.x + (W - ts.x) * 0.5f, barY + (barH - ts.y) * 0.5f), HostTheme::PanelText(), tabNames[s_tab]);
				ImGui::PopFont();
				// left: arrow + LB pill + name, right: name + RB pill + arrow
				const ImVec2 lMin(panelMin.x + pad, barY);
				const float x1 = _DrawShoulderHint(dl, lMin, barH, "LB", true, unit);
				const ImVec2 pts = ImGui::CalcTextSize(tabNames[prev]);
				dl->AddText(ImVec2(x1 + 14.0f * unit, barY + (barH - pts.y) * 0.5f), HostTheme::PanelTextDim(), tabNames[prev]);
				const ImVec2 nts = ImGui::CalcTextSize(tabNames[next]);
				const float rightPillW = _ShoulderHintWidth(barH, unit);
				const ImVec2 rMin(panelMax.x - pad - rightPillW, barY);
				_DrawShoulderHint(dl, rMin, barH, "RB", false, unit);
				dl->AddText(ImVec2(rMin.x - 14.0f * unit - nts.x, barY + (barH - nts.y) * 0.5f), HostTheme::PanelTextDim(), tabNames[next]);
				// mouse/touch on the two sides
				if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					if (ImGui::IsMouseHoveringRect(lMin, ImVec2(x1 + 14.0f * unit + pts.x, barY + barH)))
						s_tab = prev;
					else if (ImGui::IsMouseHoveringRect(ImVec2(rMin.x - 14.0f * unit - nts.x, barY), ImVec2(panelMax.x - pad, barY + barH)))
						s_tab = next;
				}
			}

			const ImVec2 contentMin(panelMin.x, barY + barH + 10.0f * unit);
			if (s_tab == kTabGames)
			{
				_BeginPanelContent(contentMin, panelMax, pad * 0.5f, false);
				launch = _DrawGamesTab();
				ImGui::EndChild();
			}
			else
			{
				_BeginPanelContent(contentMin, panelMax, pad, true);
				if (s_tab == kTabGraphicPacks)
					_DrawGraphicPacksTab();
				else if (s_tab == kTabControllers)
					_DrawControllersTab();
				else if (s_tab == kTabSettings)
					_DrawSettingsTab();
				else
					_DrawAboutTab();
				ImGui::EndChild();
			}
		}

		// hint pills along the bottom
		{
			const float pillH = 40.0f * unit;
			const float y = screenMax.y - 24.0f * unit - pillH;
			struct Hint { const char* glyph; ImU32 color; const char* text; };
			std::vector<Hint> hints;
			if (s_tab == kTabGames && !s_showSetup)
				hints = { { "A", IM_COL32(40, 170, 60, 255), "Start" }, { "X", IM_COL32(30, 110, 220, 255), "Refresh" }, { "L3+R3", 0, "In-Game Menu" }, { "Y", IM_COL32(220, 170, 20, 255), "Settings" } };
			else
				hints = { { "A", IM_COL32(40, 170, 60, 255), "Select" }, { "B", IM_COL32(210, 50, 50, 255), "Back" }, { "LB/RB", 0, "Switch" } };
			float total = 0.0f;
			std::vector<float> widths;
			for (const Hint& hint : hints)
			{
				const float w = _HintPillWidth(hint.glyph, hint.text, pillH, unit);
				widths.push_back(w);
				total += w;
			}
			total += 16.0f * unit * (hints.size() - 1);
			float x = screenMin.x + (W - total) * 0.5f;
			for (size_t i = 0; i < hints.size(); i++)
			{
				_DrawHintPill(dl, ImVec2(x, y), pillH, hints[i].glyph, hints[i].color, hints[i].text, unit);
				x += widths[i] + 16.0f * unit;
			}
		}

		// messages
		{
			std::unique_lock _l(s_mutex);
			if (!s_messages.empty())
			{
				const Message msg = s_messages.front();
				_l.unlock();
				if (!ImGui::IsPopupOpen("##message"))
					ImGui::OpenPopup("##message");
				ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.6f, 0.0f));
				if (ImGui::BeginPopupModal("##message", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
				{
					ImGui::PushFont(s_fontLarge);
					ImGui::TextUnformatted(msg.title.c_str());
					ImGui::PopFont();
					ImGui::TextWrapped("%s", msg.text.c_str());
					ImGui::Spacing();
					if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))
					{
						std::unique_lock _l2(s_mutex);
						if (!s_messages.empty())
							s_messages.erase(s_messages.begin());
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
			}
		}
	}
	ImGui::End();

	auto deferred = std::move(s_deferred);
	s_deferred.clear();
	for (auto& fn : deferred)
		fn();
	if (!launch && s_pendingLaunch)
	{
		launch = s_pendingLaunch;
		s_pendingLaunch.reset();
	}
	return launch;
}

void HostUI::PushMessage(std::string title, std::string text)
{
	std::unique_lock _l(s_mutex);
	s_messages.emplace_back(Message{ std::move(title), std::move(text) });
}

void HostUI::SetStatus(bool /*isIdle*/, bool /*isLoading*/, double fps)
{
	std::unique_lock _l(s_mutex);
	s_fps = fps;
}

double HostUI::GetFPS()
{
	std::unique_lock _l(s_mutex);
	return s_fps;
}

void HostUI::RequestGameListRefresh()
{
	std::unique_lock _l(s_mutex);
	s_refreshRequested = true;
}

bool HostUI::HandleBack()
{
	if (s_tab != 0)
	{
		s_tab = 0;
		return true;
	}
	return false;
}

void HostUI::SetLastSessionProblem(std::string title, std::string text)
{
	s_lastSessionProblem = Message{ std::move(title), std::move(text) };
}

void HostUI::SetTextureProvider(std::function<void*(uint64 key, const uint8* rgba, int width, int height)> provider)
{
	s_textureProvider = std::move(provider);
}
