#pragma once

// Graphic packs for the host frontend: downloads the community graphic packs (github.com/cemu-project/cemu_graphic_packs,
// same source and folder as the desktop "Download latest community graphic packs") and keeps the enabled packs and
// selected presets in settings.xml. Packs are applied when a game starts (GraphicPack2::ActivateForCurrentTitle), so the
// UI only edits them while no game runs.

namespace HostGraphicPacks
{
	enum class State
	{
		Idle,
		Checking,
		Downloading,
		Extracting,
		Done,
		Failed,
	};

	struct Status
	{
		State state = State::Idle;
		std::string text; // what happened, for the UI
		float progress = -1.0f; // 0..1, negative if unknown
	};

	// starts checking for and installing the latest community graphic packs on a worker thread. Does nothing if an
	// update is already running
	void StartUpdate();
	Status GetStatus();
	bool IsBusy();

	// version (release name) of the installed community graphic packs, empty if none are installed
	std::string GetInstalledVersion();

	// call once per frame on the UI thread: reloads the pack list after an update finished
	void Update();

	// stores the enabled state and active presets of all packs in settings.xml
	void SaveToConfig();
}
