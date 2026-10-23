#include "gui/host/HostMusic.h"
#include "gui/host/HostMusicDecoder.h"
#include "gui/host/HostSounds.h"
#include "gui/host/HostTranslation.h"
#include "util/helpers/helpers.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <wrl/client.h>
#include <xaudio2.h>
#include <imgui.h>

// the game's XAudio2 backend loads xaudio2_9.dll at runtime on desktop; the launcher music is only ever on Windows 10+
// where it's always there, so both builds link it
#pragma comment(lib, "xaudio2.lib")

namespace HostMusic
{
	constexpr size_t kBufferCount = 3;
	constexpr double kBufferSeconds = 0.1;
	constexpr float kFadeSeconds = 0.6f;

	std::mutex s_mutex;
	std::condition_variable s_wake;
	std::thread s_thread;
	bool s_quit = false;
	bool s_active = false;
	bool s_settingsChanged = true;
	fs::path s_trackPath;

	fs::path GetMusicFolder()
	{
		return ActiveSettings::GetUserDataPath("theme");
	}

	static fs::path _FindTrack()
	{
		for (const char* name : { "menu_music.ogg", "menu_music.mp3", "menu_music.wav", "menu_music.flac" })
		{
			std::error_code ec;
			fs::path path = GetMusicFolder() / name;
			if (fs::is_regular_file(path, ec))
				return path;
		}
		return {};
	}

	struct VoiceDeleter
	{
		void operator()(IXAudio2Voice* voice) const { if (voice) voice->DestroyVoice(); }
	};

	// everything below is only touched by the music thread
	struct Player
	{
		Microsoft::WRL::ComPtr<IXAudio2> xaudio;
		std::unique_ptr<IXAudio2MasteringVoice, VoiceDeleter> master;
		std::unique_ptr<IXAudio2SourceVoice, VoiceDeleter> source;
		std::unique_ptr<HostMusicDecoder> decoder;
		std::vector<float> buffers[kBufferCount];
		size_t nextBuffer = 0;
		float gain = 0.0f;

		bool Open(const fs::path& path)
		{
			decoder = HostMusicDecoder::Open(path);
			if (!decoder)
			{
				cemuLog_log(LogType::Force, "Host: menu music {} can't be read (ogg, mp3, wav and flac are supported)", _pathToUtf8(path));
				return false;
			}
			HRESULT hr = XAudio2Create(xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
			IXAudio2MasteringVoice* masterVoice = nullptr;
			// default device and channel count: the console's audio setting decides stereo or 5.1
			if (SUCCEEDED(hr))
				hr = xaudio->CreateMasteringVoice(&masterVoice);
			master.reset(masterVoice);
			WAVEFORMATEX format{};
			format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
			format.nChannels = (WORD)decoder->channels;
			format.nSamplesPerSec = decoder->sampleRate;
			format.wBitsPerSample = 32;
			format.nBlockAlign = format.nChannels * 4;
			format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
			IXAudio2SourceVoice* sourceVoice = nullptr;
			if (SUCCEEDED(hr))
				hr = xaudio->CreateSourceVoice(&sourceVoice, &format);
			source.reset(sourceVoice);
			if (FAILED(hr))
			{
				cemuLog_log(LogType::Force, "Host: menu music can't open the audio output (hres {:#x})", (uint32)hr);
				Close();
				return false;
			}
			const size_t frames = (size_t)(decoder->sampleRate * kBufferSeconds);
			for (auto& buffer : buffers)
				buffer.resize(frames * decoder->channels);
			gain = 0.0f;
			source->SetVolume(0.0f);
			source->Start();
			cemuLog_log(LogType::Force, "Host: menu music {} ({} Hz, {} channels)", _pathToUtf8(path), decoder->sampleRate, decoder->channels);
			return true;
		}

		void Close()
		{
			if (source)
				source->Stop();
			source.reset();
			master.reset();
			xaudio.Reset();
			decoder.reset();
		}

		bool IsOpen() const { return source != nullptr; }

		// keeps kBufferCount buffers queued, loops at the end of the track. False if the track stopped decoding
		bool Feed()
		{
			XAUDIO2_VOICE_STATE state{};
			source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
			for (size_t queued = state.BuffersQueued; queued < kBufferCount; queued++)
			{
				std::vector<float>& buffer = buffers[nextBuffer];
				const size_t capacity = buffer.size() / decoder->channels;
				size_t frames = 0;
				bool rewound = false;
				while (frames < capacity)
				{
					const size_t read = decoder->Read(buffer.data() + frames * decoder->channels, capacity - frames);
					if (read > 0)
					{
						frames += read;
						rewound = false;
						continue;
					}
					// end of the track: start over without a gap. Twice in a row means nothing decodes at all
					if (rewound || !decoder->Rewind())
						break;
					rewound = true;
				}
				if (frames == 0)
					return false;
				XAUDIO2_BUFFER xbuffer{};
				xbuffer.AudioBytes = (UINT32)(frames * decoder->channels * sizeof(float));
				xbuffer.pAudioData = (const BYTE*)buffer.data();
				if (FAILED(source->SubmitSourceBuffer(&xbuffer)))
					return false;
				nextBuffer = (nextBuffer + 1) % kBufferCount;
			}
			return true;
		}
	};

	static void _Run()
	{
		SetThreadName("HostMusic");
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		Player player;
		fs::path openPath;
		bool failed = false; // the file didn't play: don't retry until the settings change
		auto last = std::chrono::steady_clock::now();
		std::unique_lock lock(s_mutex);
		while (!s_quit)
		{
			if (s_settingsChanged)
			{
				s_settingsChanged = false;
				s_trackPath = GetConfig().host_menu_music.GetValue() ? _FindTrack() : fs::path();
				failed = false;
			}
			const bool wanted = s_active && !s_trackPath.empty() && !failed;
			const float target = wanted ? std::clamp(GetConfig().host_menu_music_volume.GetValue(), 0, 100) / 100.0f : 0.0f;
			const fs::path trackPath = s_trackPath;
			lock.unlock();

			const auto now = std::chrono::steady_clock::now();
			const float dt = std::chrono::duration<float>(now - last).count();
			last = now;
			if (player.IsOpen() && openPath != trackPath)
				player.Close(); // another file (or switched off): the new one fades in from silence
			if (wanted && !player.IsOpen())
			{
				failed = !player.Open(trackPath);
				openPath = trackPath;
			}
			if (player.IsOpen())
			{
				const float step = dt / kFadeSeconds;
				player.gain = target > player.gain ? std::min(target, player.gain + step) : std::max(target, player.gain - step);
				player.source->SetVolume(player.gain * player.gain); // squared: a fade sounds even to the ear
				if (!wanted && player.gain <= 0.0f)
					player.Close(); // faded out for a game: the game gets the audio device to itself
				else if (!player.Feed())
				{
					cemuLog_log(LogType::Force, "Host: menu music stopped, the file doesn't decode");
					player.Close();
					failed = true;
				}
			}

			lock.lock();
			if (player.IsOpen())
				s_wake.wait_for(lock, std::chrono::milliseconds(20));
			else
				s_wake.wait(lock, [&] { return s_quit || s_settingsChanged || (s_active && !s_trackPath.empty() && !failed); });
			if (!player.IsOpen())
				last = std::chrono::steady_clock::now();
		}
		lock.unlock();
		player.Close();
		CoUninitialize();
	}

	void SetActive(bool active)
	{
		std::unique_lock lock(s_mutex);
		if (!s_thread.joinable() && !s_quit)
			s_thread = std::thread(_Run);
		if (s_active == active)
			return;
		s_active = active;
		s_wake.notify_all();
	}

	void ApplySettings()
	{
		std::unique_lock lock(s_mutex);
		s_settingsChanged = true;
		s_wake.notify_all();
	}

	void Shutdown()
	{
		{
			std::unique_lock lock(s_mutex);
			s_quit = true;
			s_wake.notify_all();
		}
		if (s_thread.joinable())
			s_thread.join();
	}

	fs::path GetTrackPath()
	{
		std::unique_lock lock(s_mutex);
		return s_trackPath;
	}

	bool DrawSettings()
	{
		auto& config = GetConfig();
		bool changed = false;
		bool music = config.host_menu_music.GetValue();
		if (ImGui::Checkbox(_TR("Menu music"), &music))
		{
			config.host_menu_music = music;
			changed = true;
			ApplySettings();
		}
		if (!music)
			return changed;
		ImGui::SameLine();
		int volume = config.host_menu_music_volume.GetValue();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		if (ImGui::SliderInt("##musicvolume", &volume, 0, 100, "%d%%"))
			config.host_menu_music_volume = volume; // the music thread picks it up while playing
		if (ImGui::IsItemDeactivatedAfterEdit())
			changed = true;
		ImGui::SameLine();
		if (ImGui::Button(_TR("Look for the file again")))
		{
			ApplySettings();
			HostSounds::Reload(); // the own sounds in theme/sfx too
		}
		const fs::path track = GetTrackPath();
		if (track.empty())
			ImGui::TextDisabled("Menu music: put menu_music.ogg, .mp3, .wav or .flac into %s", _pathToUtf8(GetMusicFolder()).c_str());
		else
			ImGui::TextDisabled("Playing %s", _pathToUtf8(track.filename()).c_str());
		return changed;
	}
}
