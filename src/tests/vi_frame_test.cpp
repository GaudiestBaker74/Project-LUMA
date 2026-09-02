// =============================================================================
// M9.3: VI + frame control —
//   - VI field clock: VIInit starts a 59.94 Hz field clock; VIGetRetraceCount
//     advances, pre/post retrace callbacks fire and VIWaitForRetrace blocks
//     one field. This is the surface MainLoopFramework::waitForRetrace runs
//     on (JUTVideo's postRetraceProc pings its message queue).
//   - OSAlarm: one-shot / periodic / cancel (the GX watchdog in
//     MainLoopFramework::waitDrawDoneAndSetAlarm uses these).
// =============================================================================

#include "tests/test_runner.h"

#include <revolution/os.h>
#include <revolution/vi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {

std::atomic<u32> gPreCount{0};
std::atomic<u32> gPostCount{0};

void preRetraceCb(u32) {
    gPreCount.fetch_add(1);
}

void postRetraceCb(u32) {
    gPostCount.fetch_add(1);
}

std::atomic<int> gAlarmFired{0};
OSAlarm gAlarm;

void alarmHandler(OSAlarm*, OSContext*) {
    gAlarmFired.fetch_add(1);
}

} // namespace

TEST_CASE(vi_field_clock_ticks_at_60hz) {
    VIInit();
    // 59.94 Hz => ~12 fields per 200 ms; allow slow CI (5..25).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const u32 count = VIGetRetraceCount();
    CHECK(count >= 5);
    CHECK(count <= 25);
    CHECK_EQ(VIGetTvFormat(), static_cast<u32>(VI_NTSC));
    CHECK_EQ(VIGetScanMode(), static_cast<u32>(VI_NON_INTERLACE));
}

TEST_CASE(vi_wait_for_retrace_blocks_about_one_field) {
    const u32 before = VIGetRetraceCount();
    // A single wait may return instantly if a field fires right after the
    // snapshot; five sequential waits must span at least ~4 fields anyway.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        VIWaitForRetrace();
    }
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    CHECK(VIGetRetraceCount() - before >= 5); // each wait consumed a field
    CHECK(dt >= 50);                          // ~4-5 fields at 59.94 Hz
    CHECK(dt < 500);                          // and did not hang
}

TEST_CASE(vi_retrace_callbacks_fire) {
    gPreCount.store(0);
    gPostCount.store(0);
    const VIRetraceCallback oldPre = VISetPreRetraceCallback(preRetraceCb);
    const VIRetraceCallback oldPost = VISetPostRetraceCallback(postRetraceCb);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(gPreCount.load() >= 3);
    CHECK(gPostCount.load() >= 3);
    VISetPreRetraceCallback(oldPre);
    VISetPostRetraceCallback(oldPost);
}

TEST_CASE(os_alarm_one_shot_fires) {
    gAlarmFired.store(0);
    OSCreateAlarm(&gAlarm);
    OSSetAlarm(&gAlarm, OSMillisecondsToTicks(30), alarmHandler);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(gAlarmFired.load() >= 1);
    OSCancelAlarm(&gAlarm);
}

TEST_CASE(os_alarm_cancel_prevents_fire) {
    gAlarmFired.store(0);
    OSCreateAlarm(&gAlarm);
    OSSetAlarm(&gAlarm, OSMillisecondsToTicks(30), alarmHandler);
    OSCancelAlarm(&gAlarm);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK_EQ(gAlarmFired.load(), 0);
}

TEST_CASE(os_alarm_periodic_fires_repeatedly) {
    gAlarmFired.store(0);
    OSCreateAlarm(&gAlarm);
    OSSetPeriodicAlarm(&gAlarm, OSMillisecondsToTicks(20), OSMillisecondsToTicks(40),
                       alarmHandler);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    CHECK(gAlarmFired.load() >= 2);
    OSCancelAlarm(&gAlarm);
    const int fired = gAlarmFired.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(gAlarmFired.load(), fired); // cancelled: no more fires
}
