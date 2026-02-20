#include "offline_audio_mixer.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

#if defined(YGOPRO_USE_MINIAUDIO)
#include "SoundBackends/miniaudio/miniaudio.h"
#elif defined(YGOPRO_USE_SFML)
#include <sfAudio/SoundBuffer.hpp>
#endif

namespace ygo {

OfflineAudioMixer& OfflineAudioMixer::Instance() {
	static OfflineAudioMixer inst;
	return inst;
}

void OfflineAudioMixer::StartPipe(const char* path) {
	StopPipe();
	if(!path || path[0] == '\0') return;
	pipe = (std::strcmp(path, "-") == 0) ? stdout : std::fopen(path, "wb");
	frac_accum = 0.0;
	active.clear();
}

void OfflineAudioMixer::StopPipe() {
	if(pipe && pipe != stdout)
		std::fclose(pipe);
	pipe = nullptr;
	active.clear();
}

void OfflineAudioMixer::OnCaptureActiveChanged(bool active_now) {
	const char* env = std::getenv("EDOPRO_AUDIO_PIPE");
	if(!env || env[0] == '\0' || env[0] == '0') return;
	if(active_now)
		StartPipe(env);
	else
		StopPipe();
}

bool OfflineAudioMixer::DecodeFileToCache(const std::string& filename, std::shared_ptr<DecodedAudio>& out) {
	auto it = cache.find(filename);
	if(it != cache.end()) {
		out = it->second;
		return true;
	}
#if defined(YGOPRO_USE_MINIAUDIO)
	ma_decoder decoder;
	ma_decoder_config config = ma_decoder_config_init(ma_format_f32, target_channels, target_sample_rate);
	if(ma_decoder_init_file(filename.c_str(), &config, &decoder) != MA_SUCCESS)
		return false;
	std::vector<float> buf;
	std::vector<float> tmp(4096 * target_channels);
	ma_uint64 framesRead = 0;
	ma_result r;
	do {
		r = ma_decoder_read_pcm_frames(&decoder, tmp.data(), 4096, &framesRead);
		if(framesRead > 0)
			buf.insert(buf.end(), tmp.begin(), tmp.begin() + framesRead * target_channels);
	} while(framesRead > 0 && r == MA_SUCCESS);
	ma_decoder_uninit(&decoder);
	if(buf.empty()) return false;
#elif defined(YGOPRO_USE_SFML)
	sf::SoundBuffer sfbuf;
	if(!sfbuf.loadFromFile(filename)) return false;
	const auto* samples = sfbuf.getSamples();
	const uint32_t src_ch   = sfbuf.getChannelCount();
	const uint32_t src_rate = sfbuf.getSampleRate();
	const size_t   src_frames = sfbuf.getSampleCount() / src_ch;
	std::vector<float> buf;
	if(src_rate == target_sample_rate && src_ch == target_channels) {
		buf.resize(src_frames * target_channels);
		for(size_t i = 0; i < buf.size(); ++i)
			buf[i] = static_cast<float>(samples[i]) / 32768.0f;
	} else {
		// Convert to float interleaved source
		std::vector<float> src(src_frames * src_ch);
		for(size_t i = 0; i < src.size(); ++i)
			src[i] = static_cast<float>(samples[i]) / 32768.0f;
		// Linear resample to target rate
		const double ratio = static_cast<double>(src_rate) / static_cast<double>(target_sample_rate);
		const size_t out_frames = static_cast<size_t>(src_frames / ratio + 0.5);
		buf.resize(out_frames * target_channels);
		for(size_t f = 0; f < out_frames; ++f) {
			const double src_pos = f * ratio;
			const size_t i0  = static_cast<size_t>(src_pos);
			const double frac = src_pos - i0;
			for(uint32_t ch = 0; ch < target_channels; ++ch) {
				const uint32_t sch = std::min(ch, src_ch - 1);
				const float s0 = (i0     < src_frames) ? src[i0     * src_ch + sch] : 0.0f;
				const float s1 = (i0 + 1 < src_frames) ? src[(i0+1) * src_ch + sch] : 0.0f;
				buf[f * target_channels + ch] = static_cast<float>(s0 * (1.0 - frac) + s1 * frac);
			}
		}
	}
	if(buf.empty()) return false;
#else
	(void)filename; (void)out;
	return false;
#endif
	auto d = std::make_shared<DecodedAudio>();
	d->samples = std::move(buf);
	out = cache.emplace(filename, std::move(d)).first->second;
	return true;
}

void OfflineAudioMixer::PlaySoundFile(const std::string& filename, bool loop, float volume) {
	std::shared_ptr<DecodedAudio> dec;
	if(!DecodeFileToCache(filename, dec)) return;
	ActiveSound a;
	a.data         = dec;
	a.total_frames = dec->samples.size() / target_channels;
	a.loop         = loop;
	a.volume       = volume;
	active.emplace_back(std::move(a));
}

void OfflineAudioMixer::MixForMillis(uint32_t delta_ms) {
	if(!pipe || delta_ms == 0) return;

	const double samples_f = static_cast<double>(target_sample_rate) * delta_ms / 1000.0;
	const auto   samples_i = static_cast<size_t>(std::floor(samples_f + frac_accum));
	frac_accum += samples_f - static_cast<double>(samples_i);
	if(samples_i == 0) return;

	const size_t n = samples_i * target_channels;
	mix_buf.assign(n, 0.0f);

	for(auto it = active.begin(); it != active.end();) {
		auto& a = *it;
		if(a.pos_frames >= a.total_frames) {
			if(a.loop) a.pos_frames %= a.total_frames;
			else { it = active.erase(it); continue; }
		}
		// Single-pass loop: advance pos_frames modularly
		const float* src = a.data->samples.data();
		for(size_t f = 0; f < samples_i; ++f) {
			const size_t si = a.pos_frames * target_channels;
			for(uint32_t ch = 0; ch < target_channels; ++ch)
				mix_buf[f * target_channels + ch] += src[si + ch] * a.volume;
			if(++a.pos_frames >= a.total_frames) {
				if(a.loop) a.pos_frames = 0;
				else { a.pos_frames = a.total_frames; break; }
			}
		}
		++it;
	}

	out_buf.resize(n);
	for(size_t i = 0; i < n; ++i) {
		const float v = std::max(-1.0f, std::min(1.0f, mix_buf[i]));
		out_buf[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
	}
	std::fwrite(out_buf.data(), sizeof(int16_t), n, pipe);
}

} // namespace ygo
