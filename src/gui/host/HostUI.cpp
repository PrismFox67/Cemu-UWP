#include "gui/host/HostUI.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostControllers.h"
#include "gui/host/HostGamepad.h"
#include "gui/host/HostGraphicPacks.h"
#include "gui/host/HostIcons.h"
#include "gui/host/HostTools.h"

#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/Filesystem/FST/KeyCache.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Common/version.h"

#include <imgui.h>

#include <mutex>
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
		if (!newer.empty() && !s_updateDismissed)
		{
			ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "A newer build is available: %s", newer.c_str());
			ImGui::TextDisabled("Download cemu-uwp-x64.zip from %s", HostTools::GetDownloadUrl());
			ImGui::SameLine();
			if (ImGui::SmallButton("Hide"))
				s_updateDismissed = true;
		}
	}

	std::optional<fs::path> _DrawGamesTab()
	{
		std::optional<fs::path> launch;
		_DrawNotices();
		if (CafeTitleList::IsScanning())
			ImGui::TextDisabled("Scanning game folders...");
		if (s_games.empty())
		{
			ImGui::TextWrapped("No games found.");
			ImGui::Spacing();
			ImGui::TextWrapped("Add a folder that contains your games (WUD, WUX, WUA, RPX or extracted folders) under Settings, or install titles into the mlc folder:");
			ImGui::TextWrapped("%s", _pathToUtf8(ActiveSettings::GetMlcPath()).c_str());
		}
		else
		{
			const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f;
			if (ImGui::BeginChild("##gamelist", ImVec2(0.0f, -footer), true))
			{
				const float rowHeight = ImGui::GetTextLineHeight() * 2.8f;
				const float iconSize = rowHeight;
				const float textX = iconSize + ImGui::GetStyle().ItemSpacing.x;
				for (int i = 0; i < (int)s_games.size(); i++)
				{
					const GameEntry& game = s_games[i];
					ImGui::PushID(i);
					const bool selected = i == s_selectedGame;
					const ImVec2 rowStart = ImGui::GetCursorScreenPos();
					const bool activated = ImGui::Selectable("##game", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowHeight));
					// the game's own icon (box art) left of its name, a placeholder until it's loaded
					ImDrawList* drawList = ImGui::GetWindowDrawList();
					const ImVec2 iconMin = rowStart;
					const ImVec2 iconMax(rowStart.x + iconSize, rowStart.y + iconSize);
					void* texture = nullptr;
					if (const HostIcons::Icon* icon = HostIcons::Get(game.titleId); icon && s_textureProvider)
						texture = s_textureProvider(game.titleId, icon->rgba.data(), icon->width, icon->height);
					if (texture)
						drawList->AddImageRounded(texture, iconMin, iconMax, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8.0f * s_scale);
					else
						drawList->AddRectFilled(iconMin, iconMax, IM_COL32(40, 44, 52, 255), 8.0f * s_scale);
					const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
					const float textY = rowStart.y + (rowHeight - lineHeight * 2.0f) * 0.5f;
					drawList->AddText(ImVec2(rowStart.x + textX, textY), ImGui::GetColorU32(ImGuiCol_Text), game.name.c_str());
					const std::string details = fmt::format("{:016x}  v{}{}{}", game.titleId, game.version, game.hasUpdate ? " (update)" : "", game.hasDLC ? " + DLC" : "");
					drawList->AddText(ImVec2(rowStart.x + textX, textY + lineHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled), details.c_str());
					if (activated)
					{
						s_selectedGame = i;
						launch = game.path;
					}
					if (ImGui::IsItemFocused())
						s_selectedGame = i;
					if (ImGui::IsItemHovered() && !game.location.empty())
						ImGui::SetTooltip("%s", game.location.c_str());
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
		}
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
		ImGui::SameLine();
		ImGui::TextDisabled("A: start   Y: settings");
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

	void _DrawSettingsTab()
	{
		auto& config = GetConfig();
		bool changed = false;
		ImGui::SeparatorText("Game folders");
		_DrawGameFolderControls();

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
		if (ImGui::Button("Reset buttons to default"))
			s_deferred.emplace_back([player]() { ResetButtonMapping(player); });
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
				// stick drift: a worn stick that doesn't return to the center moves the player, a larger deadzone hides it
				const int percent = (int)std::lround(info.deadzone * 100.0f);
				ImGui::SetNextItemWidth(columnWidth);
				if (ImGui::BeginCombo("##deadzone", fmt::format("Stick deadzone {}%", percent).c_str()))
				{
					for (int p = 0; p <= 50; p += 5)
					{
						if (ImGui::Selectable(fmt::format("{}%{}", p, p == 15 ? " (default)" : p >= 25 ? " (stick drift)" : "").c_str(), p == percent))
							s_deferred.emplace_back([player, p]() { SetDeadzone(player, p / 100.0f); });
					}
					ImGui::EndCombo();
				}
				// worn sticks no longer reach the edge, a larger range gets full tilt earlier
				ImGui::SameLine();
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
				const bool open = s_mappingPlayer == player;
				if (ImGui::Button(open ? "Hide button mapping" : "Button mapping"))
					s_mappingPlayer = open ? -1 : player;
				if (s_mappingPlayer == player)
					_DrawButtonMapping(player, info, columnWidth);
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::Spacing();
		ImGui::TextDisabled("Buttons use the Xbox button with the same name (Xbox A = Wii U A) unless changed under Button mapping.");
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
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	if (ImGui::Begin("##launcher", nullptr, flags))
	{
		ImGui::PushFont(s_fontLarge);
		ImGui::TextUnformatted("Cemu");
		ImGui::PopFont();
		ImGui::SameLine();
		ImGui::TextDisabled("%s  |  %s", BUILD_VERSION_WITH_NAME_STRING, HostPlatform::GetName());

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
		if (s_showSetup && !HostControllers::IsPairing())
		{
			ImGui::Separator();
			_DrawSetup();
		}
		else
		{
			// LB/RB switch tabs (not while pairing, every button press belongs to the pairing then)
			const bool pairing = HostControllers::IsPairing();
			if (!pairing && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
				s_tab = (s_tab + kTabCount - 1) % kTabCount;
			if (!pairing && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
				s_tab = (s_tab + 1) % kTabCount;
			if (!pairing && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
				s_tab = kTabSettings;
			if (pairing)
				s_tab = kTabControllers;

			const char* tabNames[kTabCount] = { "Games", "Graphic packs", "Controllers", "Settings", "About" };
			for (int i = 0; i < kTabCount; i++)
			{
				if (i > 0)
					ImGui::SameLine();
				const bool active = s_tab == i;
				if (active)
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
				if (ImGui::Button(tabNames[i]))
					s_tab = i;
				if (active)
					ImGui::PopStyleColor();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(LB / RB)");
			ImGui::Separator();

			if (s_tab == kTabGames)
				launch = _DrawGamesTab();
			else if (s_tab == kTabGraphicPacks)
				_DrawGraphicPacksTab();
			else if (s_tab == kTabControllers)
				_DrawControllersTab();
			else if (s_tab == kTabSettings)
				_DrawSettingsTab();
			else
				_DrawAboutTab();
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
					if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
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
