// compat/os — OS time (OSTime.h) over Platform::Timing.
//
// The console's ticks are OS_TIMER_CLOCK = OS_BUS_CLOCK/4 (60.75 MHz on the
// Wii). JASProbe and JASDriver (audio) use OSGetTime/OSGetTick for profiling
// and watchdog math; these are mapped to the host monotonic clock.
#include "platform/Timing/Timing.h"

#include <revolution/os/OSTime.h>

#include <cstdint>
#include <ctime>

namespace {
constexpr uint64_t kTimerHz = 60750000ull; // OS_BUS_CLOCK(243 MHz) / 4

// The console's time base: ticks since boot (the timer starts when the
// console powers on). Latch the process start of the PC port.
const Platform::Timing::TimePoint kProcessStart = Platform::Timing::now();
}

// Console global mirror (declared `extern` by compat/include/revolution/os.h);
// the JAS probes (JASProbe.cpp) read it directly, and OS_BUS_CLOCK derives
// the timer frequency from it.
u32 __OSBusClock = 243000000u;

extern "C" {

OSTime OSGetTime(void) {
    // Monotonic uptime in console ticks (the old code measured from a
    // now() captured on the spot, which is ~always 0 — fixed for M9.3:
    // OSAlarm and MainLoopFramework::waitForTick rely on a real uptime).
    return static_cast<OSTime>(Platform::Timing::nanosecondsSince(kProcessStart) *
                               kTimerHz / 1000000000ull);
}

OSTick OSGetTick(void) {
    return static_cast<OSTick>(OSGetTime() & 0xFFFFFFFFull);
}

OSTime __OSGetSystemTime(void) {
    // Wall-clock since epoch, in console ticks (used by the RTC/nwc24 layer;
    // the audio path only uses OSGetTime).
    return static_cast<OSTime>(static_cast<uint64_t>(std::time(nullptr)) * kTimerHz);
}

OSTime __OSTimeToSystemTime(OSTime ticks) {
    return static_cast<OSTime>(static_cast<uint64_t>(ticks) / kTimerHz * 1000000000ull);
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* out) {
    if (out == nullptr) {
        return;
    }
    *out = OSCalendarTime{};
}

} // extern "C"

// The header declares this global 'extern' on non-Metrowerks; define it once
// (see the header comment).
vu32 OS_BUS_CLOCK_SPEED = 243000000u;
