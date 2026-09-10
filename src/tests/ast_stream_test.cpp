// M9.5.4 v7: AST stream parser/decoder + the BGM player over the virtual
// audio device.
//
// The synthetic files follow the vgmstream description of Nintendo's AST
// container (STRM header, BLCK blocks with planar channel payloads). Both
// codecs are exercised: PCM16BE (what the SMG discs use) and AFC ADPCM
// (validated against a reference decode of a known frame).
#include "tests/test_runner.h"

#include "compat/audio/AstStream.h"
#include "platform/Audio/Audio.h"
#include "platform/Filesystem/Filesystem.h"
#include "platform/Timing/Timing.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

// Builds a PCM16BE AST: `left`/`right` are the per-channel samples, split
// into blocks of `blockFrames`.
std::vector<uint8_t> makePcmAst(const std::vector<int16_t>& left, const std::vector<int16_t>& right, uint32_t rate,
                                bool loop, uint32_t loopStart, uint32_t loopEnd, size_t blockFrames) {
    std::vector<uint8_t> v;
    v.insert(v.end(), {'S', 'T', 'R', 'M'});
    put32(v, 0);                 // patched below
    put16(v, 1);                 // PCM16BE
    put16(v, 16);
    put16(v, 2);                 // channels
    put16(v, loop ? 0xFFFF : 0);
    put32(v, rate);
    put32(v, static_cast<uint32_t>(left.size()));
    put32(v, loopStart);
    put32(v, loopEnd);
    put32(v, static_cast<uint32_t>(left.size()) * 4);  // first block size (unused by us)
    while (v.size() < 0x40) v.push_back(0);

    for (size_t off = 0; off < left.size(); off += blockFrames) {
        const size_t n = std::min(blockFrames, left.size() - off);
        v.insert(v.end(), {'B', 'L', 'C', 'K'});
        put32(v, static_cast<uint32_t>(n * 2));
        while (v.size() % 0x20 != 0) v.push_back(0);  // header is 0x20 bytes total
        for (size_t i = 0; i < n; ++i) put16(v, static_cast<uint16_t>(left[off + i]));
        for (size_t i = 0; i < n; ++i) put16(v, static_cast<uint16_t>(right[off + i]));
    }
    const uint32_t body = static_cast<uint32_t>(v.size() - 0x40);
    v[4] = static_cast<uint8_t>(body >> 24);
    v[5] = static_cast<uint8_t>(body >> 16);
    v[6] = static_cast<uint8_t>(body >> 8);
    v[7] = static_cast<uint8_t>(body);
    return v;
}

// Reference AFC decode (straight transcription of the vgmstream algorithm,
// kept separate from the implementation under test).
void referenceAfc(const uint8_t* frame, int16_t* out, int32_t& h1, int32_t& h2) {
    static const int16_t coef[16][2] = {
        {0, 0},        {2048, 0},     {0, 2048},     {1024, 1024},  {4096, -2048}, {3584, -1536},
        {3072, -1024}, {4608, -2560}, {4200, -2248}, {4800, -2300}, {5120, -3072}, {2048, -2048},
        {1024, -1024}, {-1024, 1024}, {-1024, 0},    {-2048, 0},
    };
    const int scale = 1 << (frame[0] >> 4);
    const int idx = frame[0] & 0xF;
    for (int i = 0; i < 16; ++i) {
        int nibble = (i & 1) ? (frame[1 + i / 2] & 0xF) : (frame[1 + i / 2] >> 4);
        if (nibble >= 8) nibble -= 16;
        int32_t s = (((nibble * scale) << 11) + coef[idx][0] * h1 + coef[idx][1] * h2) >> 11;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[i] = static_cast<int16_t>(s);
        h2 = h1;
        h1 = s;
    }
}

std::string writeTempAssets(const std::vector<uint8_t>& ast) {
    const std::string root = (std::filesystem::temp_directory_path() / "luma-ast-test").string();
    std::filesystem::create_directories(root + "/AudioRes/Stream");
    std::ofstream f(root + "/AudioRes/Stream/SMG_title_strm.ast", std::ios::binary);
    f.write(reinterpret_cast<const char*>(ast.data()), static_cast<std::streamsize>(ast.size()));
    return root;
}

}  // namespace

TEST_CASE(ast_header_parse_and_reject) {
    std::vector<int16_t> l(64, 100), r(64, -100);
    std::vector<uint8_t> ast = makePcmAst(l, r, 32000, true, 16, 60, 32);

    compat::audio::AstInfo info;
    REQUIRE(compat::audio::parseAstHeader(ast.data(), ast.size(), info));
    CHECK(info.codec == 1);
    CHECK(info.channels == 2);
    CHECK(info.sampleRate == 32000u);
    CHECK(info.numSamples == 64u);
    CHECK(info.looped);
    CHECK(info.loopStart == 16u);
    CHECK(info.loopEnd == 60u);

    // Foreign magic / truncated / bad codec.
    std::vector<uint8_t> bad = ast;
    bad[0] = 'X';
    CHECK(!compat::audio::parseAstHeader(bad.data(), bad.size(), info));
    CHECK(!compat::audio::parseAstHeader(ast.data(), 0x20, info));
    bad = ast;
    bad[9] = 7;
    CHECK(!compat::audio::parseAstHeader(bad.data(), bad.size(), info));

    // Degenerate loop (end <= start) → treated as not looped.
    std::vector<uint8_t> degenerate = makePcmAst(l, r, 32000, true, 40, 40, 32);
    REQUIRE(compat::audio::parseAstHeader(degenerate.data(), degenerate.size(), info));
    CHECK(!info.looped);
}

TEST_CASE(ast_pcm16_blocks_decode_interleaved) {
    // Distinct ramps per channel across three blocks (the last one short).
    std::vector<int16_t> l(80), r(80);
    for (int i = 0; i < 80; ++i) {
        l[static_cast<size_t>(i)] = static_cast<int16_t>(i * 100);
        r[static_cast<size_t>(i)] = static_cast<int16_t>(-i * 50);
    }
    std::vector<uint8_t> ast = makePcmAst(l, r, 32000, false, 0, 0, 32);

    compat::audio::AstInfo info;
    std::vector<int16_t> pcm;
    REQUIRE(compat::audio::decodeAstToStereo(ast.data(), ast.size(), info, pcm));
    REQUIRE(pcm.size() == 160u);
    bool ok = true;
    for (int i = 0; i < 80; ++i) {
        if (pcm[static_cast<size_t>(i) * 2] != l[static_cast<size_t>(i)] || pcm[static_cast<size_t>(i) * 2 + 1] != r[static_cast<size_t>(i)]) {
            ok = false;
        }
    }
    CHECK(ok);

    // A truncated last block is dropped, not read past the end.
    std::vector<uint8_t> cut(ast.begin(), ast.end() - 10);
    REQUIRE(compat::audio::decodeAstToStereo(cut.data(), cut.size(), info, pcm));
    CHECK(pcm.size() == 128u);  // two full blocks of 32 frames
}

TEST_CASE(ast_afc_decoder_matches_reference) {
    // Two frames with different scale/coef headers and a mix of nibbles.
    const uint8_t frames[18] = {
        0x53, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
        0x2B, 0x7F, 0x80, 0x01, 0xFE, 0x10, 0x0F, 0xAA, 0x55,
    };
    int16_t got[32];
    int16_t hist[2] = {0, 0};
    compat::audio::decodeAfcFrames(frames, 2, got, hist);

    int16_t want[32];
    int32_t h1 = 0, h2 = 0;
    referenceAfc(frames, want, h1, h2);
    referenceAfc(frames + 9, want + 16, h1, h2);

    CHECK(std::memcmp(got, want, sizeof(got)) == 0);
    CHECK(hist[0] == static_cast<int16_t>(h1));
    CHECK(hist[1] == static_cast<int16_t>(h2));
}

TEST_CASE(ast_label_table) {
    char path[128];
    CHECK(compat::audio::streamPathForLabel("STM_TITLE", path, sizeof(path)));
    CHECK(std::strcmp(path, "/AudioRes/Stream/SMG_title_strm.ast") == 0);
    CHECK(compat::audio::streamPathForLabel("stm_file_select", path, sizeof(path)));  // case-insensitive
    CHECK(std::strcmp(path, "/AudioRes/Stream/SMG_fileselect_strm.ast") == 0);
    CHECK(!compat::audio::streamPathForLabel("STM_DOES_NOT_EXIST", path, sizeof(path)));
    CHECK(std::strcmp(path, "/AudioRes/Stream/STM_DOES_NOT_EXIST.ast") == 0);
}

TEST_CASE(ast_player_streams_loops_and_fades) {
    // Virtual audio device (no SDL), small ring so the pacing path runs.
    Platform::Audio::Config cfg;
    cfg.enable = false;
    cfg.inputFreq = 32000;
    cfg.latencyMs = 40;
    REQUIRE(Platform::Audio::init(cfg));

    // 1000 frames, loop [200, 800): the player must keep producing well past
    // 1000 frames when looping and stop at 1000 when not.
    std::vector<int16_t> l(1000), r(1000);
    for (int i = 0; i < 1000; ++i) {
        l[static_cast<size_t>(i)] = static_cast<int16_t>(1000 + i);
        r[static_cast<size_t>(i)] = static_cast<int16_t>(-(1000 + i));
    }
    const std::string root = writeTempAssets(makePcmAst(l, r, 32000, true, 200, 800, 256));
    const std::string savedRoot = Platform::Filesystem::getRootDir();
    Platform::Filesystem::setRootDir(root);

    // Missing file → false, nothing playing.
    CHECK(!compat::audio::startStream("/AudioRes/Stream/nope.ast", true));
    CHECK(!compat::audio::isStreamPlaying());

    // Looping playback: drain the ring like the device would and verify the
    // sample sequence wraps from 799 back to 200.
    REQUIRE(compat::audio::startStream("/AudioRes/Stream/SMG_title_strm.ast", true));
    CHECK(compat::audio::isStreamPlaying());
    CHECK(std::strcmp(compat::audio::currentStreamPath(), "/AudioRes/Stream/SMG_title_strm.ast") == 0);

    std::vector<int16_t> drained;
    std::vector<int16_t> tmp(512);
    const double deadline = Platform::Timing::nowSeconds() + 5.0;
    while (drained.size() < 2 * 2600 && Platform::Timing::nowSeconds() < deadline) {
        const int n = Platform::Audio::pull(tmp.data(), static_cast<int>(tmp.size()));
        if (n > 0) {
            drained.insert(drained.end(), tmp.begin(), tmp.begin() + n);
        } else {
            Platform::Timing::sleepSeconds(0.002);
        }
    }
    REQUIRE(drained.size() >= 2 * 2600);

    // Expected per-frame left values: 1000..1799, then 1200..1799 repeating.
    bool sequenceOk = true;
    int expected = 1000;
    for (size_t f = 0; f < 2600; ++f) {
        if (drained[f * 2] != expected || drained[f * 2 + 1] != -expected) {
            sequenceOk = false;
            break;
        }
        ++expected;
        if (expected == 1800) expected = 1200;
    }
    CHECK(sequenceOk);

    // Fade-out over 6 frames (= 3200 samples @ 32 kHz): the stream ends by
    // itself and the tail is attenuated to (near) silence.
    compat::audio::stopStream(6);
    const double fadeDeadline = Platform::Timing::nowSeconds() + 5.0;
    drained.clear();
    while (Platform::Timing::nowSeconds() < fadeDeadline) {
        const int n = Platform::Audio::pull(tmp.data(), static_cast<int>(tmp.size()));
        if (n > 0) {
            drained.insert(drained.end(), tmp.begin(), tmp.begin() + n);
        } else if (!compat::audio::isStreamPlaying()) {
            break;
        } else {
            Platform::Timing::sleepSeconds(0.002);
        }
    }
    CHECK(!compat::audio::isStreamPlaying());
    REQUIRE(drained.size() >= 64);
    int16_t lastAbs = 0;
    for (size_t i = drained.size() - 64; i < drained.size(); ++i) {
        lastAbs = std::max<int16_t>(lastAbs, static_cast<int16_t>(std::abs(drained[i])));
    }
    CHECK(lastAbs < 200);  // full-scale values were ~1200-1800

    // Non-looping playback ends after exactly numSamples frames.
    REQUIRE(compat::audio::startStream("/AudioRes/Stream/SMG_title_strm.ast", false));
    size_t total = 0;
    const double endDeadline = Platform::Timing::nowSeconds() + 5.0;
    while (Platform::Timing::nowSeconds() < endDeadline) {
        const int n = Platform::Audio::pull(tmp.data(), static_cast<int>(tmp.size()));
        if (n > 0) {
            total += static_cast<size_t>(n);
        } else if (!compat::audio::isStreamPlaying()) {
            break;
        } else {
            Platform::Timing::sleepSeconds(0.002);
        }
    }
    // Drain whatever is left after the thread finished.
    for (;;) {
        const int n = Platform::Audio::pull(tmp.data(), static_cast<int>(tmp.size()));
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    CHECK(total == 2000u);  // 1000 frames * 2 channels

    compat::audio::shutdownStreams();
    Platform::Filesystem::setRootDir(savedRoot);
    Platform::Audio::shutdown();
}
