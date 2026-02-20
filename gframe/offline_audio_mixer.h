#ifndef OFFLINE_AUDIO_MIXER_H
#define OFFLINE_AUDIO_MIXER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>
#include <map>

namespace ygo {

class OfflineAudioMixer {
public:
	static OfflineAudioMixer& Instance();

	// Called on replay start/end. Reads EDOPRO_AUDIO_PIPE; no-ops if unset.
	void OnCaptureActiveChanged(bool active);

	// Enqueue a sound for mixing. Called from SoundManager.
	void PlaySoundFile(const std::string& filename, bool loop, float volume);

	// Produce audio for delta_ms ms and write raw s16le stereo 44100 to pipe.
	void MixForMillis(uint32_t delta_ms);

private:
	OfflineAudioMixer() = default;
	~OfflineAudioMixer() { StopPipe(); }

	void StartPipe(const char* path);
	void StopPipe();

	struct DecodedAudio {
		std::vector<float> samples; // interleaved float32
	};
	struct ActiveSound {
		std::shared_ptr<DecodedAudio> data;
		size_t pos_frames{0};
		size_t total_frames{0};
		bool   loop{false};
		float  volume{1.0f};
	};

	static constexpr uint32_t target_sample_rate = 44100;
	static constexpr uint32_t target_channels    = 2;

	bool DecodeFileToCache(const std::string& filename, std::shared_ptr<DecodedAudio>& out);

	std::map<std::string, std::shared_ptr<DecodedAudio>> cache;
	std::vector<ActiveSound> active;
	// Persistent scratch buffers – avoids per-frame heap allocation
	std::vector<float>   mix_buf;
	std::vector<int16_t> out_buf;

	FILE*  pipe{nullptr};
	double frac_accum{0.0};
};

} // namespace ygo

#endif // OFFLINE_AUDIO_MIXER_H
