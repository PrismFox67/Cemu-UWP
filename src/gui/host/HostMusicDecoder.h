#pragma once

#include <filesystem>
#include <memory>

// streaming audio file decoder for the launcher music (HostMusic). Built on the public-domain single-file decoders in
// thirdparty/ (stb_vorbis, dr_mp3, dr_wav, dr_flac), kept in their own translation unit without the precompiled header
class HostMusicDecoder
{
public:
	virtual ~HostMusicDecoder() = default;
	// decodes up to frameCount frames of interleaved float samples, returns the number of frames, 0 at the end
	virtual size_t Read(float* out, size_t frameCount) = 0;
	virtual bool Rewind() = 0;

	unsigned channels = 0;
	unsigned sampleRate = 0;

	// picks the decoder by file extension (.ogg, .mp3, .wav, .flac), nullptr if the file can't be opened
	static std::unique_ptr<HostMusicDecoder> Open(const std::filesystem::path& path);
};
