#pragma once

// Platform independent part of the wxWidgets-free frontend used for UWP/Xbox (and a Win32 test build):
// initialization, an ImGui launcher (game list, settings) rendered with its own small D3D12 renderer, game launch and an
// in-game menu drawn on top of the emulator's output.
//
// Threading: everything here runs on the shell's UI thread, except DrawInGameMenu() which runs on the GPU thread
// as part of the emulator's overlay rendering.

#include "interface/WindowSystem.h"

namespace HostApp
{
	struct InitOptions
	{
		// folder for settings.xml, mlc01, shader caches, logs, controller profiles. On UWP: the app's LocalState folder
		fs::path userDataPath;
		// read only data shipped with the app (resources/, gameProfiles/). On UWP: the package install folder
		fs::path dataPath;
		// game path or title to launch directly (command line / protocol activation), may be empty
		fs::path launchPath;
	};

	// sets up paths, config, mlc and the emulator core. Shows nothing yet. On failure errorOut describes the problem
	bool Initialize(const InitOptions& options, std::string& errorOut);

	// renders one launcher frame or, while a game runs, handles the in-game menu. Returns false once the app should quit
	bool RunFrame();

	// saves settings and stops the running title (if any). Call before the process exits
	void Shutdown();

	bool IsGameRunning();

	// --- window events from the shell (UI thread) ---
	void OnResize(int physWidth, int physHeight, double dpiScale);
	void OnFocusChanged(bool hasFocus);
	void OnMouseMove(float x, float y);
	void OnMouseButton(int button, bool down);
	void OnMouseWheel(float delta);
	// platform virtual key code (VK_* on Windows), forwarded to the emulator's keyboard input as well
	void OnKey(uint32 virtualKey, bool down);
	void OnChar(uint32 utf32);
	// back button / B on Xbox. Returns true if the UI consumed it
	bool OnBackRequested();

	// --- emulator side ---
	// true while the in-game menu has the controller (the game doesn't see input then)
	bool IsInGameMenuOpen();
	// drawn on the GPU thread into the emulator's ImGui context, see LatteOverlay
	void DrawInGameMenu();
}
