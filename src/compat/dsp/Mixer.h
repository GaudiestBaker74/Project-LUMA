#pragma once
// =============================================================================
// compat::dsp::Mixer — the JAS DSP "microcode" reimplemented in portable C++.
//
// On the Wii, SMG's audio runs through the custom JAS DSP microcode (the
// `jdsp` image in JSystem/JAudio2/dsptask.cpp): the CPU (JASDriver) writes
// per-frame command mail to the DSP and 64 voice descriptors (JASDsp::CH_BUF)
// plus 4 FX line descriptors (JASDsp::FX_BUF); the DSP decompresses, pitches,
// filters, ramps and mixes every voice into the output buffers.
//
// This class executes the same descriptors with the same per-frame cadence
// (subframes of 0x50 samples) on the host CPU. See docs/audio.md §"Fidelity"
// for the exact level of behavioral fidelity per stage.
//
// Pure C++ (no SDL, no OS calls) — unit-tested in dsp_mixer_test.cpp.
// =============================================================================

#include "JSystem/JAudio2/JASDspInterface.hpp"

#include <cstdint>

namespace compat::dsp {

// Voice interpolation modes (the DSP always resamples; see docs/audio.md).
enum class Interp {
    Linear, // linear interpolation (exact at pitch 1.0)
    Sinc4,  // 4-tap windowed-sinc, phase = frac (kernel computed at runtime;
            // TODO(PC_PORT): use the DSPRES_FILTER ROM table once its layout
            // is confirmed against the jdsp microcode)
};

struct MixerConfig {
    Interp interp = Interp::Linear;
    int subFrameSamples = 0x50; // DSP subframe size (80)
    // Resolves a 32-bit wave-data token (the game's 32-bit ABI word in
    // TChannel._118) to a host pointer. Required for wave channels; when null
    // wave channels are silent (see docs/audio.md §"32-bit pointer ABI").
    const uint8_t* (*resolveWave)(uint32_t token) = nullptr;
};

class Mixer {
public:
    explicit Mixer(const MixerConfig& config = {}) : mConfig(config) {}
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // Resets all persistent voice state (as if the DSP were rebooted).
    void reset();

    // Mixes one DSP subframe (mConfig.subFrameSamples samples) from the 64
    // voice descriptors into the non-interleaved stereo outputs.
    // `channels`/`fx` point at JASDsp::CH_BUF / JASDsp::FX_BUF (host memory).
    // outL/outR: subFrameSamples s16 each. mixerLevel: the DSP mixer level
    // (1.15, as packed by DsetMixerLevel; 0x4000 = 0.5).
    void mixSubframe(JASDsp::TChannel* channels, JASDsp::FxBuf* fx, uint32_t mixerLevel,
                     int16_t* outL, int16_t* outR);

    // Number of voices currently producing audio (for stats/tests).
    int activeVoices() const;

    // Test access: re-arm the runtime for voice `index` (as if the DSP saw a
    // playStart followed by one subframe).
    void resetVoiceState(int index);

private:
    struct VoiceRuntime;
    void processVoice(int index, JASDsp::TChannel* ch, int32_t* busAcc[/*11*/ 11], int samples);
    int16_t sampleAt(int idx, JASDsp::TChannel* ch, VoiceRuntime& rt, bool* ended);

    MixerConfig mConfig;
    VoiceRuntime* mRuntimes = nullptr;
    int mRuntimeCount = 0;
    bool mHaveRuntimes = false;
};

} // namespace compat::dsp
