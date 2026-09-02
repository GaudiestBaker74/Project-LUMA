#pragma once
// =============================================================================
// compat::dsp::adpcm — Nintendo DSP ADPCM decoder (JAS wave "format 0").
//
// The JAS DSP microcode decompresses each 16-sample block from a 1-byte
// header + 8 bytes of 4-bit samples. The algorithm is the standard DSP
// ADPCM (identical to the DSP .adpcm file format used across first-party
// titles and to the AX ucode): see docs/audio.md §"Fidelity".
//
// Pure C++, no console/PC dependencies — unit-tested (dsp_mixer_test.cpp).
// =============================================================================

#include <cstdint>

namespace compat::dsp {

inline constexpr int kAdpcmBlockSamples = 16; // samples per ADPCM block
inline constexpr int kAdpcmBlockBytes = 9;    // 1 header + 8 data bytes
inline constexpr int kAdpcmCoefPairs = 16;    // pairs available in the DSP table

// Decoder history (2 previous samples), kept across blocks of a wave.
struct AdpcmState {
    int16_t h1 = 0; // most recent output
    int16_t h2 = 0; // second most recent
};

// The 16 coefficient pairs (s15 fixed point), in the exact order of
// JASDsp::DSPADPCM_FILTER (the ROM byte image, big-endian u16 words).
// Index 0..15; the DSP header's top nibble selects the pair.
const int16_t (*adpcmCoefTable())[2];

// Decodes one block (kAdpcmBlockBytes bytes) into 16 samples, advancing
// `state`. `scale`/`coef` come from the block header byte:
//   bits 7-4 = coefficient pair index, bits 3-0 = scale exponent.
void adpcmDecodeBlock(const uint8_t* block, int16_t out[kAdpcmBlockSamples], AdpcmState& state);

// --- PCM formats used by JAS waves -------------------------------------------
inline constexpr int kPcm8BlockSamples = 8;  // 8-bit PCM, 1 byte/sample
inline constexpr int kPcm16BlockSamples = 16; // 16-bit PCM, 2 bytes/sample (big-endian)

// 8-bit unsigned PCM (0x00..0xFF, mid = 0x80) → s16.
void pcm8DecodeBlock(const uint8_t* block, int16_t out[kPcm8BlockSamples]);

// 16-bit big-endian PCM.
void pcm16DecodeBlock(const uint8_t* block, int16_t out[kPcm16BlockSamples]);

// Signed 16-bit clamp.
int16_t clamp16(int32_t v);

} // namespace compat::dsp
