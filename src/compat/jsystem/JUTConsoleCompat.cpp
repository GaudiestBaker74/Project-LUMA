// =============================================================================
// compat/jsystem — JUTConsole minimal emulation.
//
// JUTConsole is JSystem's on-screen debug console. For the PC port the console
// functions are routed to Platform::Log. The full JUTConsole class (with its
// on-screen rendering) is not needed until the game actually draws its debug
// console, which it only does in debug builds.
//
// TODO(PC_PORT): if the game's debug console output ever needs to be rendered
// on screen (developer builds), port JUTConsole.cpp proper.
// =============================================================================

#include "platform/platform.h"

#include <JSystem/JUtility/JUTConsole.hpp>

#include <cstdarg>
#include <cstdio>

extern "C" {

void JUTWarningConsole_f(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt ? fmt : "", args);
    va_end(args);
    PL_LOG_WARN("JUTConsole", "%s", buffer);
}

void JUTReportConsole_f(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt ? fmt : "", args);
    va_end(args);
    PL_LOG_DEBUG("JUTConsole", "%s", buffer);
}

} // extern "C"
