#ifndef OSTIME_H
#define OSTIME_H

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Change vs. upstream: `OS_BUS_CLOCK_SPEED` is declared `extern` instead of
// being defined as a global (`vu32 OS_BUS_CLOCK_SPEED;`). A global definition
// in a header would produce one symbol per translation unit at host-link time.
// Nothing in the compiled game code references it directly (verified by grep);
// the compat layer defines it once if/when needed. Same rationale as the
// __OSBusClock/__MEM2End fix in revolution/os.h.
// ============================================================================


#include <revolution/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef u32 OSTick;
typedef s64 OSTime;

typedef struct OSCalendarTime {
    int sec;
    int min;
    int hour;
    int mday;
    int mon;
    int year;
    int wday;
    int yday;
    int msec;
    int usec;
} OSCalendarTime;

OSTime OSGetTime(void);
OSTick OSGetTick(void);
OSTime __OSGetSystemTime(void);
OSTime __OSTimeToSystemTime(OSTime);
void OSTicksToCalendarTime(OSTime, OSCalendarTime*);

#ifdef __MWERKS__
vu32 OS_BUS_CLOCK_SPEED : 0x800000F8;
#else
extern vu32 OS_BUS_CLOCK_SPEED;
#endif

#define OS_TIME_SPEED (OS_BUS_CLOCK_SPEED / 4)

#define OS_SEC_TO_TICKS(x) ((x) * (OS_TIME_SPEED))
#define OS_MSEC_TO_TICKS(x) ((x) * (OS_TIME_SPEED / 1000))
#define OS_USEC_TO_TICKS(x) ((x) * (OS_TIME_SPEED / 125000) / 8)
#define OS_NSEC_TO_TICKS(x) ((x) * (OS_TIME_SPEED / 125000) / 8000)

#ifdef __cplusplus
}
#endif

#endif  // OSTIME_H
