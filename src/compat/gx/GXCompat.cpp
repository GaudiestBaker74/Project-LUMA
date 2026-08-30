// =============================================================================
// compat/gx — M3 GX subset routed to Platform::Video (see GXCompat.h).
// =============================================================================

#include "compat/gx/GXCompat.h"

#include "platform/Log/Log.h"
#include "platform/Renderer/Renderer.h"

extern "C" {

void GXInit(void* fifoPtr, u32 fifoSize) {
    // M4: the Vulkan state is owned by Platform::Renderer (initialized by
    // main.cpp). The FIFO pointer from the game boot is intentionally unused.
    PL_LOG_TRACE("gx", "GXInit(%p, %u) — no-op (renderer already initialized)",
                 fifoPtr, static_cast<unsigned>(fifoSize));
}

void GXSetViewport(f32 x, f32 y, f32 w, f32 h, f32 nearZ, f32 farZ) {
    (void)nearZ;
    (void)farZ;
    PL_LOG_TRACE("gx", "GXSetViewport(%.0f, %.0f, %.0f, %.0f)", x, y, w, h);
    if (Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().setViewport(x, y, w, h);
    }
}

void GXClearColor(GXColor color) {
    PL_LOG_TRACE("gx", "GXClearColor(%u, %u, %u, %u)", color.r, color.g, color.b, color.a);
    if (Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().setClearColor(
            color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
    }
}

void GXClear(u32 clrMask) {
    // M4: the pass is always cleared with the current color (the renderer's
    // no-arg beginPass() uses the color stored by GXClearColor); the mask
    // only gates whether we ask for a clear. Depth clearing arrives with the
    // depth buffer (M4.2).
    PL_LOG_TRACE("gx", "GXClear(0x%x)", static_cast<unsigned>(clrMask));
    (void)clrMask;
}

} // extern "C"
