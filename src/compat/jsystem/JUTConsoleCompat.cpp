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

// --- JUTConsoleManager host definitions --------------------------------------
// The vendored MainLoopFramework::endGX draws the debug console through the
// manager when one exists, and JUTVideo/JUTDirectPrint reference the static.
// On the host no manager is ever created (the on-screen console arrives with
// the JUT milestone, see the header comment), so sManager stays null and
// draw()/drawDirect() are provided as no-ops for the link.
JUTConsoleManager* JUTConsoleManager::sManager = nullptr;

void JUTConsoleManager::draw() const {
    // TODO(PC_PORT, M9.5): on-screen debug console rendering.
}

void JUTConsoleManager::drawDirect(bool) const {
    // TODO(PC_PORT, M9.5): direct-print console rendering.
}
