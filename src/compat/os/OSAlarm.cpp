// compat/os — OSAlarm host implementation (M9.3).
//
// The Wii fires OSAlarm handlers from the timer interrupt context; game code
// arms them for watchdogs and periodic work. The host implementation keeps a
// single alarm thread with a sorted pending list:
//   - OSCreateAlarm       initialize the OSAlarm record
//   - OSSetAlarm          (re)arm at now + tick (one-shot)
//   - OSSetPeriodicAlarm  arm at `start`, repeating every `period`
//   - OSCancelAlarm       dequeue (also re-arming reschedules in place)
//
// Handlers are dispatched on the alarm thread with the queue lock held, so
// OSCancelAlarm can never return while its (possibly stack-allocated) alarm's
// handler is still running — the only caller in the boot path,
// MainLoopFramework::waitDrawDoneAndSetAlarm, cancels the watchdog right
// after GXDrawDone and depends on exactly that guarantee.
//
// Ticks use OSGetTime()'s unit (OS_TIMER_CLOCK = 60.75 MHz), matching the
// vendored OSTime.cpp.
#include "compat/os/OSCompat.h"

#include <revolution/os/OSAlarm.h>
#include <revolution/os/OSTime.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kTimerHz = 60750000ull; // OS_TIMER_CLOCK (see OSTime.cpp)

struct AlarmEntry {
    OSAlarm* alarm;
    OSTime fire;   // absolute, in OSGetTime() ticks
    OSTime period; // 0 = one-shot
};

std::mutex sAlarmMutex;
std::condition_variable sAlarmCv;
std::vector<AlarmEntry> sPending; // sorted by `fire`
std::thread sAlarmThread;
bool sThreadRunning = false;
bool sQuit = false;

std::chrono::microseconds ticksToUsec(OSTime ticks) {
    return std::chrono::microseconds(static_cast<int64_t>(ticks * 1000000ull / kTimerHz));
}

void insertAlarm(OSAlarm* alarm, OSTime fire, OSTime period) {
    // Remove any existing entry (OSSetAlarm reschedules), then insert sorted.
    sPending.erase(std::remove_if(sPending.begin(), sPending.end(),
                                  [&](const AlarmEntry& e) { return e.alarm == alarm; }),
                   sPending.end());
    AlarmEntry e{};
    e.alarm = alarm;
    e.fire = fire;
    e.period = period;
    auto it = std::upper_bound(sPending.begin(), sPending.end(), e,
                               [](const AlarmEntry& a, const AlarmEntry& b) {
                                   return a.fire < b.fire;
                               });
    sPending.insert(it, e);
}

void alarmThreadMain() {
    for (;;) {
        std::unique_lock<std::mutex> lock(sAlarmMutex);
        if (sQuit) {
            return;
        }
        if (sPending.empty()) {
            sAlarmCv.wait(lock);
            continue;
        }
        if (sPending.front().fire <= OSGetTime()) {
            AlarmEntry due = sPending.front();
            sPending.erase(sPending.begin());
            OSAlarm* alarm = due.alarm;
            if (due.period != 0) {
                // Periodic: reschedule for the next interval. The handler is
                // dispatched with the lock held (see the file comment).
                insertAlarm(alarm, due.fire + due.period, due.period);
            }
            if (alarm->handler != nullptr) {
                alarm->handler(alarm, nullptr);
            }
            continue;
        }
        sAlarmCv.wait_for(lock, ticksToUsec(sPending.front().fire - OSGetTime()));
    }
}

void shutdownAlarmThread() {
    {
        std::lock_guard<std::mutex> lock(sAlarmMutex);
        sQuit = true;
    }
    sAlarmCv.notify_all();
    if (sAlarmThread.joinable()) {
        sAlarmThread.join();
    }
}

void ensureAlarmThread() {
    if (!sThreadRunning) {
        sThreadRunning = true;
        sQuit = false;
        sAlarmThread = std::thread(alarmThreadMain);
        // Joined at process exit from the atexit handler above (the Wii OS
        // also tears alarms down at shutdown); keeping it joinable lets the
        // suite binary terminate cleanly.
        std::atexit(shutdownAlarmThread);
    }
}

} // namespace

extern "C" {

void OSCreateAlarm(OSAlarm* alarm) {
    if (alarm == nullptr) {
        return;
    }
    std::memset(alarm, 0, sizeof(*alarm));
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    if (alarm == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sAlarmMutex);
        alarm->handler = handler;
        alarm->fire = OSGetTime() + tick;
        alarm->period = 0;
        insertAlarm(alarm, alarm->fire, 0);
        ensureAlarmThread();
    }
    sAlarmCv.notify_all();
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    if (alarm == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sAlarmMutex);
        alarm->handler = handler;
        alarm->fire = OSGetTime() + start;
        alarm->period = period;
        insertAlarm(alarm, alarm->fire, period);
        ensureAlarmThread();
    }
    sAlarmCv.notify_all();
}

void OSCancelAlarm(OSAlarm* alarm) {
    if (alarm == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sAlarmMutex);
        sPending.erase(std::remove_if(sPending.begin(), sPending.end(),
                                      [&](const AlarmEntry& e) { return e.alarm == alarm; }),
                       sPending.end());
    }
    sAlarmCv.notify_all();
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) {
    if (alarm != nullptr) {
        alarm->tag = tag;
    }
}

void OSSetAlarmUserData(OSAlarm* alarm, void* userData) {
    if (alarm != nullptr) {
        alarm->userData = userData;
    }
}

void* OSGetAlarmUserData(const OSAlarm* alarm) {
    return alarm != nullptr ? alarm->userData : nullptr;
}

} // extern "C"
