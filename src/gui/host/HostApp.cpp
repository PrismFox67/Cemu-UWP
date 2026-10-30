#include "gui/host/HostApp.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostUI.h"
#include "gui/host/HostGamepad.h"
#include "gui/host/HostControllers.h"
#include "gui/host/HostIcons.h"
#include "gui/host/HostMusic.h"
#include "gui/host/HostSounds.h"
#include "gui/host/HostTools.h"
#include "gui/host/LauncherRenderer.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/nn_nfp/nn_nfp.h"
#include "Cafe/OS/libs/nsyshid/Skylander.h"
#include "Common/FileStream.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/NetworkSettings.h"
#include "input/InputManager.h"
#include "input/api/XInput/XInputController.h"
#include "input/api/Keyboard/KeyboardController.h"
#include "input/emulated/VPADController.h"
#include "audio/IAudioAPI.h"

#include <imgui.h>
#include "imgui/imgui_extension.h"

#include <Windows.h>

// main.cpp
void CemuCommonInit();

namespace
{
	enum class State
	{
		Launcher,
		Running,
		Quitting,
	};

	enum MenuItem
	{
		kMenuResume,
		kMenuPairControllers,
		kMenuGamePadScreen,
		kMenuGamePadOverlay,
		kMenuOverlayCorner,
		kMenuOverlaySize,
		kMenuOverlayOpacity,
		kMenuTouchCursor,
		kMenuShowFPS,
		kMenuVSync,
		kMenuVolume,
		kMenuDeadzone,
		kMenuStickTilt,
		kMenuAmiibo,
		kMenuSkylander,
		kMenuExitGame,
		kMenuExit,
		kMenuCount,
	};

	HostApp::InitOptions s_options;
	State s_state = State::Launcher;
	ImGuiContext* s_launcherContext = nullptr;
	std::unique_ptr<LauncherRenderer> s_launcher;
	bool s_launcherFailed = false;
	std::optional<fs::path> s_pendingLaunch;

	void _EnableSkylanderPortal(); // in-game menu section below

	// in-game menu, read by the GPU thread
	std::atomic_bool s_menuOpen = false;
	std::atomic_int s_menuSelection = 0;
	// "Menu: click both sticks" is shown for a few seconds when the first game of the session starts (issue #4: the menu
	// wasn't found)
	std::atomic_bool s_menuHintPending = false; // set when the game starts, the GPU thread starts the timer on its first in-game frame
	std::chrono::steady_clock::time_point s_menuHintUntil{};
	bool s_menuHintShown = false;
	uint32 s_lastPadButtons = 0;
	// touch cursor: stands in for the GamePad's touch screen (Super Mario Maker can't be played without it). The right
	// stick moves it over the GamePad screen, RT touches. Position is 0..1 of the GamePad image, drawn by the GPU thread
	std::atomic_bool s_touchCursor = false;
	std::atomic_bool s_stickTilt = false; // the right stick tilts the GamePad (motion controls), see _UpdateStickTilt
	std::atomic<float> s_touchX = 0.5f, s_touchY = 0.5f;
	std::atomic_bool s_touchDown = false;
	bool s_touchRestoreTVScreen = false;
	std::chrono::steady_clock::time_point s_touchLastUpdate{};
	std::chrono::steady_clock::time_point s_comboStart{};
	bool s_comboActive = false;
	bool s_comboConsumed = false;
	bool s_exitGameRequested = false; // "Exit to game list", done at the start of the next frame
	bool s_launcherIgnoresPad = false; // until all buttons are released, so the A that picked "Exit" doesn't start the game again

	// GamePad overlay (picture-in-picture of the other screen), remembered per title in gamepad_overlay.txt as lines
	// "<title id hex> <on 0/1> <corner 0-3> <size 0-2> <opacity %>". Titles never set keep it off
	fs::path _OverlaySettingsPath()
	{
		return ActiveSettings::GetUserDataPath("gamepad_overlay.txt");
	}

	LatteScreenPiP _LoadOverlaySettings(TitleId titleId)
	{
		LatteScreenPiP pip;
		std::ifstream in(_OverlaySettingsPath());
		std::string line;
		while (std::getline(in, line))
		{
			unsigned long long id = 0;
			int on = 0, corner = 3, size = 0, opacity = 100;
			if (sscanf(line.c_str(), "%llx %d %d %d %d", &id, &on, &corner, &size, &opacity) == 5 && id == titleId)
			{
				pip.enabled = on != 0;
				pip.corner = std::clamp(corner, 0, 3);
				pip.size = std::clamp(size, 0, 2);
				pip.opacity = std::clamp(opacity, 10, 100) / 100.0f;
			}
		}
		return pip;
	}

	void _SaveOverlaySettings(TitleId titleId, const LatteScreenPiP& pip)
	{
		std::vector<std::string> lines;
		{
			std::ifstream in(_OverlaySettingsPath());
			std::string line;
			while (std::getline(in, line))
			{
				unsigned long long id = 0;
				if (sscanf(line.c_str(), "%llx", &id) == 1 && id != titleId)
					lines.push_back(line);
			}
		}
		lines.push_back(fmt::format("{:016x} {} {} {} {}", titleId, pip.enabled ? 1 : 0, pip.corner, pip.size, (int)std::lround(pip.opacity * 100.0f)));
		std::ofstream out(_OverlaySettingsPath(), std::ios::trunc);
		for (const auto& l : lines)
			out << l << '\n';
	}

	void _ChangeOverlay(int item, int direction)
	{
		LatteScreenPiP pip = LatteOverlay_getScreenPiP();
		switch (item)
		{
		case kMenuGamePadOverlay:
			pip.enabled = !pip.enabled;
			break;
		case kMenuOverlayCorner:
			pip.corner = (pip.corner + 4 + direction) % 4;
			break;
		case kMenuOverlaySize:
			pip.size = std::clamp(pip.size + direction, 0, 2);
			break;
		case kMenuOverlayOpacity:
			pip.opacity = std::clamp(std::round(pip.opacity * 10.0f + direction) / 10.0f, 0.3f, 1.0f);
			break;
		}
		LatteOverlay_setScreenPiP(pip);
		_SaveOverlaySettings(CafeSystem::GetForegroundTitleId(), pip);
	}

	WindowSystem::WindowInfo& _WindowInfo()
	{
		return WindowSystem::GetWindowInfo();
	}

	float _UIScale()
	{
		// 1.0 at 1080p, follow the window height
		const int h = _WindowInfo().phys_height;
		return h > 0 ? std::clamp((float)h / 1080.0f, 0.5f, 2.0f) : 1.0f;
	}

	bool _CreateDefaultMLCFiles(const fs::path& mlc)
	{
		std::error_code ec;
		const fs::path directories[] = {
			mlc,
			mlc / "sys",
			mlc / "usr",
			mlc / "usr/title/00050000",
			mlc / "usr/title/0005000c",
			mlc / "usr/title/0005000e",
			mlc / "usr/save/00050010/1004a000/user/common/db",
			mlc / "usr/save/00050010/1004a100/user/common/db",
			mlc / "usr/save/00050010/1004a200/user/common/db",
			mlc / "sys/title/0005001b/1005c000/content",
		};
		for (auto& path : directories)
		{
			if (!fs::exists(path, ec) && !fs::create_directories(path, ec))
				return false;
		}
		try
		{
			const fs::path langDir = mlc / "sys/title/0005001b/1005c000/content";
			const fs::path langFile = langDir / "language.txt";
			if (!fs::exists(langFile, ec))
			{
				std::ofstream file(langFile);
				const char* langStrings[] = { "ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh" };
				for (const char* lang : langStrings)
					file << fmt::format(R"("{}",)", lang) << std::endl;
			}
			const fs::path countryFile = langDir / "country.txt";
			if (!fs::exists(countryFile, ec))
			{
				std::ofstream file(countryFile);
				for (sint32 i = 0; i < NCrypto::GetCountryCount(); i++)
				{
					const char* countryCode = NCrypto::GetCountryAsString(i);
					if (boost::iequals(countryCode, "NN"))
						file << "NULL," << std::endl;
					else
						file << fmt::format(R"("{}",)", countryCode) << std::endl;
				}
			}
			const fs::path dummyFile = mlc / "writetestdummy";
			{
				std::ofstream file(dummyFile);
				if (!file.is_open())
					return false;
			}
			fs::remove(dummyFile, ec);
		}
		catch (const std::exception&)
		{
			return false;
		}
		return true;
	}

	void _ApplyHostDefaults(bool firstStart)
	{
		auto& config = GetConfig();
		// this frontend only supports the D3D12 renderer
		config.graphic_api = GraphicAPI::kD3D12;
		if (firstStart)
		{
			config.async_compile = true;
			config.vsync = 1;
			// a console's output: full volume, 5.1 (XAudio2 downmixes for stereo devices)
			config.tv_channels = kSurround;
			config.tv_volume = 100;
		}
		// XAudio2 is the audio API that exists everywhere this frontend runs (Windows, UWP, Xbox)
		if (!IAudioAPI::IsAudioAPIAvailable((IAudioAPI::AudioAPI)config.audio_api) && IAudioAPI::IsAudioAPIAvailable(IAudioAPI::XAudio2))
			config.audio_api = IAudioAPI::XAudio2;
	}

	bool _PrepareTitle(const fs::path& launchPath, std::string& errorOut)
	{
		TitleInfo launchTitle{ launchPath };
		if (launchTitle.IsValid())
		{
			CafeTitleList::AddTitleFromPath(launchPath);
			TitleId baseTitleId;
			if (!CafeTitleList::FindBaseTitleId(launchTitle.GetAppTitleId(), baseTitleId))
			{
				errorOut = "Unable to launch game because the base files were not found.";
				return false;
			}
			const auto r = CafeSystem::PrepareForegroundTitle(baseTitleId);
			if (r == CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT)
			{
				errorOut = "Unable to mount title. Make sure the game folders are still valid.\n\n" + _pathToUtf8(launchPath);
				return false;
			}
			if (r != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				errorOut = "Failed to launch game.\n\n" + _pathToUtf8(launchPath);
				return false;
			}
			return true;
		}
		const CafeTitleFileType fileType = DetermineCafeSystemFileType(launchPath);
		if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF)
		{
			if (CafeSystem::PrepareForegroundTitleFromStandaloneRPX(launchPath) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				errorOut = "Failed to launch executable.\n\n" + _pathToUtf8(launchPath);
				return false;
			}
			return true;
		}
		errorOut = "Unable to launch game.\n\n" + _pathToUtf8(launchPath);
		if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_DISC_KEY)
			errorOut += "\n\nCould not decrypt the title. Make sure keys.txt contains the disc key for this game.";
		else if (launchTitle.GetInvalidReason() == TitleInfo::InvalidReason::NO_TITLE_TIK)
			errorOut += "\n\nCould not decrypt the title because title.tik is missing.";
		return false;
	}

	void _LaunchGame(const fs::path& path)
	{
		HostIcons::Stop(); // it mounts titles, finish before the game's title is mounted
		std::string error;
		if (!_PrepareTitle(path, error))
		{
			HostUI::PushMessage("Can't start the game", error);
			return;
		}
		{
			// launch log for tester reports: which title, and which update and DLC the title list found for it
			const TitleId titleId = CafeSystem::GetForegroundTitleId();
			uint16 baseVersion = 0, updateVersion = 0, dlcVersion = 0;
			const bool hasBase = CafeTitleList::HasTitle(titleId, baseVersion);
			const bool hasUpdate = CafeTitleList::HasTitle((titleId & ~0xFF00000000ull) | 0x0E00000000ull, updateVersion);
			const bool hasDlc = CafeTitleList::HasTitle((titleId & ~0xFF00000000ull) | 0x0C00000000ull, dlcVersion);
			cemuLog_log(LogType::Force, "Host: launching {} (title {:016x}, base {}, update {}, DLC {})", _pathToUtf8(path), titleId,
				hasBase ? fmt::format("v{}", baseVersion) : std::string("not in list"),
				hasUpdate ? fmt::format("v{}", updateVersion) : std::string("none"),
				hasDlc ? fmt::format("v{}", dlcVersion) : std::string("none"));
		}

		// the emulator renders into the same window, the launcher has to give up its swap chain first
		s_launcher.reset();

		auto& windowInfo = _WindowInfo();
		try
		{
			g_renderer = std::make_unique<D3D12Renderer>();
			D3D12Renderer::GetInstance()->InitializeSurface({ windowInfo.phys_width.load(), windowInfo.phys_height.load() }, true);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "Host: failed to initialize the D3D12 renderer: {}", ex.what());
			g_renderer.reset();
			HostUI::PushMessage("Graphics error", std::string("Failed to initialize Direct3D 12:\n") + ex.what() + "\n\nCemu will close.");
			// the title is already prepared, there is no clean way back to the launcher
			s_state = State::Quitting;
			return;
		}
		_EnableSkylanderPortal();
		{
			// diagnostics: <user>/dump_shaders.txt makes the shader cache loader write every shader to <user>/dump/shaders
			// (there is no debug menu on the Xbox)
			std::error_code ec;
			const bool dumpShaders = fs::exists(ActiveSettings::GetUserDataPath("dump_shaders.txt"), ec);
			ActiveSettings::EnableDumpShaders(dumpShaders);
			if (dumpShaders)
				fs::create_directories(ActiveSettings::GetUserDataPath("dump/shaders"), ec);
		}
		LatteOverlay_resetScreenPiPSources();
		LatteOverlay_setScreenPiP(_LoadOverlaySettings(CafeSystem::GetForegroundTitleId()));
		CafeSystem::LaunchForegroundTitle();
		s_state = State::Running;
		HostTools::MarkGameStarted(CafeSystem::GetForegroundTitleName());
		if (!s_menuHintShown)
		{
			s_menuHintShown = true;
			s_menuHintPending = true;
		}
	}

	void _RenderLauncherFrame()
	{
		auto& windowInfo = _WindowInfo();
		const int w = windowInfo.phys_width;
		const int h = windowInfo.phys_height;
		if (w <= 0 || h <= 0)
			return;

		ImGui::SetCurrentContext(s_launcherContext);
		if (!s_launcher && !s_launcherFailed)
		{
			try
			{
				s_launcher = std::make_unique<LauncherRenderer>(HostPlatform::GetRenderTarget(), w, h, s_launcherContext);
			}
			catch (const std::exception& ex)
			{
				s_launcherFailed = true;
				cemuLog_log(LogType::Force, "Host: failed to create the launcher renderer: {}", ex.what());
				WindowSystem::ShowErrorDialog(ex.what(), "Direct3D 12 error");
				return;
			}
		}
		if (!s_launcher)
			return;

		if (HostUI::SetScale(_UIScale()))
			s_launcher->InvalidateFonts();

		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2((float)w, (float)h);
		io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
		static auto s_lastFrame = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		io.DeltaTime = std::clamp(std::chrono::duration<float>(now - s_lastFrame).count(), 1.0f / 1000.0f, 0.25f);
		s_lastFrame = now;
		HostControllers::UpdatePairing();
		HostControllers::UpdateCapture();
		HostGamepad::State pad = HostGamepad::Poll();
		if (HostControllers::IsCapturing())
			pad = HostGamepad::State{ pad.connected }; // the press being captured isn't UI input
		else if (s_launcherIgnoresPad)
		{
			if (pad.buttons == 0)
				s_launcherIgnoresPad = false;
			else
				pad = HostGamepad::State{ pad.connected };
		}
		HostGamepad::FeedImGui(pad);

		s_launcher->NewFrame();
		ImGui::NewFrame();
		std::optional<fs::path> launch = HostUI::DrawLauncher();
		ImGui::Render();
		const float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
		if (!s_launcher->Render(ImGui::GetDrawData(), clearColor))
		{
			cemuLog_log(LogType::Force, "Host: launcher device lost, recreating");
			s_launcher.reset();
		}
		if (launch && !s_pendingLaunch)
			s_pendingLaunch = *launch; // started at the beginning of the next frame (after the game list scan)
	}

	// hands the cursor to the emulated GamePad as a touch, the same way a touch on the wx frontend's window does
	void _SetTouch(bool down)
	{
		s_touchDown = down;
		sint32 x, y, w, h;
		LatteRenderTarget_getScreenImageArea(&x, &y, &w, &h, nullptr, nullptr, false);
		auto& touch = InputManager::instance().m_main_touch;
		std::scoped_lock lock(touch.m_mutex);
		touch.position = { x + (sint32)(s_touchX * w), y + (sint32)(s_touchY * h) };
		if (down && !touch.left_down)
			touch.left_down_toggle = true; // a tap shorter than the game's input poll still counts
		touch.left_down = down;
	}

	void _UpdateTouchCursor(const HostGamepad::State& pad)
	{
		const auto now = std::chrono::steady_clock::now();
		const float dt = std::min(0.1f, std::chrono::duration<float>(now - s_touchLastUpdate).count());
		s_touchLastUpdate = now;
		// crosses the screen in about a second at full tilt, slower near the center for precise placement
		auto speed = [](float v) { return v * std::abs(v) * 1.1f; };
		s_touchX = std::clamp(s_touchX + speed(pad.rightX) * dt, 0.0f, 1.0f);
		s_touchY = std::clamp(s_touchY - speed(pad.rightY) * dt, 0.0f, 1.0f);
		_SetTouch(pad.rightTrigger > 0.5f);
	}

	// amiibo: .bin dumps in <user data>/amiibo, picked with left/right on the menu item and scanned with A
	std::mutex s_amiiboMutex;
	std::vector<fs::path> s_amiiboFiles;
	int s_amiiboIndex = 0;
	std::string s_amiiboStatus;

	void _RefreshAmiiboFiles()
	{
		std::vector<fs::path> files;
		std::error_code ec;
		const fs::path folder = ActiveSettings::GetUserDataPath("amiibo");
		fs::create_directories(folder, ec);
		for (fs::directory_iterator it(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			std::error_code ecEntry;
			if (it->is_regular_file(ecEntry) && boost::iequals(_pathToUtf8(it->path().extension()), ".bin"))
				files.emplace_back(it->path());
		}
		std::sort(files.begin(), files.end());
		std::scoped_lock lock(s_amiiboMutex);
		s_amiiboFiles = std::move(files);
		s_amiiboIndex = std::clamp(s_amiiboIndex, 0, std::max(0, (int)s_amiiboFiles.size() - 1));
	}

	void _StepAmiibo(int step)
	{
		std::scoped_lock lock(s_amiiboMutex);
		if (s_amiiboFiles.empty())
			return;
		const int count = (int)s_amiiboFiles.size();
		s_amiiboIndex = (s_amiiboIndex + step + count) % count;
		s_amiiboStatus.clear();
	}

	std::string _AmiiboLabel()
	{
		std::scoped_lock lock(s_amiiboMutex);
		if (s_amiiboFiles.empty())
			return "Scan amiibo: put .bin files into the amiibo folder";
		std::string label = fmt::format("Scan amiibo: < {} >", _pathToUtf8(s_amiiboFiles[s_amiiboIndex].stem()));
		if (!s_amiiboStatus.empty())
			label += "  " + s_amiiboStatus;
		return label;
	}

	void _ScanAmiibo()
	{
		std::scoped_lock lock(s_amiiboMutex);
		if (s_amiiboFiles.empty())
			return;
		const fs::path& file = s_amiiboFiles[s_amiiboIndex];
		uint32 nfcError = 0;
		const bool ok = nnNfp_touchNfcTagFromFile(file, &nfcError);
		s_amiiboStatus = ok ? "(scanned)" : fmt::format("(failed, error {})", nfcError);
		cemuLog_log(LogType::Force, "Host: amiibo {} {}", _pathToUtf8(file), ok ? "scanned" : fmt::format("failed, NFC error {}", nfcError));
	}

	// Skylanders: figure dumps (.sky/.bin/.dmp) in <user data>/skylanders, picked with left/right on the menu item. A puts the
	// picked figure on the emulated portal (replacing the one there) or takes it off again. The portal is only emulated
	// when the folder has figures at game launch (see _EnableSkylanderPortal), like desktop Cemu's opt-in setting
	std::mutex s_skyMutex;
	std::vector<fs::path> s_skyFiles;
	int s_skyIndex = 0;
	std::optional<uint8> s_skyPortalSlot; // portal slot of the placed figure
	fs::path s_skyPlaced;

	std::vector<fs::path> _ListSkylanderFiles()
	{
		std::vector<fs::path> files;
		std::error_code ec;
		const fs::path folder = ActiveSettings::GetUserDataPath("skylanders");
		fs::create_directories(folder, ec);
		for (fs::directory_iterator it(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
		{
			std::error_code ecEntry;
			const std::string ext = _pathToUtf8(it->path().extension());
			if (it->is_regular_file(ecEntry) && (boost::iequals(ext, ".sky") || boost::iequals(ext, ".bin") || boost::iequals(ext, ".dmp")))
				files.emplace_back(it->path());
		}
		std::sort(files.begin(), files.end());
		return files;
	}

	void _RefreshSkylanderFiles()
	{
		std::vector<fs::path> files = _ListSkylanderFiles();
		std::scoped_lock lock(s_skyMutex);
		s_skyFiles = std::move(files);
		s_skyIndex = std::clamp(s_skyIndex, 0, std::max(0, (int)s_skyFiles.size() - 1));
	}

	void _EnableSkylanderPortal()
	{
		if (_ListSkylanderFiles().empty())
			return;
		auto& portal = GetConfig().emulated_usb_devices.emulate_skylander_portal;
		if (!portal.GetValue())
		{
			portal = true;
			cemuLog_log(LogType::Force, "Host: skylanders folder has figures, emulating the Skylanders portal");
		}
	}

	void _StepSkylander(int step)
	{
		std::scoped_lock lock(s_skyMutex);
		if (s_skyFiles.empty())
			return;
		const int count = (int)s_skyFiles.size();
		s_skyIndex = (s_skyIndex + step + count) % count;
	}

	std::string _SkylanderLabel()
	{
		std::scoped_lock lock(s_skyMutex);
		if (s_skyFiles.empty())
			return "Skylander: put figure dumps into the skylanders folder";
		const fs::path& file = s_skyFiles[s_skyIndex];
		if (!GetConfig().emulated_usb_devices.emulate_skylander_portal.GetValue())
			return "Skylander: restart the game to connect the portal";
		std::string label = fmt::format("Skylander: < {} >", _pathToUtf8(file.stem()));
		if (s_skyPortalSlot && file == s_skyPlaced)
			label += "  (on the portal, A removes)";
		return label;
	}

	void _ToggleSkylander()
	{
		std::scoped_lock lock(s_skyMutex);
		if (s_skyFiles.empty() || !GetConfig().emulated_usb_devices.emulate_skylander_portal.GetValue())
			return;
		const fs::path file = s_skyFiles[s_skyIndex];
		const bool wasThis = s_skyPortalSlot && file == s_skyPlaced;
		if (s_skyPortalSlot)
		{
			nsyshid::g_skyportal.RemoveSkylander(*s_skyPortalSlot);
			cemuLog_log(LogType::Force, "Host: skylander {} removed from the portal", _pathToUtf8(s_skyPlaced));
			s_skyPortalSlot.reset();
		}
		if (wasThis)
			return;
		std::unique_ptr<FileStream> skyFile(FileStream::openFile2(file, true));
		std::array<uint8, nsyshid::SKY_FIGURE_SIZE> data;
		if (!skyFile || skyFile->readData(data.data(), data.size()) != data.size())
		{
			cemuLog_log(LogType::Force, "Host: skylander {} couldn't be read (needs a {} byte figure dump)", _pathToUtf8(file), data.size());
			return;
		}
		const uint8 slot = nsyshid::g_skyportal.LoadSkylander(data.data(), std::move(skyFile));
		if (slot >= nsyshid::MAX_SKYLANDERS)
		{
			cemuLog_log(LogType::Force, "Host: skylander {}: the portal is full", _pathToUtf8(file));
			return;
		}
		s_skyPortalSlot = slot;
		s_skyPlaced = file;
		cemuLog_log(LogType::Force, "Host: skylander {} placed on the portal (slot {})", _pathToUtf8(file), slot);
	}

	// Xbox controllers have no gyro. Like desktop Cemu's right mouse button motion, a held right "mouse" button tilts the
	// emulated GamePad by its position (center = flat), so the right stick deflection is fed as that position
	void _SetStickTilt(float x, float y, bool active)
	{
		int w, h;
		WindowSystem::GetWindowPhysSize(w, h);
		auto& mouse = InputManager::instance().m_main_mouse;
		std::scoped_lock lock(mouse.m_mutex);
		// up to 2/3 of the way to the edge: about 90 degrees forward/back and 120 degrees sideways at full tilt
		mouse.position = { (sint32)(w * (0.5f + x * 0.33f)), (sint32)(h * (0.5f - y * 0.33f)) };
		mouse.right_down = active;
	}

	// left/right on a menu item with a value
	void _StepMenuItem(int item, int direction)
	{
		auto& config = GetConfig();
		switch (item)
		{
		case kMenuAmiibo:
			_StepAmiibo(direction);
			break;
		case kMenuSkylander:
			_StepSkylander(direction);
			break;
		case kMenuGamePadOverlay:
		case kMenuOverlayCorner:
		case kMenuOverlaySize:
		case kMenuOverlayOpacity:
			_ChangeOverlay(item, direction);
			break;
		case kMenuVSync:
			config.vsync = config.vsync.GetValue() != 0 ? 0 : 1;
			GetConfigHandle().Save();
			break;
		case kMenuVolume:
		{
			config.tv_volume = std::clamp<int>(config.tv_volume + direction * 10, 0, 100);
			std::shared_lock lock(g_audioMutex);
			if (g_tvAudio)
				g_tvAudio->SetVolume(config.tv_volume);
			GetConfigHandle().Save();
			break;
		}
		case kMenuDeadzone:
		{
			const auto info = HostControllers::GetPlayer(0);
			const float deadzone = std::clamp(std::round(info.deadzone[0] * 20.0f + direction) / 20.0f, 0.0f, 0.9f);
			HostControllers::SetDeadzone(0, 0, deadzone);
			HostControllers::SetDeadzone(0, 1, std::clamp(std::round(info.deadzone[1] * 20.0f + direction) / 20.0f, 0.0f, 0.9f));
			break;
		}
		default:
			break;
		}
	}

	void _ExecuteMenuItem(int item)
	{
		auto& config = GetConfig();
		switch (item)
		{
		case kMenuResume:
			s_menuOpen = false;
			break;
		case kMenuPairControllers:
			s_menuOpen = false;
			HostControllers::BeginPairing(true);
			break;
		case kMenuGamePadScreen:
			// same state the screen swap hotkey (Ctrl+Tab in the wx frontend) flips
			LatteGPUState.isDRCPrimary = !LatteGPUState.isDRCPrimary;
			break;
		case kMenuGamePadOverlay:
		case kMenuOverlayCorner:
		case kMenuOverlaySize:
		case kMenuOverlayOpacity:
			_ChangeOverlay(item, 1);
			break;
		case kMenuTouchCursor:
			s_touchCursor = !s_touchCursor;
			if (s_touchCursor)
			{
				if (s_stickTilt)
				{
					s_stickTilt = false; // both use the right stick
					_SetStickTilt(0.0f, 0.0f, false);
				}
				// touches map onto the screen that is shown, so show the GamePad's
				s_touchRestoreTVScreen = !LatteGPUState.isDRCPrimary;
				LatteGPUState.isDRCPrimary = true;
				s_touchLastUpdate = std::chrono::steady_clock::now();
				s_menuOpen = false;
			}
			else
			{
				_SetTouch(false);
				if (s_touchRestoreTVScreen)
					LatteGPUState.isDRCPrimary = false;
			}
			break;
		case kMenuVSync:
			_StepMenuItem(item, 1);
			break;
		case kMenuShowFPS:
		{
			const bool show = !(config.overlay.position != ScreenPosition::kDisabled && config.overlay.fps);
			config.overlay.position = show ? ScreenPosition::kTopLeft : ScreenPosition::kDisabled;
			config.overlay.fps = show;
			GetConfigHandle().Save();
			break;
		}
		case kMenuStickTilt:
			s_stickTilt = !s_stickTilt;
			if (s_stickTilt)
			{
				if (s_touchCursor)
				{
					s_touchCursor = false; // both use the right stick
					_SetTouch(false);
				}
				s_menuOpen = false;
			}
			else
				_SetStickTilt(0.0f, 0.0f, false);
			break;
		case kMenuAmiibo:
			_ScanAmiibo();
			break;
		case kMenuSkylander:
			_ToggleSkylander();
			break;
		case kMenuExitGame:
			s_menuOpen = false;
			s_exitGameRequested = true;
			break;
		case kMenuExit:
			s_menuOpen = false;
			s_state = State::Quitting;
			break;
		}
	}

	// the Xbox ends apps that go over their memory budget without an exception (Mario Kart 8 at 4K died after a race with
	// nothing in the log), so log the usage every 30 s and whenever it reaches a new high in 256 MB steps
	void _LogMemoryUsage()
	{
		static auto s_next = std::chrono::steady_clock::now();
		static uint64 s_highest = 0;
		static int s_pressure = 0;
		const auto now = std::chrono::steady_clock::now();
		if (now < s_next)
			return;
		s_next = now + std::chrono::milliseconds(250);
		uint64 usage, limit;
		HostPlatform::GetMemoryUsage(usage, limit);
		// above 80% of the budget the texture cache releases unused textures early, above 90% all of them at once (loading a
		// Mario Kart 8 track at 4K went from 82% to 94% within 1.3 s), below 70% it goes back to normal
		if (limit)
		{
			const uint64 percent = usage * 100 / limit;
			int pressure = 0;
			if (percent >= 90)
				pressure = 2;
			else if (percent >= 80 || (s_pressure != 0 && percent >= 70))
				pressure = 1;
			// set on every check: a renderer raises it to 2 when a texture allocation fails, this clears that again
			LatteTC_SetMemoryPressure(pressure);
			// freed CRT heap pages still count against the Xbox's budget until they're decommitted (danprice142/Cemu-UWP
			// does the same under pressure). At most every 5 s, it walks the heap under its lock
			static auto s_nextCompact = now;
			if (pressure != 0 && now >= s_nextCompact)
			{
				s_nextCompact = now + std::chrono::seconds(5);
				HeapCompact(GetProcessHeap(), 0);
			}
			if (pressure != s_pressure)
			{
				s_pressure = pressure;
				cemuLog_log(LogType::Force, "Host: memory {} MB of {} MB, texture memory pressure {}", usage >> 20, limit >> 20, pressure == 2 ? "high" : (pressure ? "on" : "off"));
			}
		}
		static auto s_lastLog = now - std::chrono::seconds(30);
		const bool newHigh = usage >= s_highest + 256 * 1024 * 1024;
		if (!newHigh && now - s_lastLog < std::chrono::seconds(30))
			return;
		s_lastLog = now;
		s_highest = std::max(s_highest, usage);
		if (limit)
			cemuLog_log(LogType::Force, "Host: memory {} MB of {} MB ({}%)", usage >> 20, limit >> 20, usage * 100 / limit);
		else
			cemuLog_log(LogType::Force, "Host: memory {} MB", usage >> 20);
	}

	// stops the title (the GPU thread destroys the renderer and its swap chain on the way out) and shows the launcher
	// again, which recreates its own swap chain for the window
	void _ExitToLauncher()
	{
		cemuLog_log(LogType::Force, "Host: stopping the title, back to the game list");
		s_menuOpen = false;
		s_comboActive = false;
		if (s_touchCursor)
		{
			s_touchCursor = false;
			_SetTouch(false);
		}
		if (s_stickTilt)
		{
			s_stickTilt = false;
			_SetStickTilt(0.0f, 0.0f, false);
		}
		CafeSystem::ShutdownTitle();
		HostTools::MarkGameStopped();
		GetConfigHandle().Save();
		InputManager::instance().save();
		s_launcherIgnoresPad = true;
		s_state = State::Launcher;
		HostUI::RequestGameListRefresh();
	}

	// test rig (#433): <user>/rig_screenshot.txt holding a name saves the next TV frame as <user>/rig/<name>.bmp and is
	// deleted. With keys posted to the window this drives the game while the PC is locked (no focus, no screen capture)
	void _CheckRigScreenshot()
	{
		static auto s_next = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		if (now < s_next || !g_renderer)
			return;
		s_next = now + std::chrono::milliseconds(250);
		std::error_code ec;
		const fs::path request = ActiveSettings::GetUserDataPath("rig_screenshot.txt");
		if (!fs::exists(request, ec))
			return;
		std::string name;
		std::ifstream(request) >> name;
		fs::remove(request, ec);
		if (name.empty())
			name = "shot";
		const fs::path dir = ActiveSettings::GetUserDataPath("rig");
		fs::create_directories(dir, ec);
		const fs::path file = dir / (name + ".bmp");
		g_renderer->RequestScreenshot([file](const std::vector<uint8>& rgb, int width, int height, bool mainWindow) -> std::optional<std::string> {
			if (!mainWindow || width <= 0 || height <= 0 || rgb.size() < (size_t)width * height * 3)
				return std::nullopt;
			const uint32 rowSize = ((uint32)width * 3 + 3) & ~3u;
			const uint32 dataSize = rowSize * (uint32)height;
			uint8 header[54] = {'B', 'M'};
			auto put32 = [&](int at, uint32 v) { memcpy(header + at, &v, 4); };
			put32(2, 54 + dataSize);
			put32(10, 54);
			put32(14, 40);
			put32(18, (uint32)width);
			put32(22, (uint32)height);
			header[26] = 1;
			header[28] = 24;
			put32(34, dataSize);
			std::ofstream out(file, std::ios::binary);
			out.write((const char*)header, sizeof(header));
			std::vector<uint8> row(rowSize, 0);
			for (int y = height - 1; y >= 0; y--)
			{
				const uint8* src = rgb.data() + (size_t)y * width * 3;
				for (int x = 0; x < width; x++)
				{
					row[x * 3 + 0] = src[x * 3 + 2];
					row[x * 3 + 1] = src[x * 3 + 1];
					row[x * 3 + 2] = src[x * 3 + 0];
				}
				out.write((const char*)row.data(), rowSize);
			}
			cemuLog_log(LogType::Force, "Host: rig screenshot {}", _pathToUtf8(file));
			return std::nullopt;
		});
	}

	void _UpdateInGame()
	{
		_CheckRigScreenshot();
		const HostGamepad::State pad = HostGamepad::Poll();
		const uint32 pressed = pad.buttons & ~s_lastPadButtons;
		s_lastPadButtons = pad.buttons;
		if (HostControllers::IsPairing())
		{
			HostControllers::UpdatePairing();
			return;
		}

		// hold View + Menu (Back + Start) for one second to open the menu
		const bool combo = (pad.buttons & (HostGamepad::kBack | HostGamepad::kStart)) == (HostGamepad::kBack | HostGamepad::kStart);
		const auto now = std::chrono::steady_clock::now();
		if (combo)
		{
			if (!s_comboActive)
			{
				s_comboActive = true;
				s_comboConsumed = false;
				s_comboStart = now;
			}
			else if (!s_comboConsumed && now - s_comboStart >= std::chrono::milliseconds(1000))
			{
				s_comboConsumed = true;
				s_menuOpen = !s_menuOpen;
				s_menuSelection = kMenuResume;
				if (s_menuOpen)
				{
					_RefreshAmiiboFiles();
					_RefreshSkylanderFiles();
				}
			}
		}
		else
			s_comboActive = false;
		// or click both sticks (like Dolphin's UWP port)
		constexpr uint32 kSticks = HostGamepad::kL3 | HostGamepad::kR3;
		if ((pad.buttons & kSticks) == kSticks && (pressed & kSticks) != 0)
		{
			s_menuOpen = !s_menuOpen;
			s_menuSelection = kMenuResume;
			if (s_menuOpen)
			{
				_RefreshAmiiboFiles();
				_RefreshSkylanderFiles();
			}
		}

		if (!s_menuOpen)
		{
			if (s_touchCursor)
				_UpdateTouchCursor(pad);
			else if (s_stickTilt)
				_SetStickTilt(pad.rightX, pad.rightY, true);
			return;
		}
		if (pressed & HostGamepad::kUp)
			s_menuSelection = (s_menuSelection + kMenuCount - 1) % kMenuCount;
		if (pressed & HostGamepad::kDown)
			s_menuSelection = (s_menuSelection + 1) % kMenuCount;
		if (pressed & HostGamepad::kB)
			s_menuOpen = false;
		if (pressed & (HostGamepad::kLeft | HostGamepad::kRight))
			_StepMenuItem(s_menuSelection, (pressed & HostGamepad::kLeft) ? -1 : 1);
		if (pressed & HostGamepad::kA)
			_ExecuteMenuItem(s_menuSelection);
	}
}

bool HostApp::Initialize(const InitOptions& options, std::string& errorOut)
{
	s_options = options;
	std::error_code ec;
	fs::create_directories(options.userDataPath, ec);

	wchar_t exePathBuffer[4096]{};
	GetModuleFileNameW(nullptr, exePathBuffer, (DWORD)std::size(exePathBuffer));
	const fs::path exePath(exePathBuffer);
	std::set<fs::path> failedWriteAccess;
	// everything the user owns lives in one folder, read only data (resources, game profiles) next to the executable
	ActiveSettings::SetPaths(true, exePath, options.userDataPath, options.userDataPath, options.userDataPath, options.dataPath, failedWriteAccess);
	if (!failedWriteAccess.empty())
	{
		errorOut = "Cemu can't write to " + _pathToUtf8(*failedWriteAccess.begin());
		return false;
	}
	fs::create_directories(ActiveSettings::GetConfigPath("controllerProfiles"), ec);
	fs::create_directories(ActiveSettings::GetUserDataPath("memorySearcher"), ec);
	// write the log from the start instead of only once a game starts: on Xbox it's read through the Device Portal, and
	// a session that never starts a game or gets terminated would otherwise leave no log at all
	cemuLog_createLogFile(false);
	// the previous session's log is log_previous.txt now
	if (auto problem = HostTools::CheckLastSession())
		HostUI::SetLastSessionProblem(problem->title, problem->text);

	const fs::path settingsPath = ActiveSettings::GetConfigPath("settings.xml");
	GetConfigHandle().SetFilename(settingsPath.generic_wstring());
	const bool firstStart = !fs::exists(settingsPath, ec);
	NetworkConfig::LoadOnce();
	if (!firstStart)
		GetConfigHandle().Load();
	GetConfigHandle().Save();

	// save folder on a USB drive (GitHub #6): when the drive isn't there, this session uses the app's own mlc01 and the
	// launcher says so; the setting is kept for the next start
	if (const std::string saveFolder = GetConfig().mlc_path.GetValue(); !saveFolder.empty() && !LaunchSettings::GetMLCPath())
	{
		const fs::path folder = _utf8ToPath(saveFolder);
		if (!fs::exists(folder.root_path(), ec))
		{
			cemuLog_log(LogType::Force, "Host: save folder {} not found (drive missing), using {} for this session", saveFolder, _pathToUtf8(ActiveSettings::GetDefaultMLCPath()));
			LaunchSettings::SetMLCPath(ActiveSettings::GetDefaultMLCPath());
			HostTools::SetSaveFolderMissing();
			HostUI::PushMessage("Save folder not found", fmt::format("{} isn't there (is the USB drive connected?). Cemu uses its own storage for saves this time; "
				"connect the drive and restart Cemu to use your save folder again.", saveFolder));
		}
	}

	if (!_CreateDefaultMLCFiles(ActiveSettings::GetMlcPath()))
	{
		errorOut = "Cemu failed to write to the mlc folder:\n" + _pathToUtf8(ActiveSettings::GetMlcPath());
		return false;
	}
	ActiveSettings::Init();
	LatteOverlay_init();
	LatteOverlay_setHostOverlayCallback(&HostApp::DrawInGameMenu);
	CemuCommonInit();
	_ApplyHostDefaults(firstStart);
	GetConfigHandle().Save();
	HostControllers::SetupDefaults();

	auto& windowInfo = _WindowInfo();
	windowInfo.canvas_main = HostPlatform::GetRenderTarget();
	windowInfo.window_main = windowInfo.canvas_main;
	windowInfo.is_fullscreen = true;
	windowInfo.app_active = true;
	windowInfo.pad_open = false;
	if (windowInfo.dpi_scale <= 0.0)
		windowInfo.dpi_scale = 1.0;

	IMGUI_CHECKVERSION();
	ImGuiContext* previous = ImGui::GetCurrentContext();
	s_launcherContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(s_launcherContext);
	HostUI::Initialize(s_launcherContext, ActiveSettings::GetDataPath("resources/fonts/launcher.ttf"), _UIScale());
	HostUI::SetTextureProvider([](uint64 key, const uint8* rgba, int width, int height) -> void* {
		return s_launcher ? s_launcher->GetOrCreateTexture(key, rgba, width, height) : nullptr;
	});
	ImGui::SetCurrentContext(previous);
	HostTools::StartUpdateCheck();

	if (!options.launchPath.empty())
		s_pendingLaunch = options.launchPath;
	return true;
}

bool HostApp::RunFrame()
{
	HostMusic::SetActive(s_state == State::Launcher);
	HostSounds::SetActive(s_state == State::Launcher);
	switch (s_state)
	{
	case State::Launcher:
		if (s_pendingLaunch && CafeTitleList::IsScanning())
		{
			// updates and DLC found later in the scan would be missing from the game (it then asks for an update),
			// so a launch waits until the game list scan is done. The launcher keeps drawing meanwhile
			static auto s_lastWaitLog = std::chrono::steady_clock::time_point{};
			if (std::chrono::steady_clock::now() - s_lastWaitLog > std::chrono::seconds(5))
			{
				s_lastWaitLog = std::chrono::steady_clock::now();
				cemuLog_log(LogType::Force, "Host: launch of {} waits for the game list scan", _pathToUtf8(*s_pendingLaunch));
			}
			_RenderLauncherFrame();
			break;
		}
		if (s_pendingLaunch)
		{
			const fs::path path = *s_pendingLaunch;
			s_pendingLaunch.reset();
			_LaunchGame(path);
			break;
		}
		_RenderLauncherFrame();
		break;
	case State::Running:
		if (s_exitGameRequested)
		{
			s_exitGameRequested = false;
			_ExitToLauncher();
			break;
		}
		_UpdateInGame();
		_LogMemoryUsage();
		if (g_renderer && g_renderer->GetType() == RendererAPI::D3D12)
			static_cast<D3D12Renderer*>(g_renderer.get())->CheckGpuProgress();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
		break;
	case State::Quitting:
		return false;
	}
	return s_state != State::Quitting;
}

void HostApp::Shutdown()
{
	s_menuOpen = false;
	if (CafeSystem::IsTitleRunning())
	{
		cemuLog_log(LogType::Force, "Host: stopping the running title");
		CafeSystem::ShutdownTitle();
	}
	HostTools::MarkGameStopped();
	HostIcons::Stop();
	HostMusic::Shutdown();
	HostSounds::Shutdown();
	s_launcher.reset();
	if (s_launcherContext)
	{
		ImGui::DestroyContext(s_launcherContext);
		s_launcherContext = nullptr;
	}
	GetConfigHandle().Save();
	InputManager::instance().save();
}

void HostApp::OnSuspending(bool suspended)
{
	HostTools::MarkSuspended(suspended);
}

bool HostApp::IsGameRunning()
{
	return s_state == State::Running;
}

void HostApp::OnResize(int physWidth, int physHeight, double dpiScale)
{
	auto& windowInfo = _WindowInfo();
	windowInfo.phys_width = physWidth;
	windowInfo.phys_height = physHeight;
	windowInfo.dpi_scale = dpiScale > 0.0 ? dpiScale : 1.0;
	windowInfo.width = (int)(physWidth / windowInfo.dpi_scale);
	windowInfo.height = (int)(physHeight / windowInfo.dpi_scale);
	if (physWidth <= 0 || physHeight <= 0)
		return;
	if (s_state == State::Launcher && s_launcher)
		s_launcher->Resize(physWidth, physHeight);
	else if (s_state == State::Running && g_renderer)
		D3D12Renderer::GetInstance()->ResizeSurface({ physWidth, physHeight }, true);
}

void HostApp::OnFocusChanged(bool hasFocus)
{
	_WindowInfo().app_active = hasFocus;
	if (!hasFocus)
		_WindowInfo().set_keystatesup();
}

void HostApp::OnMouseMove(float x, float y)
{
	if (s_state != State::Launcher || !s_launcherContext)
		return;
	ImGui::SetCurrentContext(s_launcherContext);
	ImGui::GetIO().AddMousePosEvent(x, y);
}

void HostApp::OnMouseButton(int button, bool down)
{
	if (s_state != State::Launcher || !s_launcherContext)
		return;
	ImGui::SetCurrentContext(s_launcherContext);
	ImGui::GetIO().AddMouseButtonEvent(button, down);
}

void HostApp::OnMouseWheel(float delta)
{
	if (s_state != State::Launcher || !s_launcherContext)
		return;
	ImGui::SetCurrentContext(s_launcherContext);
	ImGui::GetIO().AddMouseWheelEvent(0.0f, delta);
}

static ImGuiKey _VirtualKeyToImGuiKey(uint32 vk)
{
	switch (vk)
	{
	case VK_TAB: return ImGuiKey_Tab;
	case VK_LEFT: return ImGuiKey_LeftArrow;
	case VK_RIGHT: return ImGuiKey_RightArrow;
	case VK_UP: return ImGuiKey_UpArrow;
	case VK_DOWN: return ImGuiKey_DownArrow;
	case VK_PRIOR: return ImGuiKey_PageUp;
	case VK_NEXT: return ImGuiKey_PageDown;
	case VK_HOME: return ImGuiKey_Home;
	case VK_END: return ImGuiKey_End;
	case VK_INSERT: return ImGuiKey_Insert;
	case VK_DELETE: return ImGuiKey_Delete;
	case VK_BACK: return ImGuiKey_Backspace;
	case VK_SPACE: return ImGuiKey_Space;
	case VK_RETURN: return ImGuiKey_Enter;
	case VK_ESCAPE: return ImGuiKey_Escape;
	case VK_SHIFT: case VK_LSHIFT: return ImGuiKey_LeftShift;
	case VK_RSHIFT: return ImGuiKey_RightShift;
	case VK_CONTROL: case VK_LCONTROL: return ImGuiKey_LeftCtrl;
	case VK_RCONTROL: return ImGuiKey_RightCtrl;
	case VK_MENU: case VK_LMENU: return ImGuiKey_LeftAlt;
	case VK_RMENU: return ImGuiKey_RightAlt;
	}
	if (vk >= 'A' && vk <= 'Z')
		return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
	if (vk >= '0' && vk <= '9')
		return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
	if (vk >= VK_F1 && vk <= VK_F12)
		return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
	return ImGuiKey_None;
}

void HostApp::OnKey(uint32 virtualKey, bool down)
{
	_WindowInfo().set_keystate(virtualKey, down);
	if (s_state == State::Running)
	{
		// keyboard access to the in-game menu: F1 opens/closes, arrows + enter select
		if (!down)
			return;
		if (virtualKey == VK_ESCAPE && HostControllers::IsPairing())
		{
			HostControllers::CancelPairing();
			return;
		}
		if (virtualKey == VK_F1)
		{
			s_menuOpen = !s_menuOpen;
			s_menuSelection = kMenuResume;
			if (s_menuOpen)
			{
				_RefreshAmiiboFiles();
				_RefreshSkylanderFiles();
			}
		}
		else if (s_menuOpen)
		{
			if (virtualKey == VK_UP)
				s_menuSelection = (s_menuSelection + kMenuCount - 1) % kMenuCount;
			else if (virtualKey == VK_DOWN)
				s_menuSelection = (s_menuSelection + 1) % kMenuCount;
			else if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT)
				_StepMenuItem(s_menuSelection, virtualKey == VK_LEFT ? -1 : 1);
			else if (virtualKey == VK_RETURN)
				_ExecuteMenuItem(s_menuSelection);
			else if (virtualKey == VK_ESCAPE)
				s_menuOpen = false;
		}
		return;
	}
	if (!s_launcherContext)
		return;
	if (down && virtualKey == VK_ESCAPE && HostControllers::IsPairing())
	{
		HostControllers::CancelPairing();
		return;
	}
	ImGui::SetCurrentContext(s_launcherContext);
	ImGuiIO& io = ImGui::GetIO();
	if (virtualKey == VK_SHIFT || virtualKey == VK_LSHIFT || virtualKey == VK_RSHIFT)
		io.AddKeyEvent(ImGuiMod_Shift, down);
	if (virtualKey == VK_CONTROL || virtualKey == VK_LCONTROL || virtualKey == VK_RCONTROL)
		io.AddKeyEvent(ImGuiMod_Ctrl, down);
	if (virtualKey == VK_MENU || virtualKey == VK_LMENU || virtualKey == VK_RMENU)
		io.AddKeyEvent(ImGuiMod_Alt, down);
	const ImGuiKey key = _VirtualKeyToImGuiKey(virtualKey);
	if (key != ImGuiKey_None)
		io.AddKeyEvent(key, down);
}

void HostApp::OnChar(uint32 utf32)
{
	if (s_state != State::Launcher || !s_launcherContext)
		return;
	ImGui::SetCurrentContext(s_launcherContext);
	ImGui::GetIO().AddInputCharacter(utf32);
}

bool HostApp::OnBackRequested()
{
	if (HostControllers::IsPairing())
	{
		HostControllers::CancelPairing();
		return true;
	}
	if (s_state == State::Running)
	{
		// B in game belongs to the game. Only close the menu if it is open
		if (s_menuOpen)
		{
			s_menuOpen = false;
			return true;
		}
		return true; // never let the system navigate away (that would suspend the app)
	}
	HostUI::HandleBack();
	return true;
}

bool HostApp::IsInGameMenuOpen()
{
	return s_menuOpen;
}

void HostApp::DrawInGameMenu()
{
	if (s_touchCursor && !s_menuOpen)
	{
		sint32 x, y, w, h;
		LatteRenderTarget_getScreenImageArea(&x, &y, &w, &h, nullptr, nullptr, false);
		const ImVec2 p((float)x + s_touchX * (float)w, (float)y + s_touchY * (float)h);
		const float r = std::max(10.0f, (float)h / 60.0f);
		ImDrawList* draw = ImGui::GetForegroundDrawList();
		const ImU32 fill = s_touchDown ? IM_COL32(255, 200, 40, 230) : IM_COL32(255, 255, 255, 140);
		draw->AddCircleFilled(p, r, fill);
		draw->AddCircle(p, r, IM_COL32(0, 0, 0, 220), 0, std::max(2.0f, r / 5.0f));
	}
	const bool pairing = HostControllers::IsPairing();
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const float scale = std::max(1.0f, viewport->Size.y / 1080.0f);
	if (s_menuHintPending.exchange(false))
		s_menuHintUntil = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	if (!s_menuOpen && !pairing && std::chrono::steady_clock::now() < s_menuHintUntil)
	{
		// bottom center, small and out of the way of the game's own HUD
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y - 60.0f * scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.75f);
		ImFont* hintFont = ImGui_GetFont(24.0f * scale);
		if (hintFont)
			ImGui::PushFont(hintFont);
		if (ImGui::Begin("##menuhint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
			ImGui::TextUnformatted("Menu: click both sticks (or hold View + Menu)");
		ImGui::End();
		if (hintFont)
			ImGui::PopFont();
	}
	if (!s_menuOpen && !pairing)
		return;
	if (pairing)
	{
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowBgAlpha(0.88f);
		ImFont* pairingFont = ImGui_GetFont(28.0f * scale);
		if (pairingFont)
			ImGui::PushFont(pairingFont);
		if (ImGui::Begin("##pairing", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
		{
			const int next = HostControllers::GetPairingPlayer();
			ImGui::TextDisabled("Pair controllers");
			ImGui::Separator();
			for (int p = 0; p < HostControllers::kPlayerCount; p++)
			{
				if (p < next)
				{
					const auto info = HostControllers::GetPlayer(p);
					ImGui::Text("  Player %d: %s, controller %d", p + 1, HostControllers::KindName(info.kind), info.pad + 1);
				}
				else if (p == next)
					ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f), "> Player %d: press A on its controller", p + 1);
				else
					ImGui::TextDisabled("  Player %d", p + 1);
			}
			ImGui::Separator();
			ImGui::TextDisabled(next > 0 ? "Menu: done (remaining players keep their controllers)" : "Press A on the controller for player 1");
		}
		ImGui::End();
		if (pairingFont)
			ImGui::PopFont();
		return;
	}
	ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.88f);
	ImFont* font = ImGui_GetFont(28.0f * scale);
	if (font)
		ImGui::PushFont(font);
	if (ImGui::Begin("##ingamemenu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
	{
		const auto& config = GetConfig();
		const bool padScreen = LatteGPUState.isDRCPrimary;
		const LatteScreenPiP pip = LatteOverlay_getScreenPiP();
		static const char* const kCorners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
		static const char* const kSizes[] = { "Small", "Medium", "Large" };
		const bool fps = config.overlay.position != ScreenPosition::kDisabled && config.overlay.fps;
		const std::string labels[kMenuCount] = {
			"Resume",
			"Pair controllers",
			std::string("Show screen: ") + (padScreen ? "GamePad" : "TV"),
			std::string(padScreen ? "TV overlay: " : "GamePad overlay: ") + (pip.enabled ? "On" : "Off"),
			fmt::format("  Overlay corner: < {} >", kCorners[std::clamp(pip.corner, 0, 3)]),
			fmt::format("  Overlay size: < {} >", kSizes[std::clamp(pip.size, 0, 2)]),
			fmt::format("  Overlay opacity: < {}% >", (int)std::lround(pip.opacity * 100.0f)),
			std::string("Touch cursor: ") + (s_touchCursor ? "On (right stick, RT touches)" : "Off"),
			std::string("FPS counter: ") + (fps ? "On" : "Off"),
			std::string("VSync: ") + (config.vsync.GetValue() != 0 ? "On" : "Off"),
			fmt::format("Volume: < {}% >", std::clamp<int>(config.tv_volume, 0, 100)),
			fmt::format("Stick deadzone (player 1): < {}% >", (int)std::lround(HostControllers::GetPlayer(0).deadzone[0] * 100.0f)),
			std::string("Motion: ") + (s_stickTilt ? "On (right stick tilts the GamePad)" : "Off"),
			_AmiiboLabel(),
			_SkylanderLabel(),
			"Exit to game list",
			"Quit Cemu",
		};
		ImGui::TextDisabled("Cemu");
		ImGui::Separator();
		const int selection = s_menuSelection;
		for (int i = 0; i < kMenuCount; i++)
		{
			if (i == selection)
				ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.0f), "> %s", labels[i].c_str());
			else
				ImGui::Text("  %s", labels[i].c_str());
		}
		ImGui::Separator();
		ImGui::TextDisabled("A: select   Left/Right: change   B: close");
	}
	ImGui::End();
	if (font)
		ImGui::PopFont();
}
