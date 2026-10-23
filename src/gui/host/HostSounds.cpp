#include "gui/host/HostSounds.h"
#include "gui/host/HostMusic.h"
#include "gui/host/HostMusicDecoder.h"
#include "gui/host/HostTranslation.h"

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

#include <wrl/client.h>
#include <xaudio2.h>
#include <imgui.h>

#pragma comment(lib, "xaudio2.lib")

namespace HostSounds
{
	struct VoiceDeleter
	{
		void operator()(IXAudio2Voice* voice) const { if (voice) voice->DestroyVoice(); }
	};

	struct Sound
	{
		std::vector<float> samples;
		std::unique_ptr<IXAudio2SourceVoice, VoiceDeleter> voice;
	};

	constexpr const char* kNames[(size_t)Sfx::Count] = { "move", "select", "back", "tab", "error" };
	// relative loudness: moving happens constantly and should stay subtle
	constexpr float kGain[(size_t)Sfx::Count] = { 0.5f, 0.8f, 0.8f, 0.7f, 0.8f };

	bool s_active = false;
	bool s_opened = false; // tried to open the device and load the sounds
	Microsoft::WRL::ComPtr<IXAudio2> s_xaudio;
	std::unique_ptr<IXAudio2MasteringVoice, VoiceDeleter> s_master;
	Sound s_sounds[(size_t)Sfx::Count];

	static void _Close()
	{
		for (Sound& sound : s_sounds)
		{
			sound.voice.reset();
			sound.samples.clear();
		}
		s_master.reset();
		s_xaudio.Reset();
		s_opened = false;
	}

	static void _Load(Sound& sound, const char* name)
	{
		// the user's file first, then the bundled one
		std::unique_ptr<HostMusicDecoder> decoder;
		for (const char* ext : { ".ogg", ".wav" })
		{
			std::error_code ec;
			const fs::path path = HostMusic::GetMusicFolder() / "sfx" / (std::string(name) + ext);
			if (fs::is_regular_file(path, ec) && (decoder = HostMusicDecoder::Open(path)))
				break;
		}
		if (!decoder)
			decoder = HostMusicDecoder::Open(ActiveSettings::GetDataPath("resources/sfx/{}.ogg", name));
		if (!decoder)
		{
			cemuLog_log(LogType::Force, "Host: menu sound {} not found", name);
			return;
		}
		// short effects: decode completely, at most 3 seconds
		const size_t maxFrames = decoder->sampleRate * 3;
		std::vector<float> samples(maxFrames * decoder->channels);
		size_t frames = 0;
		while (frames < maxFrames)
		{
			const size_t read = decoder->Read(samples.data() + frames * decoder->channels, maxFrames - frames);
			if (read == 0)
				break;
			frames += read;
		}
		if (frames == 0)
			return;
		samples.resize(frames * decoder->channels);
		WAVEFORMATEX format{};
		format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		format.nChannels = (WORD)decoder->channels;
		format.nSamplesPerSec = decoder->sampleRate;
		format.wBitsPerSample = 32;
		format.nBlockAlign = format.nChannels * 4;
		format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
		IXAudio2SourceVoice* voice = nullptr;
		if (FAILED(s_xaudio->CreateSourceVoice(&voice, &format)))
			return;
		sound.voice.reset(voice);
		sound.samples = std::move(samples);
	}

	static bool _Open()
	{
		if (s_opened)
			return s_master != nullptr;
		s_opened = true;
		IXAudio2MasteringVoice* master = nullptr;
		if (FAILED(XAudio2Create(s_xaudio.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(s_xaudio->CreateMasteringVoice(&master)))
		{
			cemuLog_log(LogType::Force, "Host: menu sounds can't open the audio output");
			s_xaudio.Reset();
			return false;
		}
		s_master.reset(master);
		for (size_t i = 0; i < (size_t)Sfx::Count; i++)
			_Load(s_sounds[i], kNames[i]);
		return true;
	}

	void Play(Sfx sfx)
	{
		if (!s_active || !GetConfig().host_menu_sounds.GetValue() || !_Open())
			return;
		Sound& sound = s_sounds[(size_t)sfx];
		if (!sound.voice)
			return;
		// restart: a sound pressed again while it plays starts over
		sound.voice->Stop();
		sound.voice->FlushSourceBuffers();
		XAUDIO2_BUFFER buffer{};
		buffer.AudioBytes = (UINT32)(sound.samples.size() * sizeof(float));
		buffer.pAudioData = (const BYTE*)sound.samples.data();
		buffer.Flags = XAUDIO2_END_OF_STREAM;
		const float volume = std::clamp(GetConfig().host_menu_sounds_volume.GetValue(), 0, 100) / 100.0f;
		sound.voice->SetVolume(volume * volume * kGain[(size_t)sfx]);
		if (SUCCEEDED(sound.voice->SubmitSourceBuffer(&buffer)))
			sound.voice->Start();
	}

	void SetActive(bool active)
	{
		if (active == s_active)
			return;
		s_active = active;
		if (!active)
			_Close(); // the game gets the audio device to itself
	}

	void Reload()
	{
		_Close();
	}

	void Shutdown()
	{
		s_active = false;
		_Close();
	}

	bool DrawSettings()
	{
		auto& config = GetConfig();
		bool changed = false;
		bool sounds = config.host_menu_sounds.GetValue();
		if (ImGui::Checkbox(_TR("Menu sounds"), &sounds))
		{
			config.host_menu_sounds = sounds;
			changed = true;
		}
		if (!sounds)
			return changed;
		ImGui::SameLine();
		int volume = config.host_menu_sounds_volume.GetValue();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		if (ImGui::SliderInt("##soundsvolume", &volume, 0, 100, "%d%%"))
			config.host_menu_sounds_volume = volume;
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			changed = true;
			Play(Sfx::Move); // hear the new volume
		}
		ImGui::TextDisabled("Own sounds: move, select, back, tab or error .ogg / .wav in %s", _pathToUtf8(HostMusic::GetMusicFolder() / "sfx").c_str());
		return changed;
	}
}
