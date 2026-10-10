#pragma once

// Interface between the shared ImGui frontend ("host", replaces wxWidgets) and the platform shell that owns the window:
//   - win32/HostMainWin32.cpp: plain Win32 window, used to test the frontend on a PC
//   - uwp/HostMainUWP.cpp: UWP CoreApplication, used on Xbox (Dev Mode) and as a Windows UWP app
//
// The shell implements WindowSystem::Create() (called from main.cpp), creates the window, calls HostApp::Initialize()
// and then HostApp::RunFrame() in its event loop.

#include "interface/WindowSystem.h"

#include <functional>

namespace HostPlatform
{
	// window the launcher and the emulator render into
	WindowSystem::WindowHandleInfo GetRenderTarget();

	// asynchronously asks the user for a folder (game folder) or file (game file). The callback runs on the UI thread,
	// with an empty path if the user cancelled. Platforms without pickers call it with an empty path
	void PickFolder(std::function<void(const fs::path&)> onPicked);
	void PickFile(std::function<void(const fs::path&)> onPicked);

	// shows the platform's on-screen keyboard if it has one (Xbox) for the focused ImGui text field
	void ShowTextInput(bool show);

	// true when running on an Xbox console
	bool IsXbox();

	// ask the shell to close the app at the next opportunity
	void RequestQuit();

	// short name for the UI ("Win32", "UWP", "Xbox")
	const char* GetName();

	// downloads a URL (https, follows redirects) into memory. Blocks, call it from a worker thread. progress gets the
	// received and total byte count (total is 0 if unknown). On failure returns false and describes the problem in error
	bool HttpGet(const std::string& url, std::vector<uint8>& out, std::string& error, const std::function<void(uint64 received, uint64 total)>& progress = {});
}
