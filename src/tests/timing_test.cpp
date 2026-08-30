#include "tests/test_runner.h"

#include "platform/Timing/Timing.h"

#include <cstdint>

TEST_CASE(timing_monotonic) {
    const auto t0 = Platform::Timing::now();
    Platform::Timing::sleepSeconds(0.001);
    const auto t1 = Platform::Timing::now();
    CHECK(Platform::Timing::secondsBetween(t0, t1) >= 0.0005);
    CHECK(Platform::Timing::secondsBetween(t1, t0) < 0.0); // order matters
    CHECK(Platform::Timing::secondsSince(t0) >= 0.0);
}

TEST_CASE(timing_sleep_accuracy) {
    const auto start = Platform::Timing::now();
    Platform::Timing::sleepSeconds(0.02);
    const double elapsed = Platform::Timing::secondsSince(start);
    CHECK(elapsed >= 0.015);
    CHECK(elapsed < 1.0);

    const auto startUs = Platform::Timing::now();
    Platform::Timing::sleepMicroseconds(5000);
    CHECK(Platform::Timing::nanosecondsSince(startUs) >= 4000000ull);
}

TEST_CASE(timing_ticks) {
    const uint64_t a = Platform::Timing::ticks();
    Platform::Timing::sleepMicroseconds(1000);
    const uint64_t b = Platform::Timing::ticks();
    CHECK(b > a);
    CHECK(Platform::Timing::ticksPerSecond() > 0);
}

TEST_CASE(timing_now_seconds) {
    const double a = Platform::Timing::nowSeconds();
    Platform::Timing::sleepSeconds(0.002);
    const double b = Platform::Timing::nowSeconds();
    CHECK(b >= a);
    CHECK(b - a >= 0.001);
}
