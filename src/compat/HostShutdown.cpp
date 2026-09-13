// =============================================================================
// compat/HostShutdown.cpp — see HostShutdown.h for the ordering contract.
// =============================================================================

#include "compat/HostShutdown.h"

#include <atomic>
#include <cstdlib>

#include "compat/audio/AstStream.h"
#include "compat/dvd/DVDCompat.h"
#include "compat/gx/GXCompat.h"
#include "compat/kpad/KPADCompat.h"  // Platform::CompatInput::shutdown
#include "compat/os/OSCompat.h"      // compat::shutdownAlarmThread
#include "compat/vi/VICompat.h"      // Platform::CompatVi::shutdownFieldClock

#include "platform/Audio/Audio.h"
#include "platform/Log/Log.h"
#include "platform/Renderer/Renderer.h"
#include "platform/platform.h"

namespace compat {

namespace {

std::atomic<bool> sShutdownStarted{false};

} // namespace

bool isHostShutdownInProgress() {
    return sShutdownStarted.load(std::memory_order_acquire);
}

void shutdownHostForExit() {
    // First caller wins; the others return so a worker thread that notices the
    // teardown cannot re-enter it (it is about to be joined below).
    if (sShutdownStarted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    PL_LOG_INFO("compat", "host shutdown: joining worker threads, then releasing GX/renderer/platform");

    // --- 1. Worker threads -------------------------------------------------
    // Order between the three does not matter (they are independent), but all
    // of them MUST be gone before the heap/renderer teardown below: they run
    // game code (alarm handlers, retrace callbacks, DVD read callbacks).
    Platform::CompatVi::shutdownFieldClock();
    compat::shutdownAlarmThread();
    compat::shutdownDVD();

    // --- 2. Host subsystems ------------------------------------------------
    GXCompatShutdown();     // release the GX dynamic vertex buffer
    Platform::CompatInput::shutdown();
    Platform::Renderer::shutdown();

    // Audio before Platform::shutdown() (which turns logging off) so the
    // "audio: device closed" record still lands in the log; the atexit handler
    // registered in main.cpp then finds an already-shut-down device and is a
    // no-op (Platform::Audio::shutdown and shutdownStreams are idempotent).
    compat::audio::shutdownStreams();
    Platform::Audio::shutdown();

    PL_LOG_INFO("compat", "host shutdown complete — exiting");
    Platform::shutdown();  // logging off last

    std::exit(0);
}

} // namespace compat
