#pragma once

// launcher sound effects: Kenney's CC0 Interface Sounds in resources/sfx, each replaceable by
// <user>/theme/sfx/<name>.ogg / .wav. Own XAudio2 instance, released while a game runs
namespace HostSounds
{
	enum class Sfx { Move, Select, Back, Tab, Error, Count };

	// main thread only, like SetActive
	void Play(Sfx sfx);
	// false while a game runs: nothing plays and the audio device is released
	void SetActive(bool active);
	// re-reads the files (after an upload) on the next sound
	void Reload();
	void Shutdown();
	// Settings > Look: on/off and volume. True when a setting changed that should be saved
	bool DrawSettings();
}
