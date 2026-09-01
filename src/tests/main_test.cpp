// =============================================================================
// Test binary entry point.
//
// Initializes the platform layer and the compat OS (console RAM arena) before
// running the suite — the JKR heap smoke tests need the arena, exactly like
// the real game boot does.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/dvd/DVDCompat.h"
#include "compat/os/OSCompat.h"
#include "platform/platform.h"

#include <cstdio>

int main(int argc, char** argv) {
    Platform::init();

    // The compat OS layer reserves the emulated console RAM (see
    // compat/os/OSArena.cpp). This mirrors what the Wii OS does before the
    // game's main() runs.
    compat::initOS();

    // Keep the console quiet during tests unless a failure needs reporting.
    Platform::Log::setMinLevel(Platform::Log::Level::Warn);

    const int result = pc_test::runSuite(argc, argv);

    // Stop the DVD async worker (started by the async tests, if any). Safe
    // to call unconditionally: it is a no-op when the worker was never
    // started.
    compat::shutdownDVD();

    Platform::shutdown();
    return result;
}
