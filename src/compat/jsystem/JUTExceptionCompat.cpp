// =============================================================================
// compat/jsystem — JUTException minimal emulation.
//
// JUTException is JSystem's error/panic handler (prints an error screen with
// register dumps on the Wii). Only `panic_f`/`panic_f_va` are used by the game
// code we compile so far (JKRHeap's abort path). They are routed to
// Platform::Log (Fatal) which aborts.
//
// TODO(PC_PORT): a full JUTException port would include a native stack trace
// dump; useful for debugging later milestones.
// =============================================================================

#include "platform/platform.h"

#include <JSystem/JUtility/JUTException.hpp>

#include <cstdarg>
#include <cstdio>

void JUTException::panic_f_va(const char* file, int line, const char* fmt, va_list args) {
    char buffer[1024];
    if (fmt) {
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    } else {
        buffer[0] = '\0';
    }
    PL_LOG_FATAL("JUTException", "panic at %s:%d: %s", file ? file : "?", line, buffer);
}

void JUTException::panic_f(const char* file, int line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    panic_f_va(file, line, fmt, args);
    va_end(args);
}
