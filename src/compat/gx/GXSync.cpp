// compat/gx — GX command-pipeline synchronization (M9.3).
//
// The Flipper's command processor (CP) runs the display-list/fifo stream
// asynchronously from the CPU. The GX sync APIs in this file let game code:
//   - flush the write-gather fifo to the CP          (GXFlush/GXPixModeSync)
//   - wait until the CP is idle                      (GXDrawDone)
//   - arm a token/callback when the CP passes a tag  (GXSetDrawSync(+Cb))
//   - arm a callback when the CP drains the fifo     (GXSetDrawDone(+Cb))
//   - read fifo state (GXGetCPUFifo/GXGetFifoPtrs) and performance counters.
//
// On the host the "CP" is the emulated pipeline inside compat/gx: state
// changes apply when GXBegin starts a primitive (the vertex builders in
// GXVert.h capture the stream), and the EFB is copied to the swapchain in
// GXCopyDisp. There is no asynchronous hardware processing, so every "wait"
// is satisfied immediately and every callback would fire instantly — but
// firing them eagerly changes the game's visible behavior (e.g.
// JUTVideo::drawDoneCallback flips the single-buffer draw flag), so the
// callbacks are *registered truthfully* and left unfired, with a TODO for
// the EFB-copy path that could legitimately fire the draw-done callback.
//
// The one true "sync" in the port is the VI retrace (compat/vi), which paces
// the frame loop; GX sync never blocks.
#include "compat/gx/GXCompat.h"

#include <revolution/gx/GXFrameBuf.h>
#include <revolution/gx/GXManage.h>
#include <revolution/gx/GXPerf.h>
#include <revolution/gx/GXTexture.h>
#include <revolution/gx/GXRegs.h>

#include "platform/Log/Log.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Host definitions of the register globals that GXRegs.h declares extern.
// ---------------------------------------------------------------------------
extern "C" {

// The hardware write-gather fifo register (0xCC008000). compat/gx emulates it
// with the C++ GXWGFifo object (GXCompatFifo.h); this C-linkage global only
// exists so the vendored GX_WRITE_* macros (used in MainLoopFramework's GX
// abort alarm) link. The alarm path is never taken on the host (the watchdog
// is cancelled after GXDrawDone), but the writes would be harmless anyway.
volatile PPCWGPipe gxfifo;

// PI/CP/PE/MEM register block bases — not emulated (compat/gx mirrors the
// registers it needs); null so any stray read crashes loudly instead of
// touching nonsense memory.
volatile void* __piReg = nullptr;
volatile void* __cpReg = nullptr;
volatile void* __peReg = nullptr;
volatile void* __memReg = nullptr;

} // extern "C"

namespace {

// Currently registered sync callbacks (never fired on the host today — see
// the file comment; the DrawSyncManager/JUTVideo paths that arm them run
// their own logic instead).
GXDrawSyncCallback sDrawSyncCallback = nullptr;
GXDrawDoneCallback sDrawDoneCallback = nullptr;

// The CPU fifo range last passed to GXInit (GameSystem::initGX allocates the
// 0x80000-byte command fifo); GXGetCPUFifo/GXGetFifoPtrs report it back.
void* sCpuFifoBase = nullptr;
u32 sCpuFifoSize = 0;

// Mirrored state: line width (J2DGrafContext sets it) and Z-texture mode
// (MainLoopFramework::clearEfb toggles GX_ZT_REPLACE/GX_ZT_DISABLE).
u8 sLineWidth = 6;
GXTexOffset sLineTexOffset = GX_TO_ZERO;
GXZTexOp sZTexOp = GX_ZT_DISABLE;
GXTexFmt sZTexFmt = GX_TF_Z24X8;

} // namespace

namespace Platform {
namespace CompatGx {

// Hook called by GXInit (GXCompat.cpp) so GXGetCPUFifo/GXGetFifoPtrs can
// report the game's command fifo.
void setCpuFifoRange(void* base, u32 size) {
    sCpuFifoBase = base;
    sCpuFifoSize = size;
}

} // namespace CompatGx
} // namespace Platform

extern "C" {

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb) {
    PL_LOG_TRACE("gx", "GXSetDrawSyncCallback(%p)", reinterpret_cast<void*>(cb));
    GXDrawSyncCallback prev = sDrawSyncCallback;
    sDrawSyncCallback = cb;
    // Never fired on the host: the emulated CP is synchronous (see the file
    // comment). TODO(PC_PORT, M10): the DrawSyncManager could fire the token
    // at the swapchain blit (the host equivalent of the CP passing the tag).
    return prev;
}

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb) {
    PL_LOG_TRACE("gx", "GXSetDrawDoneCallback(%p)", reinterpret_cast<void*>(cb));
    GXDrawDoneCallback prev = sDrawDoneCallback;
    sDrawDoneCallback = cb;
    // JUTVideo registers drawDoneCallback here; on the host the "draw done"
    // moment is the swapchain blit in GXCopyDisp. TODO(PC_PORT): fire it
    // there for the single-buffer path (mBufferNum == 1).
    return prev;
}

void GXFlush(void) {
    PL_LOG_TRACE("gx", "GXFlush()");
    // The CPU-side state is applied when GXBegin starts a primitive; the GPU
    // work is submitted at the pass boundaries inside the renderer. Nothing
    // to flush and nothing to wait for.
}

void GXPixModeSync(void) {
    PL_LOG_TRACE("gx", "GXPixModeSync()");
    // Pixel-engine mode registers apply immediately in the emulated TEV/PE
    // evaluator (no pipelined latency).
}

void GXDrawDone(void) {
    PL_LOG_TRACE("gx", "GXDrawDone()");
    // The emulated pipeline is synchronous: all submitted work is complete
    // when this returns.
}

void GXAbortFrame(void) {
    PL_LOG_TRACE("gx", "GXAbortFrame()");
    // Host frames are not abortable piecemeal; a pending pass simply renders
    // (the abort is only used by the never-taken GX watchdog path).
}

void GXSetDrawSync(u16 token) {
    PL_LOG_TRACE("gx", "GXSetDrawSync(%u)", static_cast<unsigned>(token));
    // No fifo tokens on the host (the token callback would fire instantly —
    // deliberately not fired, see the file comment).
    (void)token;
}

void GXSetDrawDone(void) {
    PL_LOG_TRACE("gx", "GXSetDrawDone()");
    // Same reasoning as GXSetDrawSync: arming the callback is recorded, it is
    // not fired (JUTVideo::drawDoneStart uses this for the double-buffer xfb
    // path, which the port does not run).
}

GXBool GXGetCPUFifo(GXFifoObj* fifo) {
    PL_LOG_TRACE("gx", "GXGetCPUFifo(%p)", reinterpret_cast<void*>(fifo));
    if (!fifo) {
        return GX_FALSE;
    }
    std::memset(fifo, 0, sizeof(*fifo));
    fifo->base = static_cast<u8*>(sCpuFifoBase);
    fifo->top = static_cast<u8*>(sCpuFifoBase) + (sCpuFifoSize ? sCpuFifoSize : 1);
    fifo->size = sCpuFifoSize;
    fifo->rdPtr = sCpuFifoBase;
    fifo->wrPtr = sCpuFifoBase;
    fifo->count = sCpuFifoSize ? static_cast<s32>(sCpuFifoSize) : 0;
    fifo->bind_cpu = GX_TRUE;
    return GX_TRUE;
}

void GXGetFifoPtrs(const GXFifoObj* fifo, void** readPtr, void** writePtr) {
    PL_LOG_TRACE("gx", "GXGetFifoPtrs(%p)", reinterpret_cast<const void*>(fifo));
    if (readPtr) {
        *readPtr = fifo ? fifo->rdPtr : sCpuFifoBase;
    }
    if (writePtr) {
        *writePtr = fifo ? fifo->wrPtr : sCpuFifoBase;
    }
}

void GXDisableBreakPt(void) {
    PL_LOG_TRACE("gx", "GXDisableBreakPt()");
    // Breakpoints are a CP debug feature; no-op on the host.
}

void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle,
                   GXBool* cmdIdle, GXBool* brkpt) {
    PL_LOG_TRACE("gx", "GXGetGPStatus()");
    // The emulated GP is never busy: fifo empty, no breakpoint.
    if (overhi) {
        *overhi = GX_FALSE;
    }
    if (underlow) {
        *underlow = GX_FALSE;
    }
    if (readIdle) {
        *readIdle = GX_TRUE;
    }
    if (cmdIdle) {
        *cmdIdle = GX_TRUE;
    }
    if (brkpt) {
        *brkpt = GX_FALSE;
    }
}

void GXReadXfRasMetric(u32* xfWaitIn, u32* xfWaitOut, u32* rasBusy, u32* clocks) {
    // Performance counters of the XF/RAS units — no counters on the host;
    // report quiescent. (MainLoopFramework's GX watchdog reads these.)
    if (xfWaitIn) {
        *xfWaitIn = 0;
    }
    if (xfWaitOut) {
        *xfWaitOut = 0;
    }
    if (rasBusy) {
        *rasBusy = 0;
    }
    if (clocks) {
        *clocks = 0;
    }
}

void GXInvalidateVtxCache(void) {
    PL_LOG_TRACE("gx", "GXInvalidateVtxCache()");
    // The SW vertex cache is rebuilt with every state change (no persistent
    // cache to invalidate).
}

void GXInvalidateTexAll(void) {
    PL_LOG_TRACE("gx", "GXInvalidateTexAll()");
    // Drop the texture-object state so the next GXLoadTexObj re-uploads
    // (matches the GameSystem doing this at preGX each frame).
    Platform::CompatGx::resetTextureState();
}

void GXSetLineWidth(u8 width, GXTexOffset texOffset) {
    PL_LOG_TRACE("gx", "GXSetLineWidth(%u, %d)", static_cast<unsigned>(width),
                 static_cast<int>(texOffset));
    // Mirrored: host line rendering uses the rasterizer's own width (1 px);
    // the D3D/J2D ortho lines are 1 px regardless.
    sLineWidth = width;
    sLineTexOffset = texOffset;
}

void GXSetZTexture(GXZTexOp op, GXTexFmt fmt, u32 bias) {
    PL_LOG_TRACE("gx", "GXSetZTexture(%d, %d, %u)", static_cast<int>(op),
                 static_cast<int>(fmt), static_cast<unsigned>(bias));
    // Mirrored. MainLoopFramework::clearEfb uses this to write the Z texture
    // at the depth pass; the emulated pipeline writes the clear color into
    // the depth test through GXSetZMode/GXSetZCompLoc instead.
    (void)bias;
    sZTexOp = op;
    sZTexFmt = fmt;
}

} // extern "C"
