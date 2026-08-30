#include "platform/Timing/Timing.h"

#include <thread>

namespace Platform::Timing {

TimePoint now() {
    return Clock::now();
}

double secondsBetween(TimePoint a, TimePoint b) {
    return std::chrono::duration<double>(b - a).count();
}

double secondsSince(TimePoint t) {
    return secondsBetween(t, now());
}

uint64_t nanosecondsSince(TimePoint t) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now() - t).count());
}

double nowSeconds() {
    static const TimePoint processStart = now();
    return secondsSince(processStart);
}

uint64_t ticks() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     now().time_since_epoch())
                                     .count());
}

uint64_t ticksPerSecond() {
    return 1000000000ull; // 1 ns per tick
}

void sleepSeconds(double seconds) {
    if (seconds <= 0.0) {
        return;
    }
    const auto duration = std::chrono::duration<double>(seconds);
    std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::nanoseconds>(duration));
}

void sleepMicroseconds(uint64_t microseconds) {
    if (microseconds == 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

} // namespace Platform::Timing
