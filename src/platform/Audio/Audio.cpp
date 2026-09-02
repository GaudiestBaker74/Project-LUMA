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
};

State g;

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
    g.enabled = true;
    g.virtualMode = false;
    g.status = "enabled";
    PL_LOG_INFO("Audio", "audio device '%s' opened (game %d Hz → device %d Hz, %d ch)",
                g.deviceName.c_str(), config.inputFreq, g.deviceFreqOut, g.deviceChannelsOut);
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
    return static_cast<int>(g.ring->read(dst, static_cast<size_t>(maxSamples)));
}

void setDeviceTick(DeviceTick tick, void* user) {
    g.tick = tick;
    g.tickUser = user;
}

} // namespace Platform::Audio
