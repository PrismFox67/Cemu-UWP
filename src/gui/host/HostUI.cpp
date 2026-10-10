#include "gui/host/HostUI.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostControllers.h"
#include "gui/host/HostGamepad.h"
#include "gui/host/HostGraphicPacks.h"

#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleId.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Common/version.h"

#include <imgui.h>

#include <mutex>

namespace
{
	struct GameEntry
	{
		std::string name;
		TitleId titleId;
		uint16 version;
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
		std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) { return boost::algorithm::ilexicographical_compare(a.name, b.name); });
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

	std::optional<fs::path> _DrawGamesTab()
	{
		std::optional<fs::path> launch;
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
				for (int i = 0; i < (int)s_games.size(); i++)
				{
					const GameEntry& game = s_games[i];
					ImGui::PushID(i);
					const bool selected = i == s_selectedGame;
					const std::string label = fmt::format("{}\n{:016x}  v{}", game.name, game.titleId, game.version);
					if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, ImGui::GetTextLineHeight() * 2.4f)))
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

	void _DrawSettingsTab()
	{
		auto& config = GetConfig();
		bool changed = false;
		ImGui::SeparatorText("Game folders");
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

		ImGui::SeparatorText("Graphics");
		bool vsync = config.vsync.GetValue() != 0;
		if (ImGui::Checkbox("VSync", &vsync))
		{
			config.vsync = vsync ? 1 : 0;
			changed = true;
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

		ImGui::SeparatorText("Folders");
		ImGui::TextWrapped("Data (settings, mlc01, shader caches, keys.txt): %s", _pathToUtf8(ActiveSettings::GetUserDataPath()).c_str());
		ImGui::TextWrapped("mlc01: %s", _pathToUtf8(ActiveSettings::GetMlcPath()).c_str());
		if (changed)
			GetConfigHandle().Save();
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
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::Spacing();
		ImGui::TextDisabled("Buttons use the default layout of each controller type (Xbox A = Wii U A, positions as printed).");
	}

	void _DrawAboutTab()
	{
		ImGui::TextUnformatted(BUILD_VERSION_WITH_NAME_STRING);
		ImGui::TextWrapped("Frontend: %s, renderer: Direct3D 12", HostPlatform::GetName());
		ImGui::Spacing();
		ImGui::TextWrapped("Controls");
		ImGui::BulletText("D-pad / left stick: move, A: select, B: back");
		ImGui::BulletText("In game: hold View + Menu for one second to open the menu");
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
