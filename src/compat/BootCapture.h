#pragma once
// =============================================================================
// compat/BootCapture — frame dump for --boot runs (PC_PORT).
//
// The native loop's --screenshot hook lives in main.cpp, but --boot runs the
// vendored gameMain() frame loop and never returns to it, so there was no way
// to capture what the real title screen looks like. This module adds the
// equivalent for boot runs: at present #N the EFB is read back (after a frame
// flush, exactly like the #300 EFB probe) and written as a PPM/P6 file, and
// the process then exits cleanly through compat::shutdownHostForExit().
//
// Wired from main.cpp:
//   galaxy-pc --boot --screenshot title.ppm           -> dump present #1800, exit
//   galaxy-pc --boot --frames 900 --screenshot e.ppm  -> dump present #900, exit
//   galaxy-pc --boot --frames 900                     -> no dump, exit after #900
//   F12 during a boot run                             -> dump the next present
//                                                        (luma-frame-NNNNN.ppm
//                                                        when no path was given)
//   Esc during a boot run                             -> clean shutdown
//
// PPM is deliberate: no image library is linked, and P6 is a 3-line header
// plus raw RGB, so the file converts anywhere (and the test suite reads the
// same path back).
// =============================================================================

#include <revolution/types.h>

namespace compat {

/// Default dump frame when --screenshot is given without --frames: 30 s of
/// fields at 59.94 Hz. The logo/intermission scene runs for ~10-20 s of a
/// boot, so this lands inside the title screen proper.
constexpr u32 kBootCaptureDefaultFrame = 1800;

/// Arms the boot capture. `pPath` may be null/empty (then only the
/// exit-after-N behaviour is armed). Call once, before gameMain(); the string
/// must outlive the process (main.cpp passes its own storage).
void setBootCapture(const char* pPath, u32 frame, bool exitAfter);

/// Asks for a dump of the next presented frame (the F12 hotkey). Works with or
/// without a previously armed capture; without an explicit path the file is
/// named luma-frame-<present>.ppm in the current directory.
void requestFrameDump();

/// True when a dump and/or an exit-after-N was requested.
bool hasBootCapture();

/// Reads the current EFB back and writes it as a binary PPM (P6). Shared by
/// main.cpp's --screenshot (one implementation). Returns false — and logs why
/// — when there is no EFB, the readback fails or the file cannot be written.
bool writeEfbPpm(const char* path);

/// Present-path hook, called by GXCopyDisp once per present with the 1-based
/// present index. `efbPresented` is false when that frame had no EFB pass to
/// blit (e.g. the swapchain pass): such a frame is skipped and the capture
/// waits for the next real one.
void notifyBootPresent(u32 presentIndex, bool efbPresented);

} // namespace compat
