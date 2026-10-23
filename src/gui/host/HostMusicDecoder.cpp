#include "HostMusicDecoder.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

#pragma warning(push, 0)
#define DR_MP3_IMPLEMENTATION
#include "thirdparty/dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "thirdparty/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "thirdparty/dr_flac.h"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "thirdparty/stb_vorbis.c"
#pragma warning(pop)

namespace
{
	class VorbisDecoder : public HostMusicDecoder
	{
	public:
		std::vector<unsigned char> data; // stb_vorbis reads from memory, the compressed file stays loaded
		stb_vorbis* vorbis = nullptr;
		~VorbisDecoder() override { if (vorbis) stb_vorbis_close(vorbis); }
		size_t Read(float* out, size_t frameCount) override
		{
			return (size_t)stb_vorbis_get_samples_float_interleaved(vorbis, (int)channels, out, (int)(frameCount * channels));
		}
		bool Rewind() override { return stb_vorbis_seek_start(vorbis) != 0; }
	};

	class Mp3Decoder : public HostMusicDecoder
	{
	public:
		drmp3 mp3{};
		bool open = false;
		~Mp3Decoder() override { if (open) drmp3_uninit(&mp3); }
		size_t Read(float* out, size_t frameCount) override { return (size_t)drmp3_read_pcm_frames_f32(&mp3, frameCount, out); }
		bool Rewind() override { return drmp3_seek_to_pcm_frame(&mp3, 0) != 0; }
	};

	class WavDecoder : public HostMusicDecoder
	{
	public:
		drwav wav{};
		bool open = false;
		~WavDecoder() override { if (open) drwav_uninit(&wav); }
		size_t Read(float* out, size_t frameCount) override { return (size_t)drwav_read_pcm_frames_f32(&wav, frameCount, out); }
		bool Rewind() override { return drwav_seek_to_pcm_frame(&wav, 0) != 0; }
	};

	class FlacDecoder : public HostMusicDecoder
	{
	public:
		drflac* flac = nullptr;
		~FlacDecoder() override { if (flac) drflac_close(flac); }
		size_t Read(float* out, size_t frameCount) override { return (size_t)drflac_read_pcm_frames_f32(flac, frameCount, out); }
		bool Rewind() override { return drflac_seek_to_pcm_frame(flac, 0) != 0; }
	};
}

std::unique_ptr<HostMusicDecoder> HostMusicDecoder::Open(const std::filesystem::path& path)
{
	std::wstring ext = path.extension().wstring();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
	std::unique_ptr<HostMusicDecoder> result;
	if (ext == L".ogg")
	{
		auto d = std::make_unique<VorbisDecoder>();
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return nullptr;
		d->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		int error = 0;
		d->vorbis = stb_vorbis_open_memory(d->data.data(), (int)d->data.size(), &error, nullptr);
		if (!d->vorbis)
			return nullptr;
		const stb_vorbis_info info = stb_vorbis_get_info(d->vorbis);
		d->channels = (unsigned)info.channels;
		d->sampleRate = info.sample_rate;
		result = std::move(d);
	}
	else if (ext == L".mp3")
	{
		auto d = std::make_unique<Mp3Decoder>();
		if (!drmp3_init_file_w(&d->mp3, path.c_str(), nullptr))
			return nullptr;
		d->open = true;
		d->channels = d->mp3.channels;
		d->sampleRate = d->mp3.sampleRate;
		result = std::move(d);
	}
	else if (ext == L".wav")
	{
		auto d = std::make_unique<WavDecoder>();
		if (!drwav_init_file_w(&d->wav, path.c_str(), nullptr))
			return nullptr;
		d->open = true;
		d->channels = d->wav.channels;
		d->sampleRate = d->wav.sampleRate;
		result = std::move(d);
	}
	else if (ext == L".flac")
	{
		auto d = std::make_unique<FlacDecoder>();
		d->flac = drflac_open_file_w(path.c_str(), nullptr);
		if (!d->flac)
			return nullptr;
		d->channels = d->flac->channels;
		d->sampleRate = d->flac->sampleRate;
		result = std::move(d);
	}
	if (!result || result->channels == 0 || result->channels > 8 || result->sampleRate < 8000 || result->sampleRate > 192000)
		return nullptr;
	return result;
}
