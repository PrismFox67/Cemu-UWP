#include "gui/host/HostApp.h"
#include "gui/host/HostPlatform.h"
#include "gui/host/HostUI.h"
#include "gui/host/HostGamepad.h"
#include "gui/host/HostControllers.h"
#include "gui/host/LauncherRenderer.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Renderer/D3D12/D3D12Renderer.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
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
		kMenuShowFPS,
		kMenuExit,
		kMenuCount,
	};

	HostApp::InitOptions s_options;
	State s_state = State::Launcher;
	ImGuiContext* s_launcherContext = nullptr;
	std::unique_ptr<LauncherRenderer> s_launcher;
	bool s_launcherFailed = false;
	std::optional<fs::path> s_pendingLaunch;

	// in-game menu, read by the GPU thread
	std::atomic_bool s_menuOpen = false;
	std::atomic_int s_menuSelection = 0;
	uint32 s_lastPadButtons = 0;
	std::chrono::steady_clock::time_point s_comboStart{};
	bool s_comboActive = false;
	bool s_comboConsumed = false;

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
		std::string error;
		if (!_PrepareTitle(path, error))
		{
			HostUI::PushMessage("Can't start the game", error);
			return;
		}
		cemuLog_log(LogType::Force, "Host: launching {}", _pathToUtf8(path));

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
		CafeSystem::LaunchForegroundTitle();
		s_state = State::Running;
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
		HostGamepad::FeedImGui(HostGamepad::Poll());

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
		if (launch)
			_LaunchGame(*launch);
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
			HostControllers::BeginPairing();
			break;
		case kMenuGamePadScreen:
			// same state the screen swap hotkey (Ctrl+Tab in the wx frontend) flips
			LatteGPUState.isDRCPrimary = !LatteGPUState.isDRCPrimary;
			break;
		case kMenuShowFPS:
		{
			const bool show = !(config.overlay.position != ScreenPosition::kDisabled && config.overlay.fps);
			config.overlay.position = show ? ScreenPosition::kTopLeft : ScreenPosition::kDisabled;
			config.overlay.fps = show;
			GetConfigHandle().Save();
			break;
		}
		case kMenuExit:
			s_menuOpen = false;
			s_state = State::Quitting;
			break;
		}
	}

	void _UpdateInGame()
	{
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
			}
		}
		else
			s_comboActive = false;

		if (!s_menuOpen)
			return;
		if (pressed & HostGamepad::kUp)
			s_menuSelection = (s_menuSelection + kMenuCount - 1) % kMenuCount;
		if (pressed & HostGamepad::kDown)
			s_menuSelection = (s_menuSelection + 1) % kMenuCount;
		if (pressed & HostGamepad::kB)
			s_menuOpen = false;
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

	const fs::path settingsPath = ActiveSettings::GetConfigPath("settings.xml");
	GetConfigHandle().SetFilename(settingsPath.generic_wstring());
	const bool firstStart = !fs::exists(settingsPath, ec);
	NetworkConfig::LoadOnce();
	if (!firstStart)
		GetConfigHandle().Load();
	GetConfigHandle().Save();

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
	ImGui::SetCurrentContext(previous);

	if (!options.launchPath.empty())
		s_pendingLaunch = options.launchPath;
	return true;
}

bool HostApp::RunFrame()
{
	switch (s_state)
	{
	case State::Launcher:
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
		_UpdateInGame();
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
	s_launcher.reset();
	if (s_launcherContext)
	{
		ImGui::DestroyContext(s_launcherContext);
		s_launcherContext = nullptr;
	}
	GetConfigHandle().Save();
	InputManager::instance().save();
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
		}
		else if (s_menuOpen)
		{
			if (virtualKey == VK_UP)
				s_menuSelection = (s_menuSelection + kMenuCount - 1) % kMenuCount;
			else if (virtualKey == VK_DOWN)
				s_menuSelection = (s_menuSelection + 1) % kMenuCount;
			else if (virtualKey == VK_RETURN)
				_ExecuteMenuItem(s_menuSelection);
			else if (virtualKey == VK_ESCAPE)
				s_menuOpen = false;
		}
		return;
	}
	if (!s_launcherContext)
		return;
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
	const bool pairing = HostControllers::IsPairing();
	if (!s_menuOpen && !pairing)
		return;
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const float scale = std::max(1.0f, viewport->Size.y / 1080.0f);
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
			ImGui::TextDisabled(next > 0 ? "Menu: done (remaining players are disconnected)" : "Press A on the controller for player 1");
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
		const bool fps = config.overlay.position != ScreenPosition::kDisabled && config.overlay.fps;
		const std::string labels[kMenuCount] = {
			"Resume",
			"Pair controllers",
			std::string("Show screen: ") + (padScreen ? "GamePad" : "TV"),
			std::string("FPS counter: ") + (fps ? "On" : "Off"),
			"Save and exit Cemu",
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
		ImGui::TextDisabled("A: select   B: close");
	}
	ImGui::End();
	if (font)
		ImGui::PopFont();
}
