// compat/vi — Video Interface (VI) emulation (M9.3).
//
// On the Wii the VI scans out the XFB to the TV and raises a retrace
// interrupt every field (60 Hz NTSC / 50 Hz PAL). The game's frame loop is
// paced by that interrupt: MainLoopFramework::waitForRetrace blocks on the
// JUTVideo message queue, which postRetraceProc feeds with VIGetRetraceCount
// at every retrace (MainLoopFramework.cpp / JUTVideo.cpp).
//
// On the host there is no separate scanout yet: the boot path (--boot) is
// headless, and the frame is presented by the renderer only through
// compat/gx GXCopyDisp. So the retrace clock is an emulated NTSC field clock
// (59.94 Hz) started by VIInit — the same observation the game makes: it
// renders one frame and blocks for the next retrace (waitForTick), which is
// exactly "vsync" from the game's point of view.
//
// When the windowed boot lands (M9.5, renderer + swapchain), the swapchain
// present can drive the clock directly: Platform::CompatVi::fireRetrace()
// (called from GXCopyDisp right after the present) advances the same state
// machine — the two sources may not be combined without reworking the
// cadence, so today the field clock is authoritative and fireRetrace() is the
// documented hook for the present-driven mode (it is deliberately NOT called
// while the clock thread runs; see the flag comment in VIInit).
#include "compat/vi/VICompat.h"

#include "platform/Log/Log.h"

#include <revolution/vi.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>

#if defined(_WIN32)
// PC_PORT (Windows): the default system timer granularity (15.6 ms) rounds
// the 16.68 ms field sleep up to ~31 ms — the field clock would run at half
// speed and VIWaitForRetrace's 30 ms cap times out just before the next
// field (vi_wait_for_retrace_blocks_about_one_field saw <5 fields). Request
// 1 ms resolution (process-wide, standard game practice). Declared directly
// to avoid pulling <windows.h> into this TU; winmm is linked via the pragma
// below (MSVC) and via CMake (MinGW).
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
#pragma comment(lib, "winmm.lib")
#endif

namespace {

std::atomic<u32> sRetraceCount{0};
std::mutex sRetraceMutex;
std::condition_variable sRetraceCv;

VIRetraceCallback sPreRetraceCallback = nullptr;
VIRetraceCallback sPostRetraceCallback = nullptr;
GXRenderModeObj* sRenderMode = nullptr;
bool sHasRenderMode = false;
BOOL sBlack = FALSE;
BOOL sDimming = FALSE;
u32 sDimmingCount = 0;
void* sNextFrameBuffer = nullptr;
void* sCurrentFrameBuffer = nullptr;

// The NTSC field clock (59.94 Hz). 16 683 360 ns per field.
constexpr std::chrono::nanoseconds kFieldPeriod(16683360);

std::thread sFieldClockThread;
std::atomic<bool> sClockRunning{false};

// Advances one field: bump the retrace counter, swap the display pointer and
// run the pre/post retrace callbacks (post pings the JUTVideo message queue).
void tickField() {
    u32 count = sRetraceCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    sCurrentFrameBuffer = sNextFrameBuffer;

    VIRetraceCallback pre = sPreRetraceCallback;
    VIRetraceCallback post = sPostRetraceCallback;

    if (sBlack && (count & 0x3F) == 0) {
        PL_LOG_TRACE("vi", "retrace %u (black)", count);
    }

    if (pre) {
        pre(count);
    }
    if (post) {
        post(count);
    }
    if (sDimming) {
        ++sDimmingCount;
    }
    sRetraceCv.notify_all();
}

// Stops the field clock at process exit. Registered once by VIInit.
void shutdownFieldClock() {
    sClockRunning.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(sRetraceMutex);
        sRetraceCv.notify_all();
    }
    if (sFieldClockThread.joinable()) {
        sFieldClockThread.join();
    }
}

// Sleeps the field period; busy-sleep correction keeps the cadence exact
// when the callback work is short (it is: two ~microsecond callbacks).
void fieldClockMain() {
    auto next = std::chrono::steady_clock::now();
    while (sClockRunning.load(std::memory_order_acquire)) {
        next += kFieldPeriod;
        {
            std::unique_lock<std::mutex> lock(sRetraceMutex);
            sRetraceCv.wait_until(lock, next);
        }
        if (!sClockRunning.load(std::memory_order_acquire)) {
            break;
        }
        tickField();
        // Drift control: catch up / skip when a callback overran.
        auto now = std::chrono::steady_clock::now();
        if (now > next) {
            const auto over = now - next;
            next = now - (over % kFieldPeriod);
        }
    }
}

} // namespace

extern "C" {

void VIInit(void) {
    PL_LOG_INFO("vi", "VIInit: host video interface (59.94 Hz field clock, "
                      "present-driven mode hooks in GXCopyDisp)");
    sRetraceCount.store(0);
    sPreRetraceCallback = nullptr;
    sPostRetraceCallback = nullptr;
    sRenderMode = nullptr;
    sHasRenderMode = false;
    sBlack = FALSE;
    sDimming = FALSE;
    sDimmingCount = 0;
    sNextFrameBuffer = nullptr;
    sCurrentFrameBuffer = nullptr;

    // Field clock: the virtual scanout. Started once (VIInit is called once
    // by the boot; JUTVideo::createManager also VIInits on re-creation).
    if (sClockRunning.load(std::memory_order_acquire)) {
        return;
    }
#if defined(_WIN32)
    // Once per process: raise the OS timer resolution so the condvar sleeps
    // below can hit the 16.68 ms field cadence (see the declaration note).
    timeBeginPeriod(1);
#endif
    sClockRunning.store(true, std::memory_order_release);
    sFieldClockThread = std::thread(fieldClockMain);
    // The clock thread is joined at process exit from the atexit handler
    // below; keeping it joinable lets the suite binary terminate cleanly.
    std::atexit(shutdownFieldClock);
}

void VIFlush(void) {
    // The VI register writes mirror into host state; nothing to push.
}

void VIWaitForRetrace(void) {
    // PC_PORT: pump the host event queue once per field.
    //
    // The native path polls SDL in Platform::Input::poll from main.cpp's demo
    // loop, but --boot enters gameMain(), which never returns — nothing else
    // drains the OS event queue. On Windows an unpumped queue makes the window
    // go "not responding" after a few seconds even though the frame loop is
    // healthy (the user sees exactly that at ~10 s into the Logo scene). The
    // retrace wait runs on the boot's main thread once per frame, which is the
    // one place that is both per-frame and on the window's thread.
    Platform::CompatVi::pumpHostEvents();

    // Block until the retrace counter advances (one field = ~16.7 ms). The
    // 30 ms cap only matters if the clock is somehow not running.
    u32 before = sRetraceCount.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lock(sRetraceMutex);
    sRetraceCv.wait_for(lock, std::chrono::milliseconds(30),
                        [&] { return sRetraceCount.load(std::memory_order_acquire) != before; });
}

void VIConfigure(const GXRenderModeObj* rmode) {
    if (!rmode) {
        PL_LOG_WARN("vi", "VIConfigure(nullptr)");
        return;
    }
    sRenderMode = const_cast<GXRenderModeObj*>(rmode);
    sHasRenderMode = true;
    PL_LOG_INFO("vi", "VIConfigure: tvmode 0x%x, %ux%u (efb %u, xfb %u)%s",
                static_cast<unsigned>(rmode->viTVmode),
                static_cast<unsigned>(rmode->fbWidth),
                static_cast<unsigned>(rmode->efbHeight),
                static_cast<unsigned>(rmode->xfbHeight),
                static_cast<unsigned>(rmode->xfbHeight),
                rmode->aa ? ", AA" : "");
}

void VIConfigurePan(u16 xfbWidth, u16 xfbHeight, u16 panX, u16 panY) {
    (void)xfbWidth;
    (void)xfbHeight;
    (void)panX;
    (void)panY; // No pan on the host (the swapchain scales the whole frame).
}

void VISetNextFrameBuffer(void* fb) {
    sNextFrameBuffer = fb;
}

void* VIGetNextFrameBuffer(void) {
    return sNextFrameBuffer;
}

void* VIGetCurrentFrameBuffer(void) {
    return sCurrentFrameBuffer;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback prev = sPreRetraceCallback;
    sPreRetraceCallback = cb;
    return prev;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback prev = sPostRetraceCallback;
    sPostRetraceCallback = cb;
    return prev;
}

void VISetBlack(BOOL black) {
    sBlack = black;
}

u32 VIGetRetraceCount(void) {
    return sRetraceCount.load(std::memory_order_acquire);
}

u32 VIGetCurrentLine(void) {
    // Not meaningful off-console; report a quiescent value (the game uses
    // this only for diagnostics).
    return 0;
}

u32 VIGetTvFormat(void) {
    // Host display treated as NTSC-rate 60 Hz (the field clock cadence; the
    // game logic only branches 50 vs 60).
    return VI_NTSC;
}

u32 VIGetScanMode(void) {
    // Progressive (non-interlaced) — matches the host scanout and makes
    // MR::getSuitableRenderMode pick the progressive tables.
    return VI_NON_INTERLACE;
}

u32 VIGetDTVStatus(void) {
    // DTV connected: getSuitableRenderMode() returns the progressive modes.
    return 1;
}

BOOL VIEnableDimming(BOOL enable) {
    BOOL prev = sDimming;
    sDimming = enable;
    return prev;
}

u32 VIGetDimmingCount(void) {
    return sDimmingCount;
}

BOOL VIResetDimmingCount(void) {
    sDimmingCount = 0;
    return TRUE;
}

void VISetTrapFilter(VIBool filter) {
    (void)filter; // No VI "screen trap" on the host.
}

} // extern "C"

namespace Platform::CompatVi {

void fireRetrace() {
    // Present-driven retrace (the future windowed/swapchain mode — M9.5).
    // While the field clock runs this would double the cadence, so it is
    // documented as the hook and not called from GXCopyDisp today.
    tickField();
}

void pumpHostEvents() {
    // The headless unit tests call VIWaitForRetrace without ever initializing
    // SDL, so skip when the event subsystem is not up.
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0) {
        return;
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            // gameMain never returns, so the window close button is the only
            // way out of a boot run. Exit through the normal path so the
            // atexit handlers stop the field clock and retire the renderer.
            PL_LOG_INFO("vi", "close requested — exiting boot");
            std::exit(0);
        }
    }
}

} // namespace Platform::CompatVi
