// compat/vi — PC_PORT hook: the VI retrace heartbeat (M9.3).
#pragma once

// The full type comes from the SDK headers (GXStruct.h defines it; vi.h only
// forward-declares the _GXRenderModeObj tag), so include it here.
#include <revolution/gx/GXStruct.h>

namespace Platform::CompatVi {

// Called by compat/gx GXCopyDisp right after the frame is presented to the
// swapchain. Advances the VI retrace counter and runs the registered
// pre/post retrace callbacks (JUTVideo's preRetraceProc swaps the displayed
// XFB; postRetraceProc pings the JUTVideo message queue, which is exactly
// what MainLoopFramework::waitForRetrace blocks on).
void fireRetrace();

// Drains the host OS event queue (SDL). Called from VIWaitForRetrace once per
// field on the boot's main thread: the --boot path runs the vendored gameMain,
// which never returns to the native loop that would otherwise poll SDL — on
// Windows the window goes "not responding" without this. A close/quit request
// exits the process cleanly (atexit handlers stop the field clock). No-op when
// SDL's event subsystem is not initialized (headless unit tests).
void pumpHostEvents();

// -----------------------------------------------------------------------------
// PC_PORT (title widescreen): the host render mode.
//
// The EFB is created with the render mode's (fbWidth, efbHeight) — compat/gx
// GXCopyDisp -> GXSetDispCopySrc -> ensureEfb — and MainLoopFramework uses the
// same numbers for the clear, the J2D ortho and the copy. The game's render
// mode object therefore decides the host's actual framebuffer geometry.
//
// With the console's 640x456 mode the EFB was a 4:3 image that the present path
// stretched over the window: at 1280x720 that is a 1.184x horizontal stretch of
// EVERYTHING (sky, planet, logo, text) plus a resample. Keeping the render mode
// in sync with the window makes the present a 1:1 blit and lets the scene/UI
// layers work in the window's true aspect ratio.
// -----------------------------------------------------------------------------

/// The mutable render mode object the boot hands to JUTVideo/VIConfigure.
GXRenderModeObj* hostRenderMode();

/// Resizes the host framebuffer geometry (fbWidth/efbHeight/xfbHeight/viWidth).
/// Returns true when something changed. Safe to call every frame.
bool setHostFramebufferSize(unsigned width, unsigned height);

/// Reads Platform::Window's drawable size and calls setHostFramebufferSize()
/// when it changed. No-op without a window (headless tests) or before the
/// renderer exists.
bool syncHostRenderModeWithWindow();

// -----------------------------------------------------------------------------
// Field-clock lifecycle (PC_PORT).
// -----------------------------------------------------------------------------

/// Stops the emulated field clock and joins its thread. Idempotent and safe to
/// call from any thread (including the clock thread itself); also registered
/// with atexit by VIInit, so the exit path never leaves the clock running
/// while the rest of the process is torn down. Called by
/// compat::shutdownHostForExit() before the renderer/platform go away.
void shutdownFieldClock();

} // namespace Platform::CompatVi
