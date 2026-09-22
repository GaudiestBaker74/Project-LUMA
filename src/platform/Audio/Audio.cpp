// Platform::Audio — see Audio.h.
//
// SDL3 backend: the device is opened as a *device stream*
// (SDL_OpenAudioDeviceStream) whose get-callback pulls from our ring buffer
// and feeds SDL with SDL_PutAudioStreamData; SDL resamples to the physical
// device format. Pulling from a ring in the callback keeps the audio thread
// (producer) non-blocking and lets the same ring serve headless tests.
#include "platform/Audio/Audio.h"

#include "platform/Audio/RingBuffer.h"
#include "platform/Log/Log.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace Platform::Audio {

namespace {

struct State {
    std::mutex mutex;
    bool initialized = false;
    bool enabled = false;
    bool virtualMode = false;
    std::string status = "not initialized";
    std::string deviceName;
    int deviceFreqOut = 0;
    int deviceChannelsOut = 0;
    int inputFreq = 32000;

    SDL_AudioStream* stream = nullptr;
    RingBuffer* ring = nullptr;

    // Producer counters (audio thread).
    std::atomic<uint64_t> framesPushed{0};
    std::atomic<uint64_t> framesDropped{0};
    std::atomic<uint64_t> framesConsumed{0};

    std::atomic<bool> isPaused{false};
    std::atomic<float> gain{1.0f};

    DeviceTick tick = nullptr;
    void* tickUser = nullptr;
    std::atomic<int> lastRequestFrames{0};

    // One-shot voices (M8.5 prep): decoded PCM clips mixed on top of the ring
    // (music) output. Guarded by their own mutex — the device callback mixes
    // them without touching the state mutex (no deadlock with init/shutdown).
    struct OneShotVoice {
        std::vector<int16_t> pcm; // interleaved stereo
        size_t pos = 0;           // consumed SAMPLES (2 per frame)
        float gain = 1.0f;
    };
    std::mutex voicesMutex;
    std::vector<OneShotVoice> voices;
};

State g;

// Mixes the active one-shot voices into `buf` (frames stereo samples, already
// containing the ring output) with saturation, advancing/retiring them.
// Returns true if any voice contributed samples. Caller holds no locks.
bool mixOneShots(int16_t* buf, size_t frames) {
    std::lock_guard<std::mutex> lock(g.voicesMutex);
    if (g.voices.empty() || frames == 0) {
        return false;
    }
    bool any = false;
    for (size_t v = 0; v < g.voices.size();) {
        State::OneShotVoice& voice = g.voices[v];
        const size_t want = frames * 2;
        const size_t left = voice.pcm.size() - voice.pos;
        const size_t n = left < want ? left : want;
        for (size_t i = 0; i < n; ++i) {
            const int32_t mixed = static_cast<int32_t>(buf[i]) +
                                  static_cast<int32_t>(voice.pcm[voice.pos + i] * voice.gain);
            buf[i] = static_cast<int16_t>(mixed > 32767 ? 32767 : (mixed < -32768 ? -32768 : mixed));
        }
        voice.pos += n;
        if (n > 0) {
            any = true;
        }
        if (voice.pos >= voice.pcm.size()) {
            g.voices[v] = std::move(g.voices.back()); // retire (swap-pop)
            g.voices.pop_back();
        } else {
            ++v;
        }
    }
    return any;
}

void deviceGetCallback(void* userdata, SDL_AudioStream* stream, int additionalAmount,
                        int totalAmount) {
    (void)userdata;
    (void)totalAmount;
    if (g.stream != stream) {
        return;
    }
    // `additionalAmount` is in bytes of the stream's *input* format
    // (S16 stereo → 4 bytes/frame).
    const int frameBytes = 2 * 2; // s16 × 2 channels
    const int wantSamples = additionalAmount > 0 ? (additionalAmount / frameBytes) * 2 : 0;
    g.lastRequestFrames.store(wantSamples / 2, std::memory_order_relaxed);

    static thread_local int16_t scratch[8192];
    int got = 0;
    if (!g.isPaused.load(std::memory_order_relaxed)) {
        got = static_cast<int>(g.ring->read(scratch, static_cast<size_t>(wantSamples)));
    }
    // One-shot voices (SEs) mix on top of the ring output. When the ring ran
    // dry but a voice is still playing, the missing frames are silence — fill
    // them so the voice keeps flowing (framesConsumed only counts ring data).
    if (!g.isPaused.load(std::memory_order_relaxed) && wantSamples > 0) {
        const int have = got;
        if (have < wantSamples) {
            std::memset(scratch + have, 0, static_cast<size_t>(wantSamples - have) * sizeof(int16_t));
        }
        if (mixOneShots(scratch, static_cast<size_t>(wantSamples) / 2)) {
            got = wantSamples;
        } else if (have < wantSamples) {
            got = have; // nothing mixed; keep the short read as before
        }
    }
    // Master gain (applied on the device thread; cheap enough).
    const float gain = g.gain.load(std::memory_order_relaxed);
    if (gain != 1.0f) {
        for (int i = 0; i < got; ++i) {
            float f = scratch[i] * gain;
            if (f > 32767.0f) f = 32767.0f;
            if (f < -32768.0f) f = -32768.0f;
            scratch[i] = static_cast<int16_t>(f);
        }
    }
    g.framesConsumed.fetch_add(static_cast<uint64_t>(got / 2), std::memory_order_relaxed);

    if (got > 0) {
        SDL_PutAudioStreamData(stream, scratch, got * 2); // bytes
    }

    if (g.tick != nullptr) {
        g.tick(g.tickUser, wantSamples / 2);
    }
}

} // namespace

bool init(const Config& config) {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.initialized) {
        return true;
    }
    g.ring = new RingBuffer(static_cast<size_t>(config.inputFreq) * config.latencyMs / 1000 * 2);
    g.inputFreq = config.inputFreq;
    g.initialized = true;

    if (!config.enable) {
        g.virtualMode = true;
        g.status = "virtual (audio disabled by configuration)";
        return true;
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        g.virtualMode = true;
        g.status = std::string("virtual (SDL audio init failed: ") + SDL_GetError() + ")";
        PL_LOG_WARN("Audio", "SDL audio unavailable: %s — running in virtual mode", g.status.c_str());
        return true;
    }

    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = config.inputFreq;

    // Device selection: the default playback device for now (config.deviceName
    // is honored by setting SDL_AUDIODRIVER/SDL_AUDIO_DEVICE before init; a
    // by-name picker can be added later, TODO(PC_PORT)).
    g.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                         deviceGetCallback, nullptr);
    if (g.stream == nullptr) {
        g.virtualMode = true;
        g.status = std::string("virtual (SDL_OpenAudioDeviceStream failed: ") + SDL_GetError() + ")";
        PL_LOG_WARN("Audio", "audio device open failed: %s — running in virtual mode", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return true;
    }

    // Actual device format (for stats/logging).
    SDL_AudioSpec srcSpec{}, dstSpec{};
    if (SDL_GetAudioStreamFormat(g.stream, &srcSpec, &dstSpec)) {
        g.deviceFreqOut = dstSpec.freq;
        g.deviceChannelsOut = dstSpec.channels;
        SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(g.stream);
        const char* name = SDL_GetAudioDeviceName(dev);
        g.deviceName = name ? name : "?";
    }
    // PC_PORT (M9.5.4 v7): SDL3 opens device streams PAUSED
    // (SDL_OpenAudioDeviceStream docs: "the device starts paused; call
    // SDL_ResumeAudioStreamDevice to start playback"). Nothing ever resumed
    // it, so the get-callback never ran and every push() drained into a ring
    // nobody read — the audio path was "working" by every counter while the
    // speakers stayed silent.
    if (!SDL_ResumeAudioStreamDevice(g.stream)) {
        PL_LOG_WARN("Audio", "SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
    }

    g.enabled = true;
    g.virtualMode = false;
    g.status = "enabled";
    PL_LOG_INFO("Audio", "audio device '%s' opened (game %d Hz → device %d Hz, %d ch, ring %d ms)",
                g.deviceName.c_str(), config.inputFreq, g.deviceFreqOut, g.deviceChannelsOut, config.latencyMs);
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.initialized) {
        return;
    }
    if (g.stream != nullptr) {
        SDL_DestroyAudioStream(g.stream);
        g.stream = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    delete g.ring;
    g.ring = nullptr;
    {
        std::lock_guard<std::mutex> vlock(g.voicesMutex);
        g.voices.clear();
    }
    g.initialized = false;
    g.enabled = false;
    g.virtualMode = false;
}

bool isInitialized() { return g.initialized; }
bool isEnabled() { return g.enabled; }
bool isVirtual() { return g.initialized && g.virtualMode; }
const char* statusString() { return g.status.c_str(); }
int deviceFreq() { return g.deviceFreqOut; }
int deviceChannels() { return g.deviceChannelsOut; }
const char* deviceName() { return g.deviceName.c_str(); }
int inputFreq() { return g.initialized ? g.inputFreq : 0; }

void push(const int16_t* interleaved, int frames) {
    if (!g.initialized || interleaved == nullptr || frames <= 0) {
        return;
    }
    if (g.isPaused.load(std::memory_order_relaxed)) {
        return;
    }
    const size_t n = g.ring->write(interleaved, static_cast<size_t>(frames) * 2);
    const size_t want = static_cast<size_t>(frames) * 2;
    g.framesPushed.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
    if (n < want) {
        g.framesDropped.fetch_add(static_cast<uint64_t>((want - n) / 2), std::memory_order_relaxed);
    }
}

uint64_t framesPushed() { return g.framesPushed.load(std::memory_order_relaxed); }
uint64_t framesDropped() { return g.framesDropped.load(std::memory_order_relaxed); }
uint64_t framesConsumed() { return g.framesConsumed.load(std::memory_order_relaxed); }
int queuedFrames() { return g.initialized ? static_cast<int>(g.ring->available() / 2) : 0; }
int capacityFrames() { return g.initialized ? static_cast<int>(g.ring->capacity() / 2) : 0; }

void pause() { g.isPaused.store(true, std::memory_order_relaxed); }
void resume() { g.isPaused.store(false, std::memory_order_relaxed); }
bool paused() { return g.isPaused.load(std::memory_order_relaxed); }

void setMasterGain(float gain) {
    g.gain.store(gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain), std::memory_order_relaxed);
}
float masterGain() { return g.gain.load(std::memory_order_relaxed); }

int pull(int16_t* dst, int maxSamples) {
    if (!g.initialized || dst == nullptr || maxSamples <= 0) {
        return 0;
    }
    int got = static_cast<int>(g.ring->read(dst, static_cast<size_t>(maxSamples)));
    // One-shots mix here too (virtual mode / tests use pull as the sink).
    if (!g.isPaused.load(std::memory_order_relaxed)) {
        const int have = got;
        if (have < maxSamples) {
            std::memset(dst + have, 0, static_cast<size_t>(maxSamples - have) * sizeof(int16_t));
        }
        if (mixOneShots(dst, static_cast<size_t>(maxSamples) / 2)) {
            got = maxSamples;
        } else if (have < maxSamples) {
            got = have;
        }
    }
    return got;
}

void setDeviceTick(DeviceTick tick, void* user) {
    g.tick = tick;
    g.tickUser = user;
}

bool playOneShot(const int16_t* interleaved, int frames, float gain) {
    if (!g.initialized || interleaved == nullptr || frames <= 0) {
        return false;
    }
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    std::lock_guard<std::mutex> lock(g.voicesMutex);
    constexpr size_t kMaxVoices = 16;
    if (g.voices.size() >= kMaxVoices) {
        return false; // pool full — drop (SEs are fire-and-forget)
    }
    State::OneShotVoice voice;
    voice.pcm.assign(interleaved, interleaved + static_cast<size_t>(frames) * 2);
    voice.gain = gain;
    g.voices.push_back(std::move(voice));
    return true;
}

int activeVoices() {
    std::lock_guard<std::mutex> lock(g.voicesMutex);
    return static_cast<int>(g.voices.size());
}

} // namespace Platform::Audio
