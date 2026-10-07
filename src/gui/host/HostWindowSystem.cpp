// WindowSystem implementation for the wxWidgets-free frontend. WindowSystem::Create() is implemented by the platform
// shell (win32/HostMainWin32.cpp or uwp/HostMainUWP.cpp).

#include "interface/WindowSystem.h"
#include "gui/host/HostApp.h"
#include "gui/host/HostUI.h"

#include <Windows.h>

namespace
{
	WindowSystem::WindowInfo s_windowInfo{};
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo()
{
	return s_windowInfo;
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<WindowSystem::ErrorCategory> /*errorCategory*/)
{
	cemuLog_log(LogType::Force, "{}{}{}", title, title.empty() ? "" : ": ", message);
	HostUI::PushMessage(std::string(title.empty() ? "Error" : title), std::string(message));
}

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps)
{
	HostUI::SetStatus(isIdle, isLoading, fps);
}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	w = s_windowInfo.width;
	h = s_windowInfo.height;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	// the GamePad screen is shown in the main window (toggle in the in-game menu), there is no separate window
	w = 0;
	h = 0;
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	w = s_windowInfo.phys_width;
	h = s_windowInfo.phys_height;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	w = 0;
	h = 0;
}

double WindowSystem::GetWindowDPIScale()
{
	return s_windowInfo.dpi_scale;
}

double WindowSystem::GetPadDPIScale()
{
	return 1.0;
}

bool WindowSystem::IsPadWindowOpen()
{
	return false;
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	return s_windowInfo.get_keystate(key);
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes platformKey)
{
	switch (platformKey)
	{
	case PlatformKeyCodes::LCONTROL:
		return IsKeyDown((uint32)VK_LCONTROL);
	case PlatformKeyCodes::RCONTROL:
		return IsKeyDown((uint32)VK_RCONTROL);
	case PlatformKeyCodes::TAB:
		return IsKeyDown((uint32)VK_TAB);
	case PlatformKeyCodes::ESCAPE:
		return IsKeyDown((uint32)VK_ESCAPE);
	}
	return false;
}

std::string WindowSystem::GetKeyCodeName(uint32 key)
{
	if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
		return std::string(1, (char)key);
	switch (key)
	{
	case VK_SPACE: return "Space";
	case VK_RETURN: return "Enter";
	case VK_ESCAPE: return "Esc";
	case VK_TAB: return "Tab";
	case VK_SHIFT: case VK_LSHIFT: return "Shift";
	case VK_RSHIFT: return "Right Shift";
	case VK_CONTROL: case VK_LCONTROL: return "Ctrl";
	case VK_RCONTROL: return "Right Ctrl";
	case VK_MENU: case VK_LMENU: return "Alt";
	case VK_UP: return "Up";
	case VK_DOWN: return "Down";
	case VK_LEFT: return "Left";
	case VK_RIGHT: return "Right";
	case VK_BACK: return "Backspace";
	}
	if (key >= VK_F1 && key <= VK_F24)
		return fmt::format("F{}", key - VK_F1 + 1);
	if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
		return fmt::format("Num {}", key - VK_NUMPAD0);
	return fmt::format("key_{}", key);
}

bool WindowSystem::InputConfigWindowHasFocus()
{
	// pauses the game's controller input while the in-game menu is open
	return HostApp::IsInGameMenuOpen();
}

void WindowSystem::NotifyGameLoaded()
{
}

void WindowSystem::NotifyGameExited()
{
}

void WindowSystem::RefreshGameList()
{
	HostUI::RequestGameListRefresh();
}

void WindowSystem::CaptureInput(const ControllerState& /*currentState*/, const ControllerState& /*lastState*/)
{
	// hotkeys are handled by the in-game menu
}

bool WindowSystem::IsFullScreen()
{
	return s_windowInfo.is_fullscreen;
}
