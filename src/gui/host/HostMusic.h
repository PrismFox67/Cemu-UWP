#pragma once

// launcher menu music: a user-supplied track from <user>/theme/menu_music.(ogg|mp3|wav|flac), streamed and looped
// through its own XAudio2 instance. Nothing is bundled: without a file the launcher is silent
namespace HostMusic
{
	// call every frame: true while a launcher screen is shown, false while a game runs. Fades in/out on changes
	void SetActive(bool active);
	// re-reads the on/off and volume settings (and looks for the file again)
	void ApplySettings();
	void Shutdown();
	// Settings > Look: on/off, volume, which file plays. True when a setting changed that should be saved
	bool DrawSettings();

	// the file that is played, empty if none was found
	fs::path GetTrackPath();
	fs::path GetMusicFolder();
}
