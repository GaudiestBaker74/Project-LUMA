#pragma once
// =============================================================================
// Platform::Audio — the PC audio output (SDL3 backend, M8).
//
// The game-side audio layer (compat/ai + the JAS driver) produces interleaved
// s16 frames at the game's output rate (~32 kHz) and hands them to
// Platform::Audio::push(); a device-thread callback drains them into the SDL
// audio stream, which resamples to the physical device (48 kHz etc.).
//
// When no device is available (headless CI, `SDL_AUDIODRIVER=dummy` failure,
// --no-audio) the module stays usable in "virtual" mode: push() keeps its
// counters and pull() drains the ring, so the whole audio pipeline can run
// and be tested without a sound card.
// =============================================================================

#include <cstdint>

namespace Platform::Audio {

struct Config {
    bool enable = true;       // false → virtual mode from the start
    int inputFreq = 32000;    // rate of the frames pushed by the game layer
    int deviceChannels = 2;   // 2 = stereo (only stereo supported)
    int latencyMs = 40;       // ring capacity
    const char* deviceName = nullptr; // nullptr = SDL default playback device
};

// Initializes the audio subsystem. Returns false only on a hard failure
// (SDL init error); device-open failures fall back to virtual mode.
bool init(const Config& config = {});
void shutdown();

bool isInitialized();
bool isEnabled(); // real output device active
bool isVirtual(); // initialized without a live device
const char* statusString();
int deviceFreq();     // actual device output rate (0 when virtual)
int deviceChannels();
const char* deviceName();

// Producer side (called by the audio/ai thread; non-blocking):
// appends interleaved s16 frames (inputFreq Hz, stereo). Frames beyond the
// ring capacity are dropped and counted.
void push(const int16_t* interleaved, int frames);

uint64_t framesPushed();
uint64_t framesDropped();
uint64_t framesConsumed();
int queuedFrames(); // s16 samples in the ring

void pause();
void resume();
bool paused();
void setMasterGain(float gain); // 0..1, applied on the device thread
float masterGain();

// Headless/test sink: drains ring samples (as the SDL callback would).
int pull(int16_t* dst, int maxSamples);

// Device-thread tick (called by the SDL callback with the amount requested,
// in output frames). Used by compat/ai for latency accounting. Optional.
using DeviceTick = void (*)(void* user, int framesRequested);
void setDeviceTick(DeviceTick tick, void* user);

} // namespace Platform::Audio
