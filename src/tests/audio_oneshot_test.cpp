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

#include "platform/Audio/Audio.h"

#include <cstdint>
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
