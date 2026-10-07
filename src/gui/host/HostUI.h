#pragma once

// ImGui launcher screens (game list, settings, about) for the wxWidgets-free frontend.
// All functions run on the UI thread, except PushMessage/SetStatus/RequestGameListRefresh which are thread safe.

#include <optional>

struct ImGuiContext;

namespace HostUI
{
	// sets up fonts and style for a 10-foot UI. uiScale scales everything (1.0 = 1080p)
	void Initialize(ImGuiContext* context, const fs::path& fontFile, float uiScale);
	// the scale changed (window resize). Returns true if the font atlas was rebuilt
	bool SetScale(float uiScale);

	// draws the launcher. Returns the path of a game the user picked to launch
	std::optional<fs::path> DrawLauncher();

	// error / info popups. Thread safe
	void PushMessage(std::string title, std::string text);
	// status for the window title (fps). Thread safe
	void SetStatus(bool isIdle, bool isLoading, double fps);
	double GetFPS();
	// the game list changed (title installed, paths changed). Thread safe
	void RequestGameListRefresh();

	// true if a popup or text field wants the back button / B
	bool HandleBack();
}
