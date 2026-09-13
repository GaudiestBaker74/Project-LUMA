#pragma once
// =============================================================================
// compat:: — PC-side glue for the Wii compatibility layer.
//
// The compat/os implementation emulates the console's low-level services
// (RAM arena, boot header, mutexes, logging/panics) on top of Platform::*.
// These PC-specific entry points are called by main.cpp before any game code
// runs; they are NOT part of the Wii API surface.
// =============================================================================

#include <cstddef>

namespace compat {

// Reserves the emulated console RAM arena and initializes the OS arena
// state (OSGetArenaLo/Hi, boot header stub). Safe to call once.
void initOS();

// Size of the reserved arena in bytes.
std::size_t arenaSizeBytes();

// Pointer to the host-side OSBootInfo stub (what the game's JKRHeap reads as
// the DOL boot header; see OSArena.cpp). Used by the JKRHeap.cpp PC patch.
const void* getBootInfo();

// Stops the emulated OS alarm thread and joins it (PC_PORT teardown hook; also
// registered with atexit by the thread itself). Idempotent and safe to call
// from any thread, including the alarm thread. See OSAlarm.cpp.
void shutdownAlarmThread();

} // namespace compat
