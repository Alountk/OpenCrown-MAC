// dcp_run's SDL3 audio sink: mixes every open AudioOut port down to float stereo 48 kHz into one SDL audio stream.
// Linux: submit() blocks while more than the queue target is queued, which is what paces CRI's mixer thread.
// macOS: submit() returns at real-time pace (like the PS4); a cushion of silence in the SDL stream absorbs jitter.
#include "sdl_audio.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "hle/audio.h"

namespace {

constexpr int kQueueBatches = 4;  // ~21 ms at 256 frames: latency vs. underrun margin
// The device pulls whole buffers of its own size from the stream (PulseAudio under WSLg: 768 frames). With a target
// of only 4 batches (1024 frames) a pull often found less than one buffer queued and SDL padded the rest with
// silence: ~10 dropouts of ~5 ms per second (crackle). So the target is at least kDeviceBuffers device buffers.
constexpr int kDeviceBuffers = 3;

class SdlSink final : public dcp::hle::AudioSink {
public:
    SdlSink(SDL_AudioStream* s, int target_frames) : s_(s), target_(target_frames * 8) {}
    void open(int port, int channels, int) override { ch_[port] = channels; }
    void close(int port) override { ch_[port] = 0; }
    void submit(int count, const int* ports, const float* const* data, int frames) override {
        mix_.assign(size_t(frames) * 2, 0.0f);
        for (int i = 0; i < count; ++i) {
            if (!data[i]) continue;
            int ch = ch_[ports[i]];
            for (int f = 0; f < frames; ++f) {
                const float* x = data[i] + size_t(f) * ch;
                float l = x[0], r = ch >= 2 ? x[1] : x[0];
                if (ch == 8) {  // FL FR C LFE SL SR BL BR -> stereo
                    l += 0.707f * x[2] + 0.707f * x[4] + 0.5f * x[6];
                    r += 0.707f * x[2] + 0.707f * x[5] + 0.5f * x[7];
                }
                mix_[f * 2] += l;
                mix_[f * 2 + 1] += r;
            }
        }
        for (float& v : mix_) v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
        if (tone_)  // DCP_AUDIO_TONE=1: a 440 Hz sine instead of the game mix, same pacing and SDL path (output check)
            for (int f = 0; f < frames; ++f, ++tone_pos_)
                mix_[f * 2] = mix_[f * 2 + 1] = 0.25f * std::sin(6.2831853f * 440.0f * float(tone_pos_ % 48000) / 48000.0f);
        int bytes = int(mix_.size() * sizeof(float));
        if (raw_) std::fwrite(mix_.data(), 1, size_t(bytes), raw_);  // DCP_AUDIO_RAW=path: f32 stereo 48 kHz, what SDL gets
#ifdef __APPLE__
        if (!primed_) {  // fixed cushion of silence: the queue then hovers around target_ while calls keep real-time pace
            std::vector<float> pad(size_t(target_ / 4), 0.0f);
            SDL_PutAudioStreamData(s_, pad.data(), int(pad.size() * sizeof(float)));
            primed_ = true;
        }
        SDL_PutAudioStreamData(s_, mix_.data(), bytes);
        int queued = SDL_GetAudioStreamQueued(s_);
        // Like the PS4 (and the paced sink), the call returns when this batch's time has elapsed, not when the queue is
        // short: CRI's streaming threads misbehave (dropouts in the voices) if the mixer runs ahead of real time. A
        // gentle correction toward the cushion keeps the host clock and the device clock from drifting apart.
        uint64_t now = SDL_GetTicksNS();
        int64_t step = int64_t(1000000000ll) * frames / 48000;
        int64_t err_ns = int64_t(queued - target_) / 8 * 1000000000ll / 48000;  // queue surplus in time
        if (next_ == 0 || int64_t(now) - next_ > 100000000ll) next_ = int64_t(now);
        next_ += step + err_ns / 64;
        if (next_ > int64_t(now)) SDL_DelayPrecise(uint64_t(next_ - int64_t(now)));
        stat(queued, bytes, SDL_GetTicksNS() - now);
#else
        // Linux / Steam Deck: unchanged: block while more than the queue target is queued, which paces CRI's mixer thread.
        int queued = SDL_GetAudioStreamQueued(s_);
        uint64_t t0 = SDL_GetTicksNS();
        while (queued > target_) SDL_Delay(1), queued = SDL_GetAudioStreamQueued(s_);
        SDL_PutAudioStreamData(s_, mix_.data(), bytes);
        stat(queued, bytes, SDL_GetTicksNS() - t0);
#endif
    }

private:
    // Once a second: batches put, queue depth before each put (min / max, bytes), puts that found the queue empty (the
    // device had already drained it: an underrun) and time spent waiting. DCP_AUDIO_LOG=1 prints every second; without
    // it only seconds with underruns are printed (past start-up; 30 lines, then one a minute), so a bug report's log
    // says whether a crackle was our feed running dry or something past SDL.
    void stat(int queued, int bytes, uint64_t wait_ns) {
        uint64_t now = SDL_GetTicksNS();
        if (!start_) start_ = now;
        if (!n_) t0_ = now, qmin_ = queued, qmax_ = queued;
        ++n_, empty_ += queued == 0, wait_ += wait_ns;
        qmin_ = queued < qmin_ ? queued : qmin_, qmax_ = queued > qmax_ ? queued : qmax_;
        if (now - t0_ >= 1000000000ull) {
            bool under = empty_ > 0 && now - start_ > 3000000000ull;
            if (under) ++under_secs_;
            if (log_ || (under && (under_secs_ <= 30 || now - last_under_log_ >= 60000000000ull))) {
                std::fprintf(stderr, "audio: %d batches (%d frames) in %.3f s, queued %d..%d B, empty %d, wait %.1f ms%s\n",
                             n_, n_ * bytes / 8, (now - t0_) / 1e9, qmin_, qmax_, empty_, wait_ / 1e6,
                             under ? " (underrun)" : "");
                if (under) last_under_log_ = now;
            }
            n_ = empty_ = 0, wait_ = 0;
        }
    }

    SDL_AudioStream* s_;
    int target_;  // bytes (f32 stereo)
    bool log_ = std::getenv("DCP_AUDIO_LOG") != nullptr;
    bool tone_ = std::getenv("DCP_AUDIO_TONE") != nullptr;
    FILE* raw_ = std::getenv("DCP_AUDIO_RAW") ? std::fopen(std::getenv("DCP_AUDIO_RAW"), "wb") : nullptr;
    uint64_t tone_pos_ = 0;
    bool primed_ = false;
    int64_t next_ = 0;
    int n_ = 0, empty_ = 0, qmin_ = 0, qmax_ = 0, under_secs_ = 0;
    uint64_t t0_ = 0, wait_ = 0, start_ = 0, last_under_log_ = 0;
    int ch_[8] = {};
    std::vector<float> mix_;
};

}  // namespace

bool install_sdl_audio() {
    // Name shown by PulseAudio / PipeWire volume controls (pavucontrol, the Plasma / GNOME mixers): the client takes
    // the app name, the stream the stream name (otherwise SDL's generic defaults).
    SDL_SetHint(SDL_HINT_APP_NAME, "Dragon's Crown Pro");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Game");
    // PulseAudio first (on SteamOS / PipeWire systems that is pipewire-pulse, the path Proton's audio takes). SDL's
    // native PipeWire backend crackled on the Deck OLED and a Steam Machine (bug reports 11, 12, 2026-10-19): it fills
    // each quantum in PipeWire's real-time thread from the stream our normal-priority mixer thread feeds, and a late
    // fill is played as a gap. A normal-priority hint, so SDL_AUDIO_DRIVER in the environment still wins.
#ifndef __APPLE__
    // Linux backends only; on macOS SDL picks CoreAudio by itself.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "pulseaudio,pipewire,alsa");
#endif
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: SDL_InitSubSystem: %s (falling back to silent paced output)\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s) {
        std::fprintf(stderr, "audio: open: %s (falling back to silent paced output)\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(s);
    SDL_AudioSpec dev{};
    int dev_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s), &dev, &dev_frames))
        std::fprintf(stderr, "audio: device %s, %d Hz, %d ch, format 0x%x, %d sample frames per buffer\n",
                     SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s)), dev.freq, dev.channels, unsigned(dev.format),
                     dev_frames);
    int target = kQueueBatches * 256;
    if (dev_frames * kDeviceBuffers > target) target = dev_frames * kDeviceBuffers;
    if (const char* e = std::getenv("DCP_AUDIO_QUEUE"); e && std::atoi(e) > 0) target = std::atoi(e);  // frames, for tests
    dcp::hle::set_audio_sink(std::make_unique<SdlSink>(s, target));
    std::fprintf(stderr, "audio: SDL3 %s, f32 stereo 48 kHz, queue target %d frames\n", SDL_GetCurrentAudioDriver(), target);
    return true;
}
