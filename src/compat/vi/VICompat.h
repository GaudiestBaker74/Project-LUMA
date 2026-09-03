// compat/vi — PC_PORT hook: the VI retrace heartbeat (M9.3).
#pragma once

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

} // namespace Platform::CompatVi
