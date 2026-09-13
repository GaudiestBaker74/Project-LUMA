#pragma once
// =============================================================================
// compat/HostShutdown — the single exit path of the host process (PC_PORT).
//
// The console never "exits": the player presses POWER and the hardware stops.
// On the host there are three ways out of a boot run (window close, SDL_QUIT —
// which is also what SIGTERM maps to on Windows —, and the native loop's own
// end), and ALL of them must tear the emulated machine down in ONE order:
//
//   1. the worker threads that run game code (VI field clock, OS alarm thread,
//      DVD reader) are stopped and joined FIRST. After that nothing else
//      allocates through the game heap or touches the renderer;
//   2. then the host subsystems, in the order main.cpp's clean-shutdown block
//      uses: GX resources -> input -> renderer -> audio -> platform.
//
// The old --boot path called std::exit(0) straight from the SDL event pump
// (compat/vi/VICompat.cpp). That runs ONLY the atexit handlers: the renderer,
// the DVD worker and the platform were never shut down, and the worker threads
// were joined in whatever order the atexit registrations happened to have —
// which on Windows destroyed the OSMutex registry while the field clock was
// still running and produced the "*** CRASH *** EXCEPTION_ACCESS_VIOLATION /
// getMutex <- OSLockMutex <- JKRExpHeap::do_free" that ends the reference
// boot.log sessions. Every exit now funnels through here.
// =============================================================================

namespace compat {

/// True once shutdownHostForExit() has started tearing the process down.
bool isHostShutdownInProgress();

/// Stops the emulated machine and exits the process with status 0. Never
/// returns; re-entrant calls (or calls from the worker threads themselves)
/// are no-ops.
void shutdownHostForExit();

} // namespace compat
