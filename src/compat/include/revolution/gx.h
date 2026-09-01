#ifndef GX_H
#define GX_H

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Change vs. upstream: the sub-header includes use angle brackets
// (`#include <revolution/gx/...>`) instead of quotes. With quotes, the
// preprocessor first searches the directory of gx.h itself — where the
// VENDORED GXVert.h lives — and can pick the vendored GXVert.h over our
// override in src/compat/include (observed with MSVC). With angle brackets,
// the include path order applies (src/compat/include comes first), so our
// GXVert.h/GXRegs.h overrides always win. All other sub-headers have no
// override and resolve to the vendored ones, as before.
//
// Everything else is identical to upstream.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

#define GX_FIFO_ADDR 0xCC008000

#include <revolution/gx/GXBump.h>
#include <revolution/gx/GXCpu2Efb.h>
#include <revolution/gx/GXCull.h>
#include <revolution/gx/GXDispList.h>
#include <revolution/gx/GXEnum.h>
#include <revolution/gx/GXFifo.h>
#include <revolution/gx/GXFrameBuf.h>
#include <revolution/gx/GXGeometry.h>
#include <revolution/gx/GXGet.h>
#include <revolution/gx/GXInit.h>
#include <revolution/gx/GXLighting.h>
#include <revolution/gx/GXManage.h>
#include <revolution/gx/GXPE.h>
#include <revolution/gx/GXPerf.h>
#include <revolution/gx/GXPixel.h>
#include <revolution/gx/GXStruct.h>
#include <revolution/gx/GXTev.h>
#include <revolution/gx/GXTexture.h>
#include <revolution/gx/GXTransform.h>
#include <revolution/gx/GXTypes.h>
#include <revolution/types.h>

#ifdef __cplusplus
}
#endif

// GXVert.h is C++: its GXCompatFifo.h include defines C++ types (GXFifoPipe /
// GXFifoWord) whose functions must keep C++ linkage (they are implemented in
// compat/gx/GXCompat.cpp with C++ linkage). Including them inside the
// extern "C" block above would give the free fifoWrite* helpers C linkage and
// break the link (undefined _ZN... vs fifoWrite*). GXVert.h opens and closes
// its own extern "C" around the macro-generated GX* writers.
#include <revolution/gx/GXVert.h>

#endif  // GX_H
