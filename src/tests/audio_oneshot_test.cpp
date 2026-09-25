// =============================================================================
// M8.5 prep: Platform::Audio one-shot voices (sound-effect layer).
//
// The music stream is the ring's only producer; SEs must MIX on top of it
// instead of queueing behind it. These tests run headless in virtual mode
// (pull() is the sink) and cover: mixing over ring audio, saturation,
// self-retirement when the clip ends, the voice pool cap, and silence
// passthrough (pull keeps returning ring data untouched when no voice plays).
// =============================================================================

#include "tests/test_runner.h"

#include "compat/audio/SystemSe.h"
#include "compat/dsp/Adpcm.h"
#include "platform/Audio/Audio.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

Platform::Audio::Config virtualConfig() {
    Platform::Audio::Config cfg;
    cfg.enable = false; // virtual mode: no SDL device
    return cfg;
}

} // namespace

TEST_CASE(audio_oneshot_mixes_over_ring_audio) {
    REQUIRE(Platform::Audio::init(virtualConfig()));

    // Four stereo frames of music at 1000, then an SE clip at 2000 (2 frames).
    const int16_t music[8] = {1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
    const int16_t se[4] = {2000, 2000, 2000, 2000};
    Platform::Audio::push(music, 4);
    REQUIRE(Platform::Audio::playOneShot(se, 2, 1.0f));
    CHECK(Platform::Audio::activeVoices() == 1);

    int16_t out[8] = {};
    const int got = Platform::Audio::pull(out, 8); // 4 frames
    CHECK(got == 8);
    // First 2 frames: music + SE. Last 2 frames: music only.
    CHECK(out[0] == 3000);
    CHECK(out[1] == 3000);
    CHECK(out[2] == 3000);
    CHECK(out[3] == 3000);
    CHECK(out[4] == 1000);
    CHECK(out[5] == 1000);
    CHECK(out[6] == 1000);
    CHECK(out[7] == 1000);
    CHECK(Platform::Audio::activeVoices() == 0); // clip ended, voice retired

    Platform::Audio::shutdown();
}

TEST_CASE(audio_oneshot_saturates_and_gains) {
    REQUIRE(Platform::Audio::init(virtualConfig()));

    // Ring silence + a hot voice: 30000 + 30000 must clamp to 32767 (not wrap).
    const int16_t silence[4] = {0, 0, 0, 0};
    const int16_t hot[2] = {30000, 30000};
    Platform::Audio::push(silence, 2);
    REQUIRE(Platform::Audio::playOneShot(hot, 1, 1.0f));

    int16_t out[4] = {};
    CHECK(Platform::Audio::pull(out, 4) == 4);
    CHECK(out[0] == 30000); // 0 (ring) + 30000 (voice)
    CHECK(out[1] == 30000);

    // Gain halves the voice: ring 20000 + 30000*0.5 = 35000 -> clamp 32767.
    const int16_t loud[2] = {20000, 20000};
    Platform::Audio::push(loud, 1);
    REQUIRE(Platform::Audio::playOneShot(hot, 1, 0.5f));
    CHECK(Platform::Audio::pull(out, 2) == 2);
    CHECK(out[0] == 32767);

    Platform::Audio::shutdown();
}

TEST_CASE(audio_oneshot_flows_when_ring_is_empty) {
    REQUIRE(Platform::Audio::init(virtualConfig()));

    // No music at all: the voice alone must reach the sink (the SE has to be
    // audible on a silent screen — e.g. the title's A+B decide sound).
    const int16_t se[6] = {500, -500, 500, -500, 500, -500};
    REQUIRE(Platform::Audio::playOneShot(se, 3, 1.0f));

    int16_t out[6] = {};
    CHECK(Platform::Audio::pull(out, 6) == 6);
    for (int i = 0; i < 6; ++i) {
        CHECK(out[i] == se[i]);
    }

    // Voice exhausted and ring empty -> pull returns 0 again.
    CHECK(Platform::Audio::pull(out, 6) == 0);
    CHECK(Platform::Audio::activeVoices() == 0);

    Platform::Audio::shutdown();
}

TEST_CASE(audio_oneshot_pool_cap_drops_excess) {
    REQUIRE(Platform::Audio::init(virtualConfig()));

    const int16_t se[2] = {100, 100};
    for (int i = 0; i < 16; ++i) {
        CHECK(Platform::Audio::playOneShot(se, 1, 1.0f));
    }
    CHECK(Platform::Audio::activeVoices() == 16);
    CHECK(!Platform::Audio::playOneShot(se, 1, 1.0f)); // 17th is dropped

    Platform::Audio::shutdown();
}

namespace {

void putBe32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    if (b.size() < off + 4) {
        b.resize(off + 4, 0);
    }
    b[off] = static_cast<uint8_t>(v >> 24);
    b[off + 1] = static_cast<uint8_t>(v >> 16);
    b[off + 2] = static_cast<uint8_t>(v >> 8);
    b[off + 3] = static_cast<uint8_t>(v);
}

void putBe16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    if (b.size() < off + 2) {
        b.resize(off + 2, 0);
    }
    b[off] = static_cast<uint8_t>(v >> 8);
    b[off + 1] = static_cast<uint8_t>(v);
}

void putFloatBe(std::vector<uint8_t>& b, size_t off, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    putBe32(b, off, u);
}

// Minimal BAA whose SYSTEM entry 0 is SE_SY_GAME_START → bank 21 program 0 →
// wave 7 of B21kawa_0.aw. Offsets match the JAS WSYS / IBNK layouts the
// loader reads.
std::vector<uint8_t> makeGameStartBaa() {
    std::vector<uint8_t> b(0x400, 0);
    std::memcpy(b.data(), "AA_<", 4);
    std::memcpy(b.data() + 0x04, "bst ", 4);
    putBe32(b, 0x08, 0x80);
    putBe32(b, 0x0C, 0x80);
    std::memcpy(b.data() + 0x10, "bstn", 4);
    putBe32(b, 0x14, 0x80);
    putBe32(b, 0x18, 0x120);
    std::memcpy(b.data() + 0x1C, "bsc ", 4);
    putBe32(b, 0x20, 0x120);
    putBe32(b, 0x24, 0x180);
    std::memcpy(b.data() + 0x28, "ws  ", 4);
    putBe32(b, 0x2C, 21);
    putBe32(b, 0x30, 0x180);
    putBe32(b, 0x34, 0xFFFFFFFFu);
    std::memcpy(b.data() + 0x38, "bnk ", 4);
    putBe32(b, 0x3C, 21);
    putBe32(b, 0x40, 0x380);

    // BSTN. Inner offsets are section-relative.
    std::memcpy(b.data() + 0x80, "BSTN", 4);
    putBe32(b, 0x8C, 0x20);
    putBe32(b, 0xA0, 3);
    putBe32(b, 0xA4, 0x30);
    putBe32(b, 0xB0, 0x0E);
    putBe32(b, 0xB8, 0x48);
    putBe32(b, 0xC8, 1);
    putBe32(b, 0xCC, 0x60);
    putBe32(b, 0xD0, 0x68);
    std::memcpy(b.data() + 0xE0, "SYSTEM", 7);
    std::memcpy(b.data() + 0xE8, "SE_SY_GAME_START", 17);

    // BSC: one SYSTEM sequence, E1 bank 21 program 0.
    b[0x120] = 'S';
    b[0x121] = 'C';
    putBe16(b, 0x122, 0x0E);
    putBe32(b, 0x124, 0x60);
    putBe32(b, 0x128, 0x40);
    putBe32(b, 0x160, 1);
    putBe32(b, 0x164, 0x50);
    b[0x170] = 0xE1;
    b[0x171] = 21;
    b[0x172] = 0;
    b[0x173] = 0xFF;

    // WSYS. Offsets are relative to the WSYS header (JAS TOffset base).
    const size_t w = 0x180;
    std::memcpy(b.data() + w, "WSYS", 4);
    putBe32(b, w + 0x08, 21);
    putBe32(b, w + 0x10, 0x18);
    putBe32(b, w + 0x14, 0xF0);
    putBe32(b, w + 0x18 + 4, 1);
    putBe32(b, w + 0x18 + 8, 0x30);
    std::memcpy(b.data() + w + 0x30, "B21kawa_0.aw", 13);
    putBe32(b, w + 0xA0, 1);
    putBe32(b, w + 0xA4, 0xC0);
    b[w + 0xC0 + 1] = 3; // PCM16
    putFloatBe(b, w + 0xC4, 32000.0f);
    putBe32(b, w + 0xC8, 0);
    putBe32(b, w + 0xCC, 8);
    putBe32(b, w + 0xDC, 4);
    putBe32(b, w + 0xF0 + 8, 1);
    putBe32(b, w + 0xF0 + 0x0C, 0x100);
    putBe32(b, w + 0x100 + 0x0C, 0x110);
    putBe32(b, w + 0x110 + 4, 1);
    putBe32(b, w + 0x110 + 8, 0x120);
    putBe32(b, w + 0x120, 7); // wave id in the low half

    // IBNK bank 21, program 0 → Inst keyboard wave 7.
    const size_t ib = 0x380;
    std::memcpy(b.data() + ib, "IBNK", 4);
    putBe32(b, ib + 0x08, 21);
    putBe32(b, ib + 0x0C, 1);
    std::memcpy(b.data() + ib + 0x20, "LIST", 4);
    putBe32(b, ib + 0x20 + 8, 1);
    putBe32(b, ib + 0x20 + 0x0C, 0x40);
    std::memcpy(b.data() + ib + 0x40, "Inst", 4);
    putBe32(b, ib + 0x40 + 0x10, 1);
    putBe16(b, ib + 0x40 + 0x14 + 0x0C, 21);
    putBe16(b, ib + 0x40 + 0x14 + 0x0E, 7);
    return b;
}

std::vector<uint8_t> yaz0Store(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    out.push_back('Y');
    out.push_back('a');
    out.push_back('z');
    out.push_back('0');
    const uint32_t n = static_cast<uint32_t>(raw.size());
    out.push_back(static_cast<uint8_t>(n >> 24));
    out.push_back(static_cast<uint8_t>(n >> 16));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), 8, 0);
    for (size_t i = 0; i < raw.size();) {
        const size_t chunk = std::min<size_t>(8, raw.size() - i);
        uint8_t flags = 0;
        for (size_t bit = 0; bit < chunk; ++bit) {
            flags |= static_cast<uint8_t>(0x80 >> bit);
        }
        out.push_back(flags);
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(i),
                   raw.begin() + static_cast<std::ptrdiff_t>(i + chunk));
        i += chunk;
    }
    return out;
}

} // namespace

TEST_CASE(audio_game_start_se_decodes_bank_wave) {
    // Other names stay silent — no synthesized stand-in.
    CHECK(!compat::audio::playNamedSystemSe("SE_SY_COIN"));

    const std::vector<uint8_t> baa = makeGameStartBaa();
    // Four PCM16 big-endian samples. Wave id 7, not wave 0, so a wrong index
    // would not reproduce these values. Pad so a "whole file" misread fails.
    const int16_t pcm[4] = {1000, -1000, 2000, -2000};
    std::vector<uint8_t> aw(16, 0x7F);
    for (int i = 0; i < 4; ++i) {
        aw[static_cast<size_t>(i) * 2] = static_cast<uint8_t>(static_cast<uint16_t>(pcm[i]) >> 8);
        aw[static_cast<size_t>(i) * 2 + 1] = static_cast<uint8_t>(pcm[i]);
    }

    std::vector<int16_t> stereo;
    REQUIRE(compat::audio::decodeNamedSystemSe(baa.data(), baa.size(), aw.data(), aw.size(), "SE_SY_GAME_START",
                                               32000, stereo));
    REQUIRE(stereo.size() == 8);
    for (int i = 0; i < 4; ++i) {
        CHECK(stereo[static_cast<size_t>(i) * 2] == pcm[i]);
        CHECK(stereo[static_cast<size_t>(i) * 2 + 1] == pcm[i]);
    }

    // Same archive, Yaz0-wrapped the way SMR.szs is stored on the disc.
    const std::vector<uint8_t> szs = yaz0Store(baa);
    stereo.clear();
    REQUIRE(compat::audio::decodeNamedSystemSe(szs.data(), szs.size(), aw.data(), aw.size(), "SE_SY_GAME_START",
                                               32000, stereo));
    CHECK(stereo.size() == 8);
    CHECK(stereo[0] == 1000);
    CHECK(stereo[6] == -2000);

    // The clip the loader hands to playOneShot is what the mixer outputs.
    REQUIRE(Platform::Audio::init(virtualConfig()));
    REQUIRE(Platform::Audio::playOneShot(stereo.data(), static_cast<int>(stereo.size() / 2), 1.0f));
    int16_t heard[8] = {};
    CHECK(Platform::Audio::pull(heard, 8) == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(heard[i] == stereo[static_cast<size_t>(i)]);
    }
    Platform::Audio::shutdown();
}

TEST_CASE(audio_game_start_se_decodes_adpcm_block) {
    // Format 0 uses the existing DSP ADPCM decoder. One block, known samples.
    std::vector<uint8_t> baa = makeGameStartBaa();
    const size_t w = 0x180;
    baa[w + 0xC0 + 1] = 0; // DSP ADPCM
    putBe32(baa, w + 0xCC, 9);
    putBe32(baa, w + 0xDC, 16);

    uint8_t block[9] = {0x00, 0x10, 0x32, 0x54, 0x76, 0x98, 0xAB, 0xCD, 0xEF};
    int16_t expect[16];
    compat::dsp::AdpcmState state;
    compat::dsp::adpcmDecodeBlock(block, expect, state);

    std::vector<int16_t> stereo;
    REQUIRE(compat::audio::decodeNamedSystemSe(baa.data(), baa.size(), block, sizeof(block), "SE_SY_GAME_START",
                                               32000, stereo));
    REQUIRE(stereo.size() == 32);
    for (int i = 0; i < 16; ++i) {
        CHECK(stereo[static_cast<size_t>(i) * 2] == expect[i]);
    }
}
