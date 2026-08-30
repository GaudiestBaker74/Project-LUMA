// =============================================================================
// compat/os — OSReport / OSPanic emulation.
//
//   OSReport  -> Platform::Log (Debug level — it is a debug printf on Wii)
//   OSPanic   -> Platform::Log (Fatal) + abort (same observable behavior as
//                the Wii OS, which halts with an error screen)
// =============================================================================

#include "platform/platform.h"

#include <revolution.h>

#include <cstdarg>
#include <cstdio>

namespace {
void formatInto(char* buffer, size_t bufferSize, const char* fmt, va_list args) {
    if (!buffer || bufferSize == 0) {
        return;
    }
    buffer[0] = '\0';
    if (fmt) {
        std::vsnprintf(buffer, bufferSize, fmt, args);
    }
}
} // namespace

extern "C" {

void OSReport(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    formatInto(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    PL_LOG_DEBUG("OSReport", "%s", buffer);
}

void OSPanic(const char* file, int line, const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    formatInto(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    // Log::fatal aborts by default.
    PL_LOG_FATAL("OSPanic", "panic at %s:%d: %s", file ? file : "?", line, buffer);
}

} // extern "C"
