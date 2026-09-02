// compat/vi — PC_PORT hook: the VI retrace heartbeat (M9.3).
#pragma once

namespace Platform::CompatVi {

// Called by compat/gx GXCopyDisp right after the frame is presented to the
// swapchain. Advances the VI retrace counter and runs the registered
// pre/post retrace callbacks (JUTVideo's preRetraceProc swaps the displayed
// XFB; postRetraceProc pings the JUTVideo message queue, which is exactly
// what MainLoopFramework::waitForRetrace blocks on).
void fireRetrace();

} // namespace Platform::CompatVi
