// =============================================================================
// M8: Platform::Audio::RingBuffer unit tests (pure, no SDL — headless).
//
// The ring is the SPSC FIFO between the JAS audio thread (writer) and the SDL
// get-callback / test sink (reader). Covers: round-trip, wrap-around, overflow
// drop, partial reads, clear, and the lock-free acquire/release ordering at
// the API level (single-threaded here; the ordering is exercised by the
// driver test in jas_audio_driver_test.cpp).
// =============================================================================

#include "tests/test_runner.h"

#include "platform/Audio/RingBuffer.h"

#include <cstdint>

TEST_CASE(audio_ring_buffer_roundtrip) {
    Platform::Audio::RingBuffer ring(64); // capacity rounds up to 64 (pow2)
    CHECK(ring.capacity() == 63);          // one slot kept free

    int16_t in[] = {10, 20, 30, 40, 50};
    CHECK(ring.write(in, 5) == 5);
    CHECK(ring.available() == 5);

    int16_t out[8] = {};
    CHECK(ring.read(out, 8) == 5); // fewer available than requested
    for (int i = 0; i < 5; ++i) {
        CHECK(out[i] == in[i]);
    }
    CHECK(ring.available() == 0);
    CHECK(ring.read(out, 8) == 0); // empty read is fine
}

TEST_CASE(audio_ring_buffer_wraparound) {
    Platform::Audio::RingBuffer ring(64);

    // Fill more than the capacity in two chunks so the head wraps past the
    // tail (the classic SPSC wrap-around bug class).
    int16_t chunk[40];
    for (int i = 0; i < 40; ++i) chunk[i] = static_cast<int16_t>(i);
    CHECK(ring.write(chunk, 40) == 40);
    CHECK(ring.available() == 40);
    CHECK(ring.write(chunk, 40) == 23); // capacity 63: only 23 slots left

    int16_t out[64] = {};
    CHECK(ring.read(out, 64) == 63);
    for (int i = 0; i < 40; ++i) CHECK(out[i] == static_cast<int16_t>(i));
    for (int i = 40; i < 63; ++i) CHECK(out[i] == static_cast<int16_t>(i - 40));
    CHECK(ring.available() == 0);
}

TEST_CASE(audio_ring_buffer_overflow_drops) {
    Platform::Audio::RingBuffer ring(64);
    int16_t in[100];
    for (int i = 0; i < 100; ++i) in[i] = static_cast<int16_t>(0x100 + i);

    // Write more than capacity: the excess is dropped, the ring stays intact.
    CHECK(ring.write(in, 100) == 63);
    CHECK(ring.available() == 63);

    // The oldest data must survive (head never overtakes tail).
    int16_t out[64] = {};
    CHECK(ring.read(out, 63) == 63);
    CHECK(out[0] == 0x100);
    CHECK(out[62] == static_cast<int16_t>(0x100 + 62));
}

TEST_CASE(audio_ring_buffer_clear_and_partial) {
    Platform::Audio::RingBuffer ring(256);
    int16_t in[64];
    for (int i = 0; i < 64; ++i) in[i] = static_cast<int16_t>(i * 3);

    CHECK(ring.write(in, 64) == 64);
    ring.clear();
    CHECK(ring.available() == 0);

    // Partial write + partial read interleaved.
    CHECK(ring.write(in, 10) == 10);
    int16_t out[4] = {};
    CHECK(ring.read(out, 4) == 4);
    CHECK(out[0] == 0 && out[3] == 9);
    CHECK(ring.available() == 6);
    CHECK(ring.write(in, 10) == 10);
    CHECK(ring.available() == 16);
}
