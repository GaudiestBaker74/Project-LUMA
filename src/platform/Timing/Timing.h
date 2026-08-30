#pragma once
// =============================================================================
// Platform::Timing — high-resolution monotonic clock.
//
// Backing for the game's tick/time needs (compat/os maps OSGetTime/OSGetTick
// onto this module). All clocks are monotonic (steady); wall-clock time is
// only used by the logger for timestamps.
// =============================================================================

#include <chrono>
#include <cstdint>

namespace Platform::Timing {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// Current monotonic time point.
TimePoint now();

// Seconds between two time points (b - a).
double secondsBetween(TimePoint a, TimePoint b);
// Seconds elapsed since `t` (b == now()).
double secondsSince(TimePoint t);
// Nanoseconds elapsed since `t`.
uint64_t nanosecondsSince(TimePoint t);

// Seconds since the first call to any Timing function in this process.
double nowSeconds();

// Raw monotonic ticks (~1 ns each) and ticks-per-second. This is the backing
// for the Wii OSGetTick() compat function.
uint64_t ticks();
uint64_t ticksPerSecond();

// Sleeps for the given duration (nanoseconds resolution).
void sleepSeconds(double seconds);
void sleepMicroseconds(uint64_t microseconds);

} // namespace Platform::Timing
