// =============================================================================
// M8: compat::dsp::Mixer unit tests (pure C++, no SDL/threads — headless).
//
// The mixer is the CPU reimplementation of the JAS DSP microcode that JASDriver
// drives through the 64 voice descriptors (JASDsp::CH_BUF semantics). These
// tests construct descriptors directly (the same layout the vendored
// JASDSPChannel.cpp writes) and verify: PCM playback + pan/connect routing,
// voice end-of-wave shutdown, ADPCM decode, two-voice accumulation, and the
// silence fallback when no wave resolver is installed.
//
// The wave "tokens" are test-chosen ids resolved by a local resolver, so no
// JAS heap / JASDram setup is needed.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/dsp/Mixer.h"

#include <cstdint>
#include <cstring>

namespace {

// 16 samples, value = i * 64 (fits the 1.15 volume math without clipping).
// PCM16 big-endian, as the DSP sees it.
const uint8_t kPcm16Wave[32] = {
    0x00, 0x00, 0x00, 0x40, 0x00, 0x80, 0x00, 0xC0, 0x01, 0x00, 0x01, 0x40, 0x01, 0x80, 0x01, 0xC0,
    0x02, 0x00, 0x02, 0x40, 0x02, 0x80, 0x02, 0xC0, 0x03, 0x00, 0x03, 0x40, 0x03, 0x80, 0x03, 0xC0,
};

// 16 samples of DSP ADPCM: header 0x0F (coef pair 15 = {0,0}, scale 2^15) and
// nibbles alternating +1/-1 → decoded 32767 / -32768 (2 blocks of 9 bytes).
const uint8_t kAdpcmWave[18] = {
    0x0F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
    0x0F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,
};

const uint32_t kTestToken = 0xABCD1234u;

// Connect words (DSP RAM addresses, JASDsp::SEND_TABLE).
const uint16_t kMainL = 0x0D00;
const uint16_t kMainR = 0x0D60;

constexpr int kSubSamples = 0x50; // 80 (Mixer default)

using compat::dsp::Mixer;
using compat::dsp::MixerConfig;

// Fills a voice descriptor the way JASDSPChannel does for a simple stereo
// voice (connect to main L and R, no envelopes, no filters, unity-ish volume).
void setupVoice(JASDsp::TChannel& ch, const uint8_t* wave, uint32_t token,
                uint16_t pitch, uint16_t vol) {
    std::memset(&ch, 0, sizeof(ch));
    ch.mIsActive = 1;
    ch.mPitch = pitch; // 4.12 (0x1000 = 1.0)
    ch._118 = token;   // 32-bit ABI word → resolved by the config resolver
    ch._110 = 0;       // loop start
    ch._114 = 16;      // loop end / one-shot end
    ch._102 = 0;       // one-shot
    ch._104 = 0;       // ADPCM loop h1
    ch._106 = 0;       // ADPCM loop h2
    // Volume slots: [0]=connect, [1]=target, [2]=current, [3]=rate.
    // Flat access: _10 is declared u16[1][4] ("size unknown" in the decomp),
    // the four slots live at offset 0x10 (the mixer reads them the same way).
    uint16_t* slots = reinterpret_cast<uint16_t*>(&ch._10);
    slots[0] = kMainL; slots[1] = vol; slots[2] = vol; slots[3] = 0;
    slots[4] = kMainR; slots[5] = vol; slots[6] = vol; slots[7] = 0;
}

const uint8_t* resolveTestWave(uint32_t token) {
    if (token != kTestToken) {
        return nullptr;
    }
    return kPcm16Wave; // the resolver only cares that the token is registered
}

Mixer makeMixer(bool withResolver = true) {
    MixerConfig cfg;
    if (withResolver) {
        cfg.resolveWave = resolveTestWave;
    }
    return Mixer(cfg);
}

} // namespace

TEST_CASE(dsp_mixer_pcm_plays_and_routes) {
    Mixer mixer = makeMixer();
    mixer.reset();

    JASDsp::TChannel channels[64] = {};
    JASDsp::FxBuf fx[4] = {};
    int16_t outL[kSubSamples] = {};
    int16_t outR[kSubSamples] = {};

    // PCM16, mono wave, pitch 1.0, stereo connect (L+R).
    setupVoice(channels[0], kPcm16Wave, kTestToken, 0x1000, 0x7FFF);
    channels[0]._64 = 1;  // PCM (unblocked)
    channels[0]._100 = 16; // PCM16 BE

    mixer.mixSubframe(channels, fx, 0x4000, outL, outR);

    // The wave starts at 0 and rises by 64 per sample; with ~unity volumes and
    // the 1.15 chain the output must rise too, symmetric in L/R.
    // NOTE: the 16-sample one-shot ends at output sample 16, well inside this
    // 80-sample subframe — by the end of the mix no voice remains active.
    CHECK(mixer.activeVoices() == 0);
    CHECK(outL[0] == outR[0]);
    CHECK(outL[1] > outL[0]); // sample 1 → 64 → small but positive
    CHECK(outL[2] > outL[1]);
    CHECK(outL[3] > outL[2]);
    CHECK(outL[4] > outL[3]);
    // 16-sample one-shot: the voice must have ended during this subframe.
    CHECK(channels[0].mIsFinished == 1);
    CHECK(channels[0].mIsActive == 0);

    // Mixing again: silence (no active voice).
    int16_t outL2[kSubSamples] = {};
    int16_t outR2[kSubSamples] = {};
    mixer.mixSubframe(channels, fx, 0x4000, outL2, outR2);
    for (int i = 0; i < kSubSamples; ++i) {
        CHECK(outL2[i] == 0);
        CHECK(outR2[i] == 0);
    }
}

TEST_CASE(dsp_mixer_pan_to_left_only) {
    Mixer mixer = makeMixer();
    mixer.reset();

    JASDsp::TChannel channels[64] = {};
    JASDsp::FxBuf fx[4] = {};
    int16_t outL[kSubSamples] = {};
    int16_t outR[kSubSamples] = {};

    // Same voice but only the left connect is set (hard pan L).
    setupVoice(channels[0], kPcm16Wave, kTestToken, 0x1000, 0x7FFF);
    channels[0]._64 = 1;
    channels[0]._100 = 16;
    uint16_t* slots = reinterpret_cast<uint16_t*>(&channels[0]._10);
    slots[4] = 0x0000; // right slot disconnected

    mixer.mixSubframe(channels, fx, 0x4000, outL, outR);

    CHECK(outL[1] > 0);
    for (int i = 0; i < kSubSamples; ++i) {
        CHECK(outR[i] == 0); // nothing routed to the right bus
    }
}

TEST_CASE(dsp_mixer_adpcm_decodes) {
    Mixer mixer = makeMixer();
    mixer.reset();

    JASDsp::TChannel channels[64] = {};
    JASDsp::FxBuf fx[4] = {};
    int16_t outL[kSubSamples] = {};
    int16_t outR[kSubSamples] = {};

    // ADPCM voice: resolver must now hand out the ADPCM wave buffer, so use a
    // dedicated resolver.
    MixerConfig cfg;
    cfg.resolveWave = [](uint32_t token) -> const uint8_t* {
        return token == kTestToken ? kAdpcmWave : nullptr;
    };
    Mixer adpcmMixer(cfg);
    adpcmMixer.reset();

    setupVoice(channels[0], kAdpcmWave, kTestToken, 0x1000, 0x7FFF);
    channels[0]._100 = 9; // 9-byte DSP ADPCM blocks (JAS block size)
    channels[0]._104 = 0;
    channels[0]._106 = 0;

    adpcmMixer.mixSubframe(channels, fx, 0x4000, outL, outR);

    // Decoded samples alternate 32767 / -32768 → post-volume ±16382-ish.
    CHECK(outL[0] > 16000);
    CHECK(outL[1] < -16000);
    CHECK(outL[2] > 16000);
    CHECK(outL[3] < -16000);
    CHECK(channels[0].mIsFinished == 1); // 16 samples, one-shot
}

TEST_CASE(dsp_mixer_two_voices_accumulate) {
    Mixer mixer = makeMixer();
    mixer.reset();

    JASDsp::TChannel channels[64] = {};
    JASDsp::FxBuf fx[4] = {};

    // One voice only → reference amplitudes.
    setupVoice(channels[0], kPcm16Wave, kTestToken, 0x1000, 0x7FFF);
    channels[0]._64 = 1;
    channels[0]._100 = 16;
    int16_t refL[kSubSamples] = {};
    int16_t refR[kSubSamples] = {};
    mixer.mixSubframe(channels, fx, 0x4000, refL, refR);

    // Two identical voices → summed on the bus (~2x, ±1 rounding).
    mixer.resetVoiceState(0);
    channels[0].mIsActive = 1; // re-arm (finished by the previous mix)
    channels[0].mIsFinished = 0;
    setupVoice(channels[1], kPcm16Wave, kTestToken, 0x1000, 0x7FFF);
    channels[1]._64 = 1;
    channels[1]._100 = 16;
    channels[1]._114 = 0x1000; // long one-shot: no end inside this subframe
    channels[0]._114 = 0x1000;
    int16_t outL[kSubSamples] = {};
    int16_t outR[kSubSamples] = {};
    mixer.mixSubframe(channels, fx, 0x4000, outL, outR);

    CHECK(mixer.activeVoices() == 2);
    for (int i = 1; i < 10; ++i) {
        const int d = static_cast<int>(outL[i]) - 2 * static_cast<int>(refL[i]);
        CHECK(d >= -1 && d <= 1);
    }
}

TEST_CASE(dsp_mixer_no_resolver_is_silent) {
    Mixer mixer = makeMixer(false); // no wave resolver
    mixer.reset();

    JASDsp::TChannel channels[64] = {};
    JASDsp::FxBuf fx[4] = {};
    int16_t outL[kSubSamples] = {};
    int16_t outR[kSubSamples] = {};

    setupVoice(channels[0], kPcm16Wave, kTestToken, 0x1000, 0x7FFF);
    channels[0]._64 = 1;
    channels[0]._100 = 16;
    channels[0]._114 = 0x1000; // long one-shot: the voice keeps playing
    mixer.mixSubframe(channels, fx, 0x4000, outL, outR);
    for (int i = 0; i < kSubSamples; ++i) {
        CHECK(outL[i] == 0);
        CHECK(outR[i] == 0);
    }
    CHECK(mixer.activeVoices() == 1); // still playing (silent) — documented
}
