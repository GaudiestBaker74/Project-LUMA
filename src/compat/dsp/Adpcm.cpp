// compat::dsp::adpcm — see Adpcm.h.
#include "compat/dsp/Adpcm.h"

#include <cstring>

namespace compat::dsp {

// The DSP ROM image of the ADPCM coefficient table, verbatim from
// JASDsp::DSPADPCM_FILTER (64 bytes = 32 big-endian u16 = 16 pairs).
static const uint8_t kAdpcmRom[64] = {
    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x04, 0x00, 0x04, 0x00, 0x10, 0x00, 0xF8, 0x00,
    0x0E, 0x00, 0xFA, 0x00, 0x0C, 0x00, 0xFC, 0x00, 0x12, 0x00, 0xF6, 0x00, 0x10, 0x68, 0xF7, 0x38,
    0x12, 0xC0, 0xF7, 0x04, 0x14, 0x00, 0xF4, 0x00, 0x08, 0x00, 0xF8, 0x00, 0x04, 0x00, 0xFC, 0x00,
    0xFC, 0x00, 0x04, 0x00, 0xFC, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const int16_t (*adpcmCoefTable())[2] {
    static int16_t table[kAdpcmCoefPairs][2];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < kAdpcmCoefPairs; ++i) {
            table[i][0] = static_cast<int16_t>((kAdpcmRom[i * 4] << 8) | kAdpcmRom[i * 4 + 1]);
            table[i][1] = static_cast<int16_t>((kAdpcmRom[i * 4 + 2] << 8) | kAdpcmRom[i * 4 + 3]);
        }
        built = true;
    }
    return table;
}

int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

void adpcmDecodeBlock(const uint8_t* block, int16_t out[kAdpcmBlockSamples], AdpcmState& state) {
    const uint8_t header = block[0];
    const int16_t (*coefs)[2] = adpcmCoefTable();
    const int16_t c1 = coefs[header >> 4][0];
    const int16_t c2 = coefs[header >> 4][1];
    const int32_t scale = 1 << (header & 0x0F);

    // 4-bit samples, high nibble first (DSP ordering).
    static const int8_t nibbleToS8[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};

    for (int i = 0; i < kAdpcmBlockSamples; ++i) {
        const uint8_t byte = block[1 + (i >> 1)];
        const uint8_t nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
        const int32_t n = nibbleToS8[nib];

        // Standard DSP ADPCM:
        //   sample = clamp((((nibble * scale) << 11) + 2048? ...) )
        // Reference (vgmstream/RetroModding DSP format, itself taken from the
        // DSP ucode behavior):
        //   sample = clamp((((n * scale) << 11) + 1024 + (c1*h1 + c2*h2)) >> 11)
        const int32_t sample =
            ((((n * scale) << 11) + 1024 + (c1 * state.h1 + c2 * state.h2)) >> 11);

        state.h2 = state.h1;
        state.h1 = clamp16(sample); // the ucode clamps to s16
        out[i] = clamp16(sample);
    }
}

void pcm8DecodeBlock(const uint8_t* block, int16_t out[kPcm8BlockSamples]) {
    for (int i = 0; i < kPcm8BlockSamples; ++i) {
        out[i] = static_cast<int16_t>((static_cast<int32_t>(block[i]) - 128) << 8);
    }
}

void pcm16DecodeBlock(const uint8_t* block, int16_t out[kPcm16BlockSamples]) {
    for (int i = 0; i < kPcm16BlockSamples; ++i) {
        out[i] = static_cast<int16_t>((block[i * 2] << 8) | block[i * 2 + 1]);
    }
}

} // namespace compat::dsp
