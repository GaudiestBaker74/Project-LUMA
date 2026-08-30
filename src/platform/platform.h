#pragma once
// =============================================================================
// Platform facade — single include for the whole PC platform layer.
//
// Game code and the Wii compatibility layer (src/compat) must include only
// this header (or the individual module headers) and must never depend on
// Win32/POSIX details. All platform-specific code (#ifdef _WIN32, POSIX,
// Win32 APIs...) lives in src/platform/linux/ and src/platform/windows/.
// =============================================================================

#include "platform/Log/Log.h"
#include "platform/Memory/Memory.h"
#include "platform/Endian/Endian.h"
#include "platform/Filesystem/Filesystem.h"
#include "platform/Timing/Timing.h"
#include "platform/Threading/Threading.h"
#include "platform/PlatformDetail.h"

namespace Platform {

// Initializes all platform subsystems (logging, memory, timing, filesystem).
// Safe to call once at process startup, before any game code runs.
// `logConfig` overrides the default logging configuration.
void init(const Log::Config& logConfig = {});

// Tears the platform subsystems down (flushes logs, frees arenas).
void shutdown();

} // namespace Platform
