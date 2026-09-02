// compat::dsp::Mixer — see Mixer.h.
//
// The DSP-side voice processing implemented here follows the JAS descriptor
// semantics (JASDsp::TChannel) and the standard Nintendo DSP algorithms
// (ADPCM, 4.12 pitch, 1.15 volumes). Behaviors that depend on the exact
// encoding of the jdsp microcode (interpolation kernel choice, envelope
// curve, FX network) are implemented as documented approximations — see
// docs/audio.md §"Fidelity".
#include "compat/dsp/Mixer.h"

#include "compat/dsp/Adpcm.h"

#include <cmath>
#include <cstring>

namespace compat::dsp {

namespace {

// Mixer-internal bus map (see docs/audio.md §"Bus map"). The keys are the
// DSP RAM addresses used by the channel connect table (JASDsp::SEND_TABLE /
// TChannel::setBusConnect) — the mixer routes by address, exactly like the
// microcode.
enum Bus : int {
    kBusMainL = 0, // 0x0D00
    kBusMainR = 1, // 0x0D60
    kBusFx0In = 2, // 0x0DC0
    kBusFx1In = 3, // 0x0E20
    kBusFx2In = 4, // 0x0E80
    kBusFx3In = 5, // 0x0EE0
    kBusAux0 = 6,  // 0x0CA0
    kBusAux1 = 7,  // 0x0F40
    kBusAux2 = 8,  // 0x0FA0
    kBusAux3 = 9,  // 0x0B00
    kBusAux4 = 10, // 0x09A0
    kBusCount = 11,
};

inline constexpr int kMaxVoiceCount = 64;

// TChannel._10 is declared `u16[1][4]` ("array size unknown" in the decomp) but
// the DSP layout holds 4 volume slots per connect word: [0]=bus connect,
// [1]=target volume, [2]=current volume, [3]=rate<<8|const. Flat access avoids
// pointer arithmetic past the declared 1-row array (UB).
inline const uint16_t* slot4(const JASDsp::TChannel* ch, int b) {
    return reinterpret_cast<const uint16_t*>(ch->_10) + b * 4;
}

// Maps a channel connect word (a DSP address, or 0 = off) to a bus index.
// Unknown addresses fall back to main L (documented).
int busForConnect(uint16_t connect) {
    switch (connect) {
    case 0x0000: return -1; // disconnected
    case 0x0D00: return kBusMainL;
    case 0x0D60: return kBusMainR;
    case 0x0DC0: return kBusFx0In;
    case 0x0E20: return kBusFx1In;
    case 0x0E80: return kBusFx2In;
    case 0x0EE0: return kBusFx3In;
    case 0x0CA0: return kBusAux0;
    case 0x0F40: return kBusAux1;
    case 0x0FA0: return kBusAux2;
    case 0x0B00: return kBusAux3;
    case 0x09A0: return kBusAux4;
    default: return kBusMainL; // documented fallback
    }
}

} // namespace

struct Mixer::VoiceRuntime {
    bool wasActive = false;

    // stream state
    uint32_t posFixed = 0; // sample position, 4.12 fixed
    int blockIdx = -1;     // cached ADPCM block index
    int16_t block[kAdpcmBlockSamples];
    AdpcmState adpcm;
    bool fini = false;

    // filter history (post-interpolation samples)
    int16_t xHist[8] = {};
    int histPos = 0;
    int16_t yHist[4] = {}; // IIR feedback

    // oscillator
    uint32_t oscPhase = 0;

    // per-bus current volumes (ramped toward the descriptor's target)
    int16_t volCur[4] = {0, 0, 0, 0};
    int16_t autoVol = 0; // auto-mixer master gain (1.15)
};

Mixer::~Mixer() {
    delete[] mRuntimes;
    mRuntimes = nullptr;
}

void Mixer::reset() {
    delete[] mRuntimes;
    mRuntimes = new VoiceRuntime[kMaxVoiceCount];
    mRuntimeCount = kMaxVoiceCount;
    mHaveRuntimes = true;
    for (int i = 0; i < kMaxVoiceCount; ++i) {
        mRuntimes[i] = VoiceRuntime{};
    }
}

void Mixer::resetVoiceState(int index) {
    if (mHaveRuntimes && index >= 0 && index < kMaxVoiceCount) {
        mRuntimes[index] = VoiceRuntime{};
    }
}

int Mixer::activeVoices() const {
    int n = 0;
    for (int i = 0; i < (mHaveRuntimes ? mRuntimeCount : 0); ++i) {
        if (mRuntimes[i].wasActive) ++n;
    }
    return n;
}

// Fetches the wave sample at absolute index `idx` (looping/one-shot aware).
// On end of a non-looping wave, sets *ended.
int16_t Mixer::sampleAt(int idx, JASDsp::TChannel* ch, VoiceRuntime& rt, bool* ended) {
    // Wave end / loop handling (descriptor fields set by setWaveInfo):
    if (ch->_102 == 0 || ch->_114 <= ch->_110) {
        if (static_cast<uint32_t>(idx) >= ch->_114) {
            *ended = true;
            return 0;
        }
    } else {
        while (static_cast<uint32_t>(idx) >= ch->_114) {
            idx = static_cast<int>(ch->_110) + (idx - static_cast<int>(ch->_114));
        }
    }

    if (mConfig.resolveWave == nullptr) {
        return 0; // no wave resolver (tests can set one; see MixerConfig)
    }
    const uint8_t* base = mConfig.resolveWave(ch->_118); // 32-bit ABI token
    if (base == nullptr) {
        return 0;
    }

    // Decoding keyed on the block-bytes/bit-depth word `_100` written by the
    // vendored setWaveInfo (see docs/audio.md §"Wave formats"):
    //   _64 == 1  -> PCM (unblocked): _100 = 8 (u8) or 16 (BE s16)
    //   _64 == 16 -> ADPCM family:    _100 = 9 (DSP ADPCM) / 5 (GADPCM, TODO)
    if (ch->_64 == 1) {
        if (ch->_100 == 16) {
            const uint8_t* p = base + idx * 2;
            return static_cast<int16_t>((p[0] << 8) | p[1]);
        }
        // 8-bit unsigned PCM (all other 1-sample-per-block formats).
        return static_cast<int16_t>((static_cast<int32_t>(base[idx]) - 128) << 8);
    }
    if (ch->_100 == 9) {
        const int blockIdx = idx >> 4;
        if (rt.blockIdx != blockIdx) {
            if (blockIdx != rt.blockIdx + 1) {
                // Jump/loop wrap: restore the loop-context history.
                rt.adpcm.h1 = ch->_104;
                rt.adpcm.h2 = ch->_106;
            }
            adpcmDecodeBlock(base + blockIdx * 9, rt.block, rt.adpcm);
            rt.blockIdx = blockIdx;
        }
        return rt.block[idx & 15];
    }
    // GADPCM (5-byte blocks) / unknown: silence until implemented (docs).
    return 0;
}

void Mixer::processVoice(int index, JASDsp::TChannel* ch, int32_t* busAcc[kBusCount], int samples) {
    VoiceRuntime& rt = mRuntimes[index];

    if (ch->mIsActive == 0) {
        if (rt.wasActive) {
            rt = VoiceRuntime{}; // voice stopped: re-arm cleanly next start
        }
        return;
    }
    if (!rt.wasActive) {
        // Voice went active: reset like the microcode's playStart handler.
        rt = VoiceRuntime{};
        rt.wasActive = true;
        for (int b = 0; b < 4; ++b) {
            const uint16_t* slot = slot4(ch, b);
            rt.volCur[b] = static_cast<int16_t>(slot[2]); // init volume (word2)
        }
        rt.autoVol = (ch->_58 != 0) ? ch->_54 : 0x7FFF; // auto-mixer off → unity
    }

    // Precompute per-bus connects (4 volume slots).
    int busIdx[4];
    for (int b = 0; b < 4; ++b) {
        busIdx[b] = busForConnect(slot4(ch, b)[0]);
    }

    const int rateAuto = (ch->_52 >> 8) & 0xFF; // auto-mixer envelope rate

    for (int s = 0; s < samples; ++s) {
        if (ch->mPauseFlag != 0 || rt.fini) {
            continue; // paused/finished: silence, do not advance
        }

        // --- 1. source: oscillator or wave -----------------------------------
        int16_t v = 0;
        if (ch->_118 == 0) {
            rt.oscPhase += ch->mPitch;
            const uint32_t ph = rt.oscPhase >> 4; // 0..0xFFFF
            switch (ch->_100 % 5) {
            case 0: v = (ph < 0x8000) ? 32767 : -32768; break; // square
            case 1: v = static_cast<int16_t>(std::sin(ph * 6.283185307179586 / 65536.0) * 32767.0); break;
            case 2: v = static_cast<int16_t>((((ph >> 10) & 1) ? (ph & 0x3FF) : 0x400 - (ph & 0x3FF)) << 4); break;
            case 3: v = static_cast<int16_t>(((ph >> 8) & 0xFF) - 128) * 256; break;
            default: v = static_cast<int16_t>((ph * 2654435761u) >> 16) & 0x7FFF; break;
            }
        } else {
            const uint32_t fixedPos = rt.posFixed;
            rt.posFixed += ch->mPitch; // 4.12
            const int i0 = static_cast<int>(fixedPos >> 12);
            const uint32_t frac = fixedPos & 0xFFF;
            bool ended = false;
            const int16_t s0 = sampleAt(i0, ch, rt, &ended);
            int16_t s1 = 0;
            if (!ended) {
                s1 = sampleAt(i0 + 1, ch, rt, &ended);
            }
            if (ended) {
                rt.fini = true;
                ch->mIsFinished = 1;
                ch->mIsActive = 0; // DSP stops the voice (CPU acks via replyFinish)
                rt.wasActive = false;
                continue;
            }
            // Linear interpolation (see docs/audio.md §"Interpolation").
            v = static_cast<int16_t>((s0 * static_cast<int32_t>(0x1000 - frac) + s1 * static_cast<int32_t>(frac)) >> 12);
        }

        // --- 2. channel filter (FIR8/IIR; descriptors default to passthrough)
        if (ch->mFilterMode != 0) {
            if (ch->mFilterMode & 0x20) {
                int32_t acc = 0;
                for (int t = 0; t < 8; ++t) {
                    acc += ch->fir_filter_params[t] * rt.xHist[(rt.histPos - t + 8) & 7];
                }
                rt.xHist[rt.histPos & 7] = v;
                rt.histPos++;
                v = clamp16(acc >> 15);
            } else {
                int32_t acc = 0;
                for (int t = 0; t < 4; ++t) {
                    acc += ch->iir_filter_params[t] * rt.xHist[(rt.histPos - t + 8) & 7];
                }
                for (int t = 0; t < 4; ++t) {
                    acc += ch->iir_filter_params[4 + t] * rt.yHist[(3 - t + 4) & 3];
                }
                rt.xHist[rt.histPos & 7] = v;
                rt.histPos++;
                const int16_t y = clamp16(acc >> 15);
                rt.yHist[(rt.histPos - 1) & 3] = y;
                v = y;
            }
        } else {
            rt.xHist[rt.histPos & 7] = v;
            rt.histPos++;
        }

        // --- 3. volume envelope (per bus) + auto mixer ------------------------
        if (ch->_58 != 0 && rateAuto != 0) {
            const int32_t step = (static_cast<int32_t>(ch->_56) - ch->_54) / rateAuto;
            ch->_54 = static_cast<uint16_t>(static_cast<int16_t>(ch->_54 + static_cast<int16_t>(step)));
            rt.autoVol = static_cast<int16_t>(ch->_54);
        } else {
            rt.autoVol = (ch->_58 != 0) ? static_cast<int16_t>(ch->_54) : 0x7FFF;
        }

        for (int b = 0; b < 4; ++b) {
            const uint16_t* slot = slot4(ch, b);
            const int16_t target = static_cast<int16_t>(slot[1]);
            const int rate = (slot[3] >> 8) & 0xFF;
            if (rate != 0) {
                const int32_t step = (static_cast<int32_t>(target) - rt.volCur[b]) / rate;
                rt.volCur[b] = static_cast<int16_t>(rt.volCur[b] + static_cast<int16_t>(step));
                const_cast<uint16_t*>(slot)[2] = static_cast<uint16_t>(rt.volCur[b]);
            } else {
                rt.volCur[b] = target;
                const_cast<uint16_t*>(slot)[2] = static_cast<uint16_t>(target);
            }

            const int bus = busIdx[b];
            if (bus < 0 || rt.volCur[b] == 0) {
                continue;
            }
            const int32_t vol = (static_cast<int32_t>(rt.volCur[b]) * rt.autoVol) >> 15;
            busAcc[bus][s] += (static_cast<int32_t>(v) * vol) >> 15;
        }
    }
}

void Mixer::mixSubframe(JASDsp::TChannel* channels, JASDsp::FxBuf* fx, uint32_t mixerLevel,
                        int16_t* outL, int16_t* outR) {
    if (!mHaveRuntimes) {
        reset();
    }
    const int samples = mConfig.subFrameSamples;

    int32_t busAccData[kBusCount][0x100];
    int32_t* busAcc[kBusCount];
    for (int b = 0; b < kBusCount; ++b) {
        std::memset(busAccData[b], 0, sizeof(int32_t) * samples);
        busAcc[b] = busAccData[b];
    }

    for (int v = 0; v < kMaxVoiceCount; ++v) {
        processVoice(v, &channels[v], busAcc, samples);
    }

    // FX sends are accumulated into kBusFx*In / kBusAux*; the wet processing
    // (delay/reverb network over FxBuf) is documented as TODO(PC_PORT) in
    // docs/audio.md — the sends are mixed into the main buses as dry signal
    // until the FX network lands, so routed channels stay audible.
    for (int f = 0; f < 4; ++f) {
        const int fxIn = kBusFx0In + f;
        for (int s = 0; s < samples; ++s) {
            busAcc[kBusMainL][s] += busAcc[fxIn][s];
            busAcc[kBusMainR][s] += busAcc[fxIn][s];
        }
        (void)fx;
    }
    // Aux sends (0x0CA0 family): structure mapped, treated as dry fills.
    for (int a = kBusAux0; a <= kBusAux4; ++a) {
        for (int s = 0; s < samples; ++s) {
            busAcc[kBusMainL][s] += busAcc[a][s] / 2;
            busAcc[kBusMainR][s] += busAcc[a][s] / 2;
        }
    }

    // Final mix: clamp + DSP mixer level (1.15, >>15 — see docs/audio.md).
    for (int s = 0; s < samples; ++s) {
        const int32_t l = (busAcc[kBusMainL][s] * static_cast<int32_t>(mixerLevel)) >> 15;
        const int32_t r = (busAcc[kBusMainR][s] * static_cast<int32_t>(mixerLevel)) >> 15;
        outL[s] = clamp16(l);
        outR[s] = clamp16(r);
    }
}

} // namespace compat::dsp
