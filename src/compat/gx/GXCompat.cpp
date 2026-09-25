// =============================================================================
// compat/gx — GX state mirror + immediate-vertex path (M5.1). See GXCompat.h.
//
// Model: the GX configuration calls mirror the console state (GX_VCD /
// GX_VAT / viewport / scissor / projection / cull / blend / clear). The
// immediate-vertex writers push raw values into the simulated write-gather
// pipe (GXCompatFifo.h -> GXWGFifo), and this file reconstructs vertices
// from that write stream using the current vertex descriptor — exactly like
// the PPC vertex processor. Completed primitives are submitted to
// Platform::Renderer.
//
// M5.1 simplifications (documented in GXCompat.h):
//   * the vertex stream is serialized as floats (CPU conversion);
//   * the vertex shader consumes position + color0 (normals/texcoords are
//     captured into the stream but not rendered yet);
//   * GX_QUADS is decomposed into two triangles on the CPU;
//   * the write order is assumed to follow the VCD attribute order (the
//     console-standard pos -> nrm -> clr -> tex sequence; the game always
//     writes in this order in practice).
// =============================================================================

#include "compat/gx/GXCompat.h"

#include <chrono>

// File Select is bound by this TU (indexed planet strips, ~44k verts/frame).
// Force this math optimized even under a Debug build: same XF formula, no
// fast-math, just fast enough to finish inside one retrace. Clang uses the
// CMake COMPILE_OPTIONS. MSVC must not get /O2 on the command line — Debug's
// /RTC1 rejects it (D8016) — so the optimize pragma is used instead.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")
#elif defined(_MSC_VER)
#pragma optimize("gt", on)
#endif

#if defined(_MSC_VER)
#define LUMA_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define LUMA_NOINLINE __attribute__((noinline))
#else
#define LUMA_NOINLINE
#endif

#include <cstdio>
#include "compat/gx/GXCompatFifo.h"

#include "platform/Log/Log.h"
#include "platform/Renderer/Renderer.h"
#include "platform/Renderer/vk_demo_shaders.h"

// M5.6: CP register bit layouts (VCD/VAT decode) and GX_PHY_ADDR (the array
// physical base baked into display lists).
#include <private/cp_reg.h>
#include <revolution/gx/GXTypes.h>

#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

// The simulated write-gather pipe (declared in GXCompatFifo.h).
GXFifoPipe GXWGFifo;

namespace {

using namespace Platform;

// --- GX state mirror ---------------------------------------------------------

// Vertex descriptor: attribute -> data source. Indexed by GX_VA_*.
GXAttrType sVtxDesc[GX_VA_MAX_ATTR] = {};
bool sVtxDescSet[GX_VA_MAX_ATTR] = {};

// Attribute format per vertex format (GX_VTXFMT0..7) and attribute.
struct AttrFmt {
    GXCompCnt cnt = GX_POS_XYZ;
    GXCompType type = GX_F32;
    u8 frac = 0;
    bool set = false;
};
AttrFmt sAttrFmt[8][GX_VA_MAX_ATTR];

// Projection (MVP for the GX vertex shader; GX matrices are clip-space ready).
f32 sProjection[16];
bool sHasProjection = false;

// Viewport/scissor/clear (M3 hooks now with real state).
f32 sViewportX = 0, sViewportY = 0, sViewportW = 0, sViewportH = 0;
f32 sViewportNearZ = 0.0f, sViewportFarZ = 1.0f; // GX viewport depth range (M9.5.3c)
u32 sScissor[4] = {0, 0, 0, 0};
// GXSetScissor may arrive before the host frame is recording; the mirror is
// then re-applied by flushDraw (Renderer drops dynamic-state calls outside a
// recording command buffer). sScissorSet keeps the {0,0,0,0} default from
// clipping everything to nothing.
bool sScissorSet = false;
f32 sClearColor[4] = {0.1f, 0.1f, 0.15f, 1.0f};

// --- pixel-engine state mirror (M5.5) ----------------------------------------
// Console defaults (from the vendored GXInit.c, see docs/gx.md §7): cull BACK,
// blend NONE(SRCALPHA, INVSRCALPHA, CLEAR), color+alpha update ENABLE,
// z TRUE/LEQUAL/TRUE, ZCompLoc TRUE, dst alpha DISABLE, dither ENABLE,
// pixel format RGB8_Z24. Note: sCullMode starts at GX_CULL_NONE here (M5.1)
// until GXInit() applies the console reset below — the renderer's own default
// (CullMode::None) matches the pre-init state.
GXCullMode sCullMode = GX_CULL_NONE;
GXBlendMode sBlendMode = GX_BM_NONE;
GXBlendFactor sBlendSrc = GX_BL_SRCALPHA;
GXBlendFactor sBlendDst = GX_BL_INVSRCALPHA;
GXLogicOp sBlendLogicOp = GX_LO_CLEAR;
bool sZTest = true;
bool sZWrite = true;
GXCompare sZFunc = GX_LEQUAL;
bool sZCompLoc = true;
bool sColorUpdate = true;
bool sAlphaUpdate = true;
bool sDstAlphaEnable = false;
u8 sDstAlphaValue = 0;
bool sDither = true;
GXPixelFmt sPixelFmt = GX_PF_RGB8_Z24;
GXZFmt16 sZFormat = GX_ZC_LINEAR;

// --- GX -> Platform enum mappings (M5.5) --------------------------------------
// The Platform enums are Vulkan-semantic; the GX values map onto them here.

// PC_PORT (M9.5.4 v8): GX front faces are CLOCKWISE in screen space — the
// opposite of the platform renderer's Vulkan default (front = counter-
// clockwise, Renderer.cpp). Evidence: the SDK's own MainLoopFramework::clearEfb
// quad (top-left → top-right → bottom-right → bottom-left = clockwise) is drawn
// under GX_CULL_BACK on the console; libogc's gx.h documents "clockwise to the
// viewer = front-facing"; Dolphin's Vulkan backend uses VK_FRONT_FACE_CLOCKWISE
// with the same NDC Y negation flushDraw applies. So GX "back" is the host's
// "front" and vice versa. Before this swap every J3D material with the usual
// GX_CULL_BACK rendered inside-out (the title sky dome disappeared). Pinned by
// gx_cull_front_face_is_clockwise (gx_copy_test.cpp).
CullMode cullModeFromGx(GXCullMode m) {
    switch (m) {
        case GX_CULL_NONE:  return CullMode::None;
        case GX_CULL_FRONT: return CullMode::Back;   // GX front (CW) = host back
        case GX_CULL_BACK:  return CullMode::Front;  // GX back (CCW) = host front
        case GX_CULL_ALL:   return CullMode::FrontAndBack;
    }
    return CullMode::None;
}

CompareOp compareFromGx(GXCompare c) {
    return static_cast<CompareOp>(c);  // GX_NEVER..GX_ALWAYS is 1:1
}

// GX blend factors apply to the whole RGBA. GX_BL_SRCCLR means "the fragment's
// output color" on either slot (the renderer maps it to SRC_COLOR).
BlendFactor blendFactorFromGx(GXBlendFactor f) {
    switch (f) {
        case GX_BL_ZERO:        return BlendFactor::Zero;
        case GX_BL_ONE:         return BlendFactor::One;
        case GX_BL_SRCCLR:      return BlendFactor::SrcColor;
        case GX_BL_INVSRCCLR:   return BlendFactor::OneMinusSrcColor;
        case GX_BL_SRCALPHA:    return BlendFactor::SrcAlpha;
        case GX_BL_INVSRCALPHA: return BlendFactor::OneMinusSrcAlpha;
        case GX_BL_DSTALPHA:    return BlendFactor::DstAlpha;
        case GX_BL_INVDSTALPHA: return BlendFactor::OneMinusDstAlpha;
    }
    return BlendFactor::One;
}

// --- current primitive -------------------------------------------------------

bool sInBegin = false;
GXPrimitive sPrimitive = GX_QUADS;
GXVtxFmt sVtxFmt = GX_VTXFMT0;
u16 sNverts = 0;
u16 sNvertsDone = 0;

// VCD: the ordered list of enabled attributes (VCD attribute order: POS, NRM,
// CLR0, CLR1, TEX0..TEX7) with their resolved component counts (VAT) and data
// source.
struct VcdSlot {
    int attr;        // GX_VA_*
    int comps;       // resolved component count (from the VAT)
    GXAttrType source; // GX_DIRECT / GX_INDEX8 / GX_INDEX16
    // M9.10 perf: the VAT fields the per-write path needs, cached here at
    // rebuild time — the vertex hot path used to re-resolve them (switch over
    // the VAT table) on every FIFO word.
    GXCompType type; // VAT component type (array reads / fixed-point scale)
    u8 frac;         // VAT fraction (fixed-point scale = 1 << frac)
};
std::vector<VcdSlot> sVcdOrder;
int sVcdTotalWrites = 0; // FIFO words per vertex (DIRECT: comps, indexed: 1)

// M9.10 perf: the FIFO stream decode table. rebuildVcd expands the VCD layout
// into one entry per FIFO word (a direct slot contributes one entry per
// component; an indexed slot contributes one), so captureWrite is a single
// indexed lookup instead of a linear scan of sVcdOrder per word (the scan ran
// ~450k times/frame on the fileselect planets).
struct WriteStep {
    std::int16_t slot; // index into sVcdOrder
    std::int16_t comp; // component within the slot (direct writes)
};
std::vector<WriteStep> sWriteSteps;

// Attribute arrays for INDEX8/INDEX16 (M5.2, GXSetArray) plus the matrix
// arrays GX_POS_MTX_ARRAY..GX_LIGHT_ARRAY (attrs 21..24, CPArray 12..15 /
// XF_A..XF_D) that LOADINDX reads in the PCPU/NCPU matrix pipelines.
constexpr int kMaxArrayAttr = GX_LIGHT_ARRAY + 1;  // 25
struct ArraySlot {
    const u8* base = nullptr;
    u8 stride = 0;
    bool set = false;
};
ArraySlot sArrays[kMaxArrayAttr];

// Per-attribute accumulators for the vertex being built (component values as
// floats in 0..1 for colors, raw for positions/normals).
float sCurAttr[GX_VA_MAX_ATTR][4] = {};
int sVtxWriteIndex = 0; // global write counter within the current vertex

// Serialized vertex stream of the current primitive (floats, VCD order).
std::vector<float> sVertexData;

// The texcoords the texgen units produced for the vertex — 8 (s,t) pairs plus
// their 8 projective components (q), in lockstep with sVertexData. Only the
// attributes the GX VCD names are serialized into sVertexData (the sky dome
// stores TEX0 alone), yet the hardware generates all eight coordinates for
// every vertex from what the material configures (POS/TEX0) and the TEV stages
// sample whichever they name. Carrying them here is what lets the fragment
// stage read tc1/tc2 and finish the projective divide per pixel.
std::vector<float> sVertexTexCoords;
std::vector<float> sVertexTexQ;

// M9.5.9 diagnostics: per-draw texcoord span. The reference frames and the port
// disagree on how much texture the sea shows, and the only honest way to tell
// whether a texgen maps one repeat or tens of repeats across the geometry is to
// measure the resolved coordinates. Enabled with LUMA_GX_UV_LOG=1.
float sTcMin[8][2];
float sTcMax[8][2];
float sQMin[8], sQMax[8];
bool sTcSeen[8];
float sPosMin[3] = {0, 0, 0}, sPosMax[3] = {0, 0, 0};
int sTcSpanNverts = 0;

void resetTcSpan() {
    for (int i = 0; i < 8; ++i) {
        sTcSeen[i] = false;
    }
    sTcSpanNverts = 0;
}

// M9.9 perf: the tc-span diagnostics are opt-in (LUMA_GX_UV_LOG=1). finishVertex
// used to pay the per-vertex position min/max and eight projective divides even
// when nobody read the numbers — on the fileselect field (~200k vertices/frame)
// that was a measurable slice of the frame.
bool tcSpanLogEnabled() {
    static const bool enabled = (std::getenv("LUMA_GX_UV_LOG") != nullptr);
    return enabled;
}

void logTcSpan() {
    if (!tcSpanLogEnabled()) {
        return;
    }
    static const auto t0 = std::chrono::steady_clock::now();
    static int emitted = 0;
    if (emitted >= 24) {
        return;
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (elapsed < 2.0) {
        return;
    }
    // Only the big environment shells are interesting (the title's sky dome and
    // planet cylinder); the logo/UI quads are small and constant-texcoord.
    float maxAbs = 0.0f;
    for (int c = 0; c < 3; ++c) {
        const float a = sPosMin[c] < 0.0f ? -sPosMin[c] : sPosMin[c];
        const float b = sPosMax[c] < 0.0f ? -sPosMax[c] : sPosMax[c];
        maxAbs = a > maxAbs ? a : maxAbs;
        maxAbs = b > maxAbs ? b : maxAbs;
    }
    // Only the projective (projmap) generators are interesting: q != 1 in those
    // draws identifies the planet shells, while the comet/halo shells use plain
    // texcoords and would burn the line budget.
    if ((sQMax[0] < 2.0f && sQMax[2] < 2.0f) || maxAbs < 100000.0f) {
        return;
    }
    if (!sTcSeen[0] && !sTcSeen[1] && !sTcSeen[2]) {
        return;
    }
    ++emitted;
    PL_LOG_INFO("gx", "draw %d: verts=%d pos x[%.0f..%.0f] y[%.0f..%.0f] z[%.0f..%.0f]", emitted,
                sTcSpanNverts, static_cast<double>(sPosMin[0]), static_cast<double>(sPosMax[0]),
                static_cast<double>(sPosMin[1]), static_cast<double>(sPosMax[1]),
                static_cast<double>(sPosMin[2]), static_cast<double>(sPosMax[2]));
    for (int i = 0; i < 3; ++i) {
        if (!sTcSeen[i]) {
            continue;
        }
        PL_LOG_INFO("gx", "  UV=tc%d/q  u[%.2f..%.2f] v[%.2f..%.2f] (span u %.2f v %.2f) q[%.0f..%.0f]", i,
                    static_cast<double>(sTcMin[i][0]), static_cast<double>(sTcMax[i][0]),
                    static_cast<double>(sTcMin[i][1]), static_cast<double>(sTcMax[i][1]),
                    static_cast<double>(sTcMax[i][0] - sTcMin[i][0]),
                    static_cast<double>(sTcMax[i][1] - sTcMin[i][1]),
                    static_cast<double>(sQMin[i]), static_cast<double>(sQMax[i]));
    }
}
int sVertexStride = 0;

// M5.2: one dynamic vertex buffer reused across primitives and frames (see
// Renderer::createDynamicBuffer). Freed by GXCompatShutdown().
Platform::BufferHandle sDynVb = nullptr;
uint64_t sDynUsedBytes = 0; // cursor: bytes of this frame already written

// Debug snapshot of the last complete primitive (floats, VCD order).
std::vector<float> sDebugData;
int sDebugStride = 0;

// --- helpers -----------------------------------------------------------------

int attrComponentCount(GXVtxFmt fmt, int attr) {
    const AttrFmt& a = sAttrFmt[fmt][attr];
    // NOTE: the GX_* component-count enums share numeric values across groups
    // (GX_POS_XY=0 == GX_NRM_XYZ=0, GX_POS_XYZ=1 == GX_TEX_ST=1), so they
    // cannot be switched on globally — resolve per attribute group instead.
    if (attr == GX_VA_NRM) {
        return 3; // a normal is always XYZ when present
    }
    if (attr == GX_VA_POS) {
        return a.cnt + 2; // GX_POS_XY=0 -> 2, GX_POS_XYZ=1 -> 3
    }
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        return a.cnt + 1; // GX_TEX_S=0 -> 1, GX_TEX_ST=1 -> 2
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        return 4;
    }
    return 0;
}

// Rebuilds sVcdOrder from the enabled attributes (VCD order). Indexed
// attributes (GX_INDEX8/16) consume ONE FIFO word (the index) per attribute;
// direct attributes consume one word per component.
void rebuildVcd() {
    sVcdOrder.clear();
    // VCD attribute order is the GX_VA_* enum order for the geometry
    // attributes (POS=9 < NRM=10 < CLR0=11 < CLR1=12 < TEX0=13 < ...).
    for (int attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
        if (sVtxDescSet[attr] && sVtxDesc[attr] != GX_NONE) {
            const int comps = attrComponentCount(sVtxFmt, attr);
            if (comps > 0) {
                const AttrFmt& fmt = sAttrFmt[sVtxFmt][attr];
                sVcdOrder.push_back({attr, comps, sVtxDesc[attr], fmt.type, fmt.frac});
            }
        }
    }
    sVcdTotalWrites = 0;
    for (const auto& slot : sVcdOrder) {
        sVcdTotalWrites += (slot.source == GX_DIRECT) ? slot.comps : 1;
    }
    // M9.10 perf: precompute the FIFO write-index decode table (see
    // WriteStep). One entry per FIFO word of the vertex, in stream order.
    sWriteSteps.clear();
    sWriteSteps.reserve(static_cast<size_t>(sVcdTotalWrites));
    for (int si = 0; si < static_cast<int>(sVcdOrder.size()); ++si) {
        const VcdSlot& slot = sVcdOrder[static_cast<size_t>(si)];
        const int writes = (slot.source == GX_DIRECT) ? slot.comps : 1;
        for (int w = 0; w < writes; ++w) {
            sWriteSteps.push_back({static_cast<std::int16_t>(si),
                                   static_cast<std::int16_t>(w)});
        }
    }
}

// Converts a raw GX component value to float according to the attribute's
// comp type (GX_VAT). Colors (u8/rgba) are normalized to 0..1; numeric
// formats scale by the fraction (value / 2^frac). M9.10 perf: reads the VAT
// fields cached in the VCD slot instead of re-resolving them per FIFO word.
float convertComponent(const VcdSlot& slot, float value, bool isColorByte) {
    if (slot.attr == GX_VA_CLR0 || slot.attr == GX_VA_CLR1) {
        return isColorByte ? value / 255.0f : value;
    }
    switch (slot.type) {
        case GX_U8:
        case GX_S8:
        case GX_U16:
        case GX_S16:
            return value / static_cast<float>(1 << slot.frac);
        case GX_F32:
        default:
            return value;
    }
}

// Byte size of one component for the given comp type (VAT), as stored in an
// attribute array. Color formats (RGBA8, ...) are handled in fetchArrayAttr
// and never reach this helper.
int compByteSize(GXCompType type) {
    switch (type) {
        case GX_U8:
        case GX_S8: return 1;
        case GX_U16:
        case GX_S16: return 2;
        case GX_F32:
        default: return 4;
    }
}

// Reads one component from an attribute array and scales it to float exactly
// like the FIFO path (fixed-point: value / 2^frac; f32: as-is).
float readArrayComp(const u8* p, GXCompType type, u8 frac) {
    switch (type) {
        case GX_U8: {
            u8 v;
            std::memcpy(&v, p, 1);
            return static_cast<float>(v) / static_cast<float>(1 << frac);
        }
        case GX_S8: {
            s8 v;
            std::memcpy(&v, p, 1);
            return static_cast<float>(v) / static_cast<float>(1 << frac);
        }
        case GX_U16: {
            u16 v;
            std::memcpy(&v, p, 2);
            return static_cast<float>(v) / static_cast<float>(1 << frac);
        }
        case GX_S16: {
            s16 v;
            std::memcpy(&v, p, 2);
            return static_cast<float>(v) / static_cast<float>(1 << frac);
        }
        case GX_F32:
        default: {
            float v;
            std::memcpy(&v, p, 4);
            return v;
        }
    }
}

// Resolves the attribute data for `index` from the GXSetArray array and fills
// sCurAttr[attr][0..comps). Colors RGBA8 normalize to 0..1. M9.10 perf: the
// component count and VAT fields come from the VCD slot (resolved once per
// VCD/VAT change), not re-derived per indexed fetch.
void fetchArrayAttr(const VcdSlot& slot, u32 index) {
    const int attr = slot.attr;
    const ArraySlot& arr = sArrays[attr];
    if (!arr.set || !arr.base) {
        PL_LOG_WARN("gx", "indexed attribute %d used without GXSetArray — zeros",
                    attr);
        for (int c = 0; c < 4; ++c) {
            sCurAttr[attr][c] = 0.0f;
        }
        return;
    }
    const u8* p = arr.base + static_cast<size_t>(index) * arr.stride;
    const int comps = slot.comps;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        // M5.2 handles RGBA8 arrays; other color encodings (RGB565, ...)
        // arrive with the color pipeline (M5.5).
        for (int c = 0; c < comps; ++c) {
            sCurAttr[attr][c] = p[c] / 255.0f;
        }
        return;
    }
    const int elemSize = compByteSize(slot.type);
    for (int c = 0; c < comps; ++c) {
        sCurAttr[attr][c] = readArrayComp(p + c * elemSize, slot.type, slot.frac);
    }
}

// Submits the completed primitive to the renderer (if initialized).
void flushDraw();

void finishVertex() {
    // M5.3: resolve the texcoord generators (GXSetTexCoordGen2) on the CPU
    // using the completed vertex attributes before serializing.
    //
    // M9.5.5: EVERY configured generator is evaluated, not only the ones whose
    // destination attribute happens to be in the vertex stream. A BMD shape
    // only declares the attributes it stores (the sky dome stores POS/NRM/TEX0),
    // while its material can configure three texgens whose sources are POS and
    // TEX0 — e.g. EarthFar_v: tc0 (projmap from POS), tc1 (TEX0 through TEXMTX1)
    // and tc2 (projmap from POS). Driving the loop from the VCD left tc1/tc2 at
    // whatever the previous vertex had put there, which sampled the earth
    // crescent (EarthFarK, the teal of the sea) and the clouds at a stale
    // texcoord. The generators also all read the *incoming* attributes, so the
    // snapshot keeps a generator that writes TEX0 from feeding the next one.
    const bool tcSpanLog = tcSpanLogEnabled();
    if (tcSpanLog) {
        if (sTcSpanNverts == 0) {
            for (int c = 0; c < 3; ++c) {
                sPosMin[c] = sPosMax[c] = sCurAttr[GX_VA_POS][c];
            }
        } else {
            for (int c = 0; c < 3; ++c) {
                const float p = sCurAttr[GX_VA_POS][c];
                sPosMin[c] = p < sPosMin[c] ? p : sPosMin[c];
                sPosMax[c] = p > sPosMax[c] ? p : sPosMax[c];
            }
        }
        ++sTcSpanNverts;
    }
    // M9.10 perf: the snapshot exists so generators read the INCOMING
    // attributes — and resolveTexGen only ever reads the POS and TEX0..7
    // rows (any other source passes through). Copy just those rows (144
    // bytes) instead of the full 25-row bank (400 bytes), and skip the copy
    // entirely when no generator is active.
    static_assert(GX_VA_TEX7 == GX_VA_TEX0 + 7, "TEX0..7 attribute rows must be contiguous");
    // M9.9 perf: the hardware only RUNS the texgen units below numTexGens
    // (GEN_MODE); coordinates at or above it are undefined at the TEV, so
    // evaluating every configured generator for all eight units per vertex was
    // wasted work — J3D materials always set GXSetNumTexGens to the number of
    // generators they configure (and the TEV can only sample below it).
    // Inactive units keep the incoming attribute (passthrough, q = 1), the
    // same as an unset generator.
    const int activeGens = Platform::CompatGx::tevTexGenCount();
    float snapshot[GX_VA_MAX_ATTR][4];
    if (activeGens > 0) {
        std::memcpy(snapshot[GX_VA_POS], sCurAttr[GX_VA_POS],
                    sizeof(snapshot[GX_VA_POS]));
        std::memcpy(snapshot[GX_VA_TEX0], sCurAttr[GX_VA_TEX0],
                    8 * sizeof(snapshot[GX_VA_TEX0]));
    }
    for (int coord = 0; coord < 8; ++coord) {
        if (coord < activeGens) {
            Platform::CompatGx::resolveTexGen(coord, sCurAttr, snapshot);
        }
    }
    for (int coord = 0; coord < 8; ++coord) {
        const int attr = GX_VA_TEX0 + coord;
        const float u = sCurAttr[attr][0];
        const float v = sCurAttr[attr][1];
        const float q = (coord < activeGens) ? Platform::CompatGx::texGenW(coord) : 1.0f;
        if (tcSpanLog) {
            // The sampler sees u/q: a projective texgen's numerator alone says
            // nothing about how much texture the geometry shows.
            const float su = (q != 0.0f) ? u / q : u;
            const float sv = (q != 0.0f) ? v / q : v;
            if (!sTcSeen[coord]) {
                sTcSeen[coord] = true;
                sTcMin[coord][0] = sTcMax[coord][0] = su;
                sTcMin[coord][1] = sTcMax[coord][1] = sv;
                sQMin[coord] = sQMax[coord] = q;
            } else {
                sTcMin[coord][0] = su < sTcMin[coord][0] ? su : sTcMin[coord][0];
                sTcMax[coord][0] = su > sTcMax[coord][0] ? su : sTcMax[coord][0];
                sTcMin[coord][1] = sv < sTcMin[coord][1] ? sv : sTcMin[coord][1];
                sTcMax[coord][1] = sv > sTcMax[coord][1] ? sv : sTcMax[coord][1];
                sQMin[coord] = q < sQMin[coord] ? q : sQMin[coord];
                sQMax[coord] = q > sQMax[coord] ? q : sQMax[coord];
            }
        }
        sVertexTexCoords.push_back(u);
        sVertexTexCoords.push_back(v);
        sVertexTexQ.push_back(q);
    }
    // Serialize the vertex (VCD order) into the stream.
    for (const auto& slot : sVcdOrder) {
        const int comps = slot.comps;
        for (int c = 0; c < comps; ++c) {
            sVertexData.push_back(sCurAttr[slot.attr][c]);
        }
    }
    sVtxWriteIndex = 0;
    ++sNvertsDone;
    if (sNvertsDone >= sNverts) {
        flushDraw();
    }
}

// --- capture from the write-gather pipe --------------------------------------

// writeSize = bytes this 32-bit FIFO word contributes to the stream (u8/s8 ->
// 1, u16/s16 -> 2, f32 -> 4). The stream is consumed positionally in VCD
// order, exactly like the PPC vertex loader: each write fills the next
// component(s) of the current vertex. For an indexed slot the FIFO word is
// the array index and the attribute data is fetched via fetchArrayAttr.
// M9.10 perf: the (slot, component) decode is a single lookup in the
// precomputed sWriteSteps table (rebuildVcd) — this runs once per FIFO word,
// ~450k times/frame on the fileselect planets.
bool noteFastColorByte(std::uint8_t b);

void captureWrite(float value, int writeSize) {
    if (noteFastColorByte(static_cast<std::uint8_t>(value))) {
        return;
    }
    if (!sInBegin) {
        // Writes outside GXBegin are illegal on the console too; the game
        // never does this.
        PL_LOG_WARN("gx", "vertex write outside GXBegin ignored");
        return;
    }
    if (static_cast<size_t>(sVtxWriteIndex) >= sWriteSteps.size()) {
        PL_LOG_WARN("gx", "vertex write overflow (VCD writes %d)", sVcdTotalWrites);
        return;
    }
    const WriteStep step = sWriteSteps[static_cast<size_t>(sVtxWriteIndex)];
    const VcdSlot& slot = sVcdOrder[static_cast<size_t>(step.slot)];
    if (slot.source != GX_DIRECT) {
        // Indexed attribute: one FIFO word = the array index (u8/u16 values
        // are exact in float). Resolve the data from the GXSetArray array.
        fetchArrayAttr(slot, static_cast<u32>(value));
    } else {
        const bool isColor = (slot.attr == GX_VA_CLR0 || slot.attr == GX_VA_CLR1);
        // One FIFO word = one component of the vertex.
        sCurAttr[slot.attr][step.comp] =
            convertComponent(slot, value, writeSize == 1 && isColor);
    }
    sVtxWriteIndex += 1;
    if (sVtxWriteIndex >= sVcdTotalWrites) {
        finishVertex();
    }
}

// GXColor1u32: one 32-bit FIFO word packs 4 RGBA8 bytes (big-endian: r = MSB
// ... a = LSB). Handled separately from captureWrite because the raw u32 must
// not round-trip through float (it would lose the low bytes). Only valid for
// DIRECT color slots (indexed colors use GXColor1x8/1x16).
void capturePackedU32(std::uint32_t packed) {
    if (!sInBegin) {
        PL_LOG_WARN("gx", "vertex write outside GXBegin ignored");
        return;
    }
    if (static_cast<size_t>(sVtxWriteIndex) >= sWriteSteps.size()) {
        PL_LOG_WARN("gx", "vertex write overflow (VCD writes %d)", sVcdTotalWrites);
        return;
    }
    const WriteStep step = sWriteSteps[static_cast<size_t>(sVtxWriteIndex)];
    const VcdSlot& slot = sVcdOrder[static_cast<size_t>(step.slot)];
    if (slot.source != GX_DIRECT ||
        (slot.attr != GX_VA_CLR0 && slot.attr != GX_VA_CLR1)) {
        PL_LOG_WARN("gx", "u32 write into a non-direct-color attribute — dropped");
        return;
    }
    if (sInBegin) {
        // Fast indexed-strip path consumes a packed RGBA8 as four color bytes.
        bool ate = false;
        for (int c = 0; c < 4; ++c) {
            const u8 byte = static_cast<u8>((packed >> (24 - 8 * c)) & 0xFF);
            ate = noteFastColorByte(byte) || ate;
        }
        if (ate) {
            return;
        }
    }
    for (int c = 0; c < 4; ++c) {
        const u8 byte = static_cast<u8>((packed >> (24 - 8 * c)) & 0xFF);
        sCurAttr[slot.attr][step.comp + c] = byte / 255.0f;
    }
    sVtxWriteIndex += 4;
    if (sVtxWriteIndex >= sVcdTotalWrites) {
        finishVertex();
    }
}

} // namespace

GxIndexCapture gGxIndexCapture;

namespace Platform::CompatGx::Detail {

void fifoWriteU8(std::uint8_t v) { captureWrite(static_cast<float>(v), 1); }
void fifoWriteS8(std::int8_t v) { captureWrite(static_cast<float>(v), 1); }
void fifoWriteU16(std::uint16_t v) { captureWrite(static_cast<float>(v), 2); }
void fifoWriteS16(std::int16_t v) { captureWrite(static_cast<float>(v), 2); }
void fifoWriteU32(std::uint32_t v) { capturePackedU32(v); }
void fifoWriteF32(float v) { captureWrite(v, 4); }

} // namespace Platform::CompatGx::Detail

namespace {

// --- flush to the renderer ---------------------------------------------------

// M5.4: 1x1 white texture + nearest/clamp sampler, bound for unloaded TEXMAP
// slots (a TEV stage sampling an unconfigured map sees white, like the
// console's default TMEM contents). Created lazily, destroyed on shutdown.
Platform::TextureHandle sWhiteTex = nullptr;
Platform::SamplerHandle sWhiteSam = nullptr;

void ensureWhiteFallback() {
    if (sWhiteTex || !Platform::Renderer::instance().isInitialized()) {
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();
    const uint8_t white[4] = {255, 255, 255, 255};
    Platform::TextureDesc td;
    td.width = 1;
    td.height = 1;
    td.format = Platform::TextureFormat::R8G8B8A8_UNORM;
    td.initialData = white;
    td.debugName = "gx-tev-white";
    sWhiteTex = r.createTexture(td);
    Platform::SamplerDesc sd;
    sd.magFilter = Platform::SamplerFilter::Nearest;
    sd.minFilter = Platform::SamplerFilter::Nearest;
    sWhiteSam = r.getOrCreateSampler(sd);
}

void destroyWhiteFallback() {
    if (sWhiteTex && Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().destroyTexture(sWhiteTex);
    }
    sWhiteTex = nullptr;
    sWhiteSam = nullptr;
}

// --- draw batching (M9.7 perf) ----------------------------------------------
// The GX immediate path emits one vkCmdDraw per GXBegin/GXEnd primitive. A
// dense brlyt screen (fileselect) draws several thousand text/pane quads per
// frame that share the exact same font texture, TEV state and MVP, so they
// differ ONLY in vertex data. Recording thousands of separate draws is pure
// CPU overhead (~9 vkCmd calls each) and is what pegs the fileselect screen at
// single-digit FPS (frame-budget log: ~5000 draws/frame there vs ~300 on the
// title). flushDraw now coalesces a run of consecutive primitives whose ENTIRE
// render state matches bit-for-bit into a single draw: the vertices are laid
// out contiguously in the shared dynamic buffer, so the run collapses to one
// (firstVertex, vertexCount) pair, and the bind+draw is deferred until the
// state changes or the pass closes (endPass -> flushPendingBatch via the hook).
// Triangle lists (QUADS/TRIANGLES/FAN) coalesce by concatenation; triangle
// STRIPS coalesce too (M9.9) via a degenerate-vertex junction that keeps the
// winding parity; line/point runs cannot be concatenated and flush alone.
struct PendingBatch {
    Platform::PipelineHandle pipe = nullptr;
    Platform::TextureHandle tex[8] = {};
    Platform::SamplerHandle sam[8] = {};
    Platform::CompatGx::TevUboData ubo;
    float mvp[16] = {};
    float vp[4] = {};          // mirrored GX viewport x,y,w,h
    uint32_t scissor[4] = {};  // x,y,w,h
    bool scissorSet = false;
    uint8_t blendAlpha = 255;  // GXSetDstAlpha constant (0..255)
    uint32_t firstVertex = 0;  // offset of the run's first vertex (fixed stride)
    uint32_t vertexCount = 0;  // accumulated vertex count across the run
    // M9.9: the run's topology. Triangle lists concatenate as-is; triangle
    // strips concatenate through a degenerate junction (flushDraw). A list
    // primitive and a strip primitive never share a run even with identical
    // state, so the topology is part of the match.
    Platform::PrimitiveTopology topo = Platform::PrimitiveTopology::TriangleList;
    bool valid = false;
};

PendingBatch sBatch;
bool sEndPassHookSet = false;
// M9.9: last vertex (kFixedStride floats) written into the dynamic buffer —
// the degenerate strip junction repeats it to bridge two strips.
constexpr int kBatchFixedStride = 35;  // must match flushDraw's kFixedStride
float sLastVertOut[kBatchFixedStride] = {};

// True if candidate `b` can be appended to pending run `a` (identical state).
inline bool batchMatches(const PendingBatch& a, const PendingBatch& b) {
    return a.pipe == b.pipe && a.blendAlpha == b.blendAlpha &&
           a.topo == b.topo &&
           a.scissorSet == b.scissorSet && a.vp[0] == b.vp[0] && a.vp[1] == b.vp[1] &&
           a.vp[2] == b.vp[2] && a.vp[3] == b.vp[3] &&
           std::memcmp(a.tex, b.tex, sizeof(a.tex)) == 0 &&
           std::memcmp(a.sam, b.sam, sizeof(a.sam)) == 0 &&
           std::memcmp(a.mvp, b.mvp, sizeof(a.mvp)) == 0 &&
           std::memcmp(&a.ubo, &b.ubo, sizeof(a.ubo)) == 0 &&
           std::memcmp(a.scissor, b.scissor, sizeof(a.scissor)) == 0;
}

// Emits the pending batch as one draw and clears it. No-op when nothing is
// pending. Called from flushDraw (state change / non-list primitive) and from
// the renderer's endPass hook (pass close).
void flushPendingBatch() {
    if (!sBatch.valid) {
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();
    if (r.isInitialized() && r.inPass()) {
        r.bindPipeline(sBatch.pipe);
        r.setBlendConstantAlpha(sBatch.blendAlpha / 255.0f);
        if (sWhiteTex) {
            r.bindFragmentTextures(sBatch.tex, sBatch.sam, 8);
        }
        if (r.uploadFragmentUbo(&sBatch.ubo, sizeof(sBatch.ubo))) {
            r.bindVertexBuffer(sDynVb, 0);
            if (sBatch.vp[2] > 0.0f && sBatch.vp[3] > 0.0f) {
                r.setViewport(sBatch.vp[0], sBatch.vp[1], sBatch.vp[2], sBatch.vp[3]);
            }
            if (sBatch.scissorSet) {
                r.setScissor(sBatch.scissor[0], sBatch.scissor[1], sBatch.scissor[2],
                             sBatch.scissor[3]);
            }
            r.setUniforms(sBatch.mvp, sizeof(sBatch.mvp));
            r.draw(sBatch.vertexCount, sBatch.firstVertex);
        }
    }
    sBatch.valid = false;
}


// Cached texgen units for the indexed-strip expander. Filled once per primitive.
Platform::CompatGx::TexGenUnit sTexGenUnits[8];

bool applyTexGenUnit(const Platform::CompatGx::TexGenUnit& g, const float* s,
                     float& u, float& v, float& q) {
    q = 1.0f;
    if (g.write == 0 || g.srcAttr < 0 || s == nullptr) {
        return false;
    }
    if (g.useMtx == 0) {
        u = s[0];
        v = s[1];
        return true;
    }
    u = g.m[0] * s[0] + g.m[1] * s[1] + g.m[2] * s[2] + g.m[3];
    v = g.m[4] * s[0] + g.m[5] * s[1] + g.m[6] * s[2] + g.m[7];
    if (g.proj3 != 0) {
        const float w = g.m[8] * s[0] + g.m[9] * s[1] + g.m[10] * s[2] + g.m[11];
        if (w != 0.0f) {
            q = w;
        }
    }
    return true;
}

// All-indexed triangle strips (File Select planets: 64 strips x ~700 verts).
// The immediate writers and the display-list reader only record indices; one
// loop fetches, lights and emits the TEV vertex. Quads, fans and mixed VCDs
// stay on the general path so their captured streams are unchanged.
struct FastSlot {
    int attr = 0;
    int comps = 0;
    int indexBytes = 2;
    const std::uint8_t* base = nullptr;
    int stride = 0;
    int type = 0;
    int frac = 0;
    int elemSize = 4;
    bool isColor = false;
};
FastSlot sFastSlot[16];
int sFastSlots = 0;
int sFastCount = 0;
bool sFastIndex = false;
std::vector<std::uint32_t> sFastIndices;
std::vector<float> sFastDrawOut;
bool sFastDrawReady = false;
bool sFastHasColor = false;
int sFastColorAttr = -1;
int sFastColorComps = 0;
int sFastColorCount = 0;
std::vector<std::uint8_t> sFastColorBytes;

void fetchFastAttr(const FastSlot& sl, std::uint32_t index, float dst[4]) {
    const std::uint8_t* p = sl.base + static_cast<size_t>(index) * static_cast<size_t>(sl.stride);
    if (sl.isColor) {
        for (int c = 0; c < sl.comps && c < 4; ++c) {
            dst[c] = p[c] / 255.0f;
        }
        for (int c = sl.comps; c < 4; ++c) {
            dst[c] = 1.0f;  // missing color components default to 1, like buildVertex
        }
        return;
    }
    const float scale = static_cast<float>(1 << sl.frac);
    for (int c = 0; c < sl.comps && c < 4; ++c) {
        const std::uint8_t* q = p + c * sl.elemSize;
        switch (sl.type) {
        case GX_U8: {
            std::uint8_t v;
            std::memcpy(&v, q, 1);
            dst[c] = static_cast<float>(v) / scale;
            break;
        }
        case GX_S8: {
            std::int8_t v;
            std::memcpy(&v, q, 1);
            dst[c] = static_cast<float>(v) / scale;
            break;
        }
        case GX_U16: {
            std::uint16_t v;
            std::memcpy(&v, q, 2);
            dst[c] = static_cast<float>(v) / scale;
            break;
        }
        case GX_S16: {
            std::int16_t v;
            std::memcpy(&v, q, 2);
            dst[c] = static_cast<float>(v) / scale;
            break;
        }
        default: {
            float v;
            std::memcpy(&v, q, 4);
            dst[c] = v;
            break;
        }
        }
    }
    for (int c = sl.comps; c < 4; ++c) {
        dst[c] = 0.0f;
    }
}

// True when this primitive is an all-indexed triangle strip whose texgens only
// read attributes the vertex itself carries. `usePipe` arms the immediate-mode
// index store; display lists fill sFastIndices themselves.
bool tryArmFastIndex(int nverts, bool usePipe) {
    gGxIndexCapture.active = false;
    gGxIndexCapture.count = 0;
    sFastIndex = false;
    sFastDrawReady = false;
    sFastCount = 0;
    sFastSlots = 0;
    if (nverts < 3 || sPrimitive != GX_TRIANGLESTRIP || sVcdOrder.empty() ||
        sVcdOrder.size() > 16) {
        return false;
    }
    bool hasPos = false;
    sFastHasColor = false;
    sFastColorAttr = -1;
    sFastColorComps = 0;
    sFastColorCount = 0;
    int indexed = 0;
    for (const auto& slot : sVcdOrder) {
        if (slot.source == GX_DIRECT) {
            // Planets carry one direct RGBA8 color among indexed pos/nrm/tex.
            if (sFastHasColor || slot.comps != 4 ||
                (slot.attr != GX_VA_CLR0 && slot.attr != GX_VA_CLR1)) {
                return false;
            }
            sFastHasColor = true;
            sFastColorAttr = slot.attr;
            sFastColorComps = slot.comps;
            continue;
        }
        if (slot.source != GX_INDEX8 && slot.source != GX_INDEX16) {
            return false;
        }
        const ArraySlot& arr = sArrays[slot.attr];
        if (!arr.set || arr.base == nullptr) {
            return false;
        }
        if (slot.attr == GX_VA_POS) {
            hasPos = true;
        }
        ++indexed;
    }
    if (!hasPos || indexed == 0) {
        return false;
    }
    Platform::CompatGx::captureTexGenUnits(sTexGenUnits);
    for (int i = 0; i < 8; ++i) {
        if (sTexGenUnits[i].write == 0) {
            continue;
        }
        bool found = false;
        for (const auto& slot : sVcdOrder) {
            if (slot.attr == sTexGenUnits[i].srcAttr) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    sFastSlots = indexed;
    int fi = 0;
    for (const auto& slot : sVcdOrder) {
        if (slot.source == GX_DIRECT) {
            continue;
        }
        FastSlot& f = sFastSlot[fi++];
        f.attr = slot.attr;
        f.comps = slot.comps;
        f.indexBytes = (slot.source == GX_INDEX16) ? 2 : 1;
        f.base = sArrays[slot.attr].base;
        f.stride = sArrays[slot.attr].stride;
        f.type = static_cast<int>(slot.type);
        f.frac = slot.frac;
        f.elemSize = compByteSize(slot.type);
        f.isColor = (slot.attr == GX_VA_CLR0 || slot.attr == GX_VA_CLR1);
    }
    const int expected = nverts * sFastSlots;
    sFastIndices.assign(static_cast<size_t>(expected), 0);
    if (sFastHasColor) {
        sFastColorBytes.assign(static_cast<size_t>(nverts) * static_cast<size_t>(sFastColorComps), 0);
    } else {
        sFastColorBytes.clear();
    }
    sFastIndex = true;
    if (usePipe) {
        gGxIndexCapture.data = sFastIndices.data();
        gGxIndexCapture.count = 0;
        gGxIndexCapture.expected = expected;
        gGxIndexCapture.active = true;
    }
    return true;
}

struct CapOp {
    int kind;  // 0 pos, 1 nrm, 2 clr0, 3 clr1, 4+t tex, -1 zero
    int comps;
};

struct TgOp {
    int src;  // -1 pos, 0..7 tex, -2 zero
    int dst;
    int proj3;
    int useMtx;
    float m[12];
};

inline void loadFastSlot(const FastSlot& sl, std::uint32_t index, float dst[4]) {
    const std::uint8_t* p = sl.base + static_cast<size_t>(index) * static_cast<size_t>(sl.stride);
    if (sl.isColor) {
        const int n = sl.comps < 4 ? sl.comps : 4;
        for (int c = 0; c < n; ++c) {
            dst[c] = p[c] * (1.0f / 255.0f);
        }
        for (int c = n; c < 4; ++c) {
            dst[c] = 1.0f;
        }
        return;
    }
    if (sl.type == GX_F32 && sl.frac == 0 && sl.elemSize == 4) {
        const float* f = reinterpret_cast<const float*>(p);
        dst[0] = sl.comps > 0 ? f[0] : 0.0f;
        dst[1] = sl.comps > 1 ? f[1] : 0.0f;
        dst[2] = sl.comps > 2 ? f[2] : 0.0f;
        dst[3] = sl.comps > 3 ? f[3] : 0.0f;
        return;
    }
    fetchFastAttr(sl, index, dst);
}

// Attn/Diff are per-primitive. Instantiating them here lets the spot/spec
// evaluator inline into the vertex loop instead of switching 44k times.
template <int Attn, int Diff, bool Uniform>
void expandHotBody(int nverts, bool render, float* draw, float* cap, int capStride,
                   const std::uint32_t* indices, int slots, int posSlot, int nrmSlot,
                   int clr0Slot, int clr1Slot, const int texSlot[8], const CapOp* caps, int ncap,
                   const TgOp* tgs, int ntg, const float* posMtx, const float* nrmMtx,
                   const Platform::CompatGx::LightingInputs& lin, unsigned uniMask,
                   int posComps, int nrmComps) {
    constexpr int kStride = 35;
    const std::uint8_t* colorBytes = sFastHasColor ? sFastColorBytes.data() : nullptr;
    const int colorComps = sFastColorComps;
    const int colorAttr = sFastColorAttr;
    for (int v = 0; v < nverts; ++v) {
        const std::uint32_t* vi = indices + static_cast<size_t>(v) * static_cast<size_t>(slots);
        float pos[3] = {0, 0, 0};
        float nrm[3] = {0, 0, 1};
        float clr0[4] = {1, 1, 1, 1};
        float clr1[4] = {1, 1, 1, 1};
        float tu[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        float tv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        float tz[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        if (posSlot >= 0) {
            float row[4];
            loadFastSlot(sFastSlot[posSlot], vi[posSlot], row);
            pos[0] = row[0];
            pos[1] = row[1];
            pos[2] = row[2];
        }
        if (nrmSlot >= 0) {
            float row[4];
            loadFastSlot(sFastSlot[nrmSlot], vi[nrmSlot], row);
            nrm[0] = row[0];
            nrm[1] = row[1];
            nrm[2] = (nrmComps == 3) ? row[2] : 1.0f;
        }
        if (clr0Slot >= 0) {
            loadFastSlot(sFastSlot[clr0Slot], vi[clr0Slot], clr0);
        }
        if (clr1Slot >= 0) {
            loadFastSlot(sFastSlot[clr1Slot], vi[clr1Slot], clr1);
        }
        if (colorBytes != nullptr &&
            v * colorComps + colorComps <= sFastColorCount) {
            const std::uint8_t* cb = colorBytes + static_cast<size_t>(v) * static_cast<size_t>(colorComps);
            float* dst = (colorAttr == GX_VA_CLR0) ? clr0 : clr1;
            for (int c = 0; c < colorComps && c < 4; ++c) {
                dst[c] = cb[c] * (1.0f / 255.0f);
            }
        }
        float inU[8], inV[8], inZ[8];
        for (int t = 0; t < 8; ++t) {
            if (texSlot[t] < 0) {
                inU[t] = inV[t] = inZ[t] = 0.0f;
                continue;
            }
            float row[4];
            loadFastSlot(sFastSlot[texSlot[t]], vi[texSlot[t]], row);
            inU[t] = tu[t] = row[0];
            inV[t] = tv[t] = row[1];
            inZ[t] = tz[t] = row[2];
        }
        float uv0[8], uv1[8], uq[8];
        for (int t = 0; t < 8; ++t) {
            uv0[t] = tu[t];
            uv1[t] = tv[t];
            uq[t] = 1.0f;
        }
        for (int i = 0; i < ntg; ++i) {
            const TgOp& tg = tgs[i];
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f;
            if (tg.src == -1) {
                s0 = pos[0];
                s1 = pos[1];
                s2 = pos[2];
            } else if (tg.src >= 0) {
                s0 = inU[tg.src];
                s1 = inV[tg.src];
                s2 = inZ[tg.src];
            }
            float u, vv, q = 1.0f;
            if (tg.useMtx == 0) {
                u = s0;
                vv = s1;
            } else {
                u = tg.m[0] * s0 + tg.m[1] * s1 + tg.m[2] * s2 + tg.m[3];
                vv = tg.m[4] * s0 + tg.m[5] * s1 + tg.m[6] * s2 + tg.m[7];
                if (tg.proj3 != 0) {
                    const float w = tg.m[8] * s0 + tg.m[9] * s1 + tg.m[10] * s2 + tg.m[11];
                    if (w != 0.0f) {
                        q = w;
                    }
                }
            }
            uv0[tg.dst] = u;
            uv1[tg.dst] = vv;
            uq[tg.dst] = q;
            tu[tg.dst] = u;
            tv[tg.dst] = vv;
        }
        float* cout = cap + static_cast<size_t>(v) * static_cast<size_t>(capStride);
        int w = 0;
        for (int i = 0; i < ncap; ++i) {
            int n = caps[i].comps;
            if (caps[i].kind == 0) {
                for (int c = 0; c < n; ++c) {
                    cout[w++] = pos[c];
                }
            } else if (caps[i].kind == 1) {
                for (int c = 0; c < n; ++c) {
                    cout[w++] = nrm[c];
                }
            } else if (caps[i].kind == 2) {
                for (int c = 0; c < n; ++c) {
                    cout[w++] = clr0[c];
                }
            } else if (caps[i].kind == 3) {
                for (int c = 0; c < n; ++c) {
                    cout[w++] = clr1[c];
                }
            } else if (caps[i].kind >= 4) {
                const int t = caps[i].kind - 4;
                for (int c = 0; c < n; ++c) {
                    cout[w++] = (c == 0) ? tu[t] : (c == 1) ? tv[t] : tz[t];
                }
            } else {
                for (int c = 0; c < n; ++c) {
                    cout[w++] = 0.0f;
                }
            }
        }
        if (!render) {
            continue;
        }
        float* o = draw + static_cast<size_t>(v) * kStride;
        const float px = pos[0], py = pos[1], pz = (posComps == 3) ? pos[2] : 0.0f;
        o[0] = px;
        o[1] = py;
        o[2] = pz;
        const float nx = nrm[0], ny = nrm[1], nz = nrm[2];
        const float posView[3] = {
            px * posMtx[0] + py * posMtx[1] + pz * posMtx[2] + posMtx[3],
            px * posMtx[4] + py * posMtx[5] + pz * posMtx[6] + posMtx[7],
            px * posMtx[8] + py * posMtx[9] + pz * posMtx[10] + posMtx[11],
        };
        float nrmView[3] = {
            nx * nrmMtx[0] + ny * nrmMtx[1] + nz * nrmMtx[2],
            nx * nrmMtx[3] + ny * nrmMtx[4] + nz * nrmMtx[5],
            nx * nrmMtx[6] + ny * nrmMtx[7] + nz * nrmMtx[8],
        };
        const float nLenSq =
            nrmView[0] * nrmView[0] + nrmView[1] * nrmView[1] + nrmView[2] * nrmView[2];
        if (nLenSq > 0.0f) {
            const float inv = 1.0f / std::sqrt(nLenSq);
            nrmView[0] *= inv;
            nrmView[1] *= inv;
            nrmView[2] *= inv;
        }
        float lit0[4], lit1[4];
        if constexpr (Uniform) {
            Platform::CompatGx::evalUniformLighting<Attn, Diff>(
                lin.chan, lin.amb, lin.mat, lin.lights, uniMask, posView, nrmView, clr0, clr1, lit0,
                lit1);
        } else {
            Platform::CompatGx::evaluateChannelLighting(lin.chan, lin.amb, lin.mat, lin.lights, posView,
                                                        nrmView, clr0, clr1, lit0, lit1);
        }
        o[3] = lit0[0];
        o[4] = lit0[1];
        o[5] = lit0[2];
        o[6] = lit0[3];
        o[7] = lit1[0];
        o[8] = lit1[1];
        o[9] = lit1[2];
        o[10] = lit1[3];
        for (int t = 0; t < 8; ++t) {
            o[11 + 3 * t] = uv0[t];
            o[12 + 3 * t] = uv1[t];
            o[13 + 3 * t] = uq[t];
        }
    }
}


// Tight planet strip: F32 pos/nrm/tex, one RGBA8 color, uniform lighting.
// Kept small on purpose — the general expander inlines into a 10 KB loop and
// drops out of the instruction cache (~13 ms). This one stays a few hundred
// bytes so the same formula finishes inside a retrace.
// Branch-free planet body. Channel flags, light colors and the two texgen
// matrices are per primitive; baking them here is what keeps the loop in L1.
// Attn/Diff stay template parameters so spot/clamp compiles without a switch.
inline float spotDiffuse(const Platform::CompatGx::LightParams& L, const float pv0, const float pv1,
                         const float pv2, const float nv0, const float nv1, const float nv2) {
    const float d0 = L.pos[0] - pv0;
    const float d1 = L.pos[1] - pv1;
    const float d2 = L.pos[2] - pv2;
    const float dist2 = d0 * d0 + d1 * d1 + d2 * d2;
    const float dist = std::sqrt(dist2);
    float lx, ly, lz;
    if (dist != 0.0f) {
        lx = d0 / dist;
        ly = d1 / dist;
        lz = d2 / dist;
    } else {
        lx = nv0;
        ly = nv1;
        lz = nv2;
    }
    const float ndl = nv0 * lx + nv1 * ly + nv2 * lz;
    const float cosA = Platform::CompatGx::gxMax0(lx * L.dir[0] + ly * L.dir[1] + lz * L.dir[2]);
    const float cosAttn = Platform::CompatGx::gxMax0(L.a[0] + L.a[1] * cosA + L.a[2] * cosA * cosA);
    const float distAttn = L.k[0] + L.k[1] * dist + L.k[2] * dist2;
    const float attn = (distAttn != 0.0f) ? cosAttn / distAttn : 0.0f;
    return attn * Platform::CompatGx::gxMax0(ndl);
}

inline float shadeComp(int amb, int mat, float d0, float d1, float c0, float c1) {
    int lacc = amb;
    lacc += Platform::CompatGx::gxFastRound(d0 * c0);
    lacc += Platform::CompatGx::gxFastRound(d1 * c1);
    lacc = Platform::CompatGx::gxMul255(mat, Platform::CompatGx::gxClamp255(lacc));
    return lacc / 255.0f;
}

// Indexed F32 pos/nrm/tex0 + RGBA8 color, two MTX2x4 texgens from TEX0, two spot
// lights, both channels fed from registers. Same numbers as the general path.
LUMA_NOINLINE void lightTwoSpot(
    const Platform::CompatGx::LightParams& L0, const Platform::CompatGx::LightParams& L1, int a0r,
    int a0g, int a0b, int a0a, int a1r, int a1g, int a1b, int a1a, int m0r, int m0g, int m0b, int m0a,
    int m1r, int m1g, int m1b, int m1a, float c0r, float c0g, float c0b, float c0a, float c1r,
    float c1g, float c1b, float c1a, float pv0, float pv1, float pv2, float nv0, float nv1, float nv2,
    float* o) {
    const float d0 = spotDiffuse(L0, pv0, pv1, pv2, nv0, nv1, nv2);
    const float d1 = spotDiffuse(L1, pv0, pv1, pv2, nv0, nv1, nv2);
    o[0] = shadeComp(a0r, m0r, d0, d1, c0r, c1r);
    o[1] = shadeComp(a0g, m0g, d0, d1, c0g, c1g);
    o[2] = shadeComp(a0b, m0b, d0, d1, c0b, c1b);
    o[3] = shadeComp(a0a, m0a, d0, d1, c0a, c1a);
    o[4] = shadeComp(a1r, m1r, d0, d1, c0r, c1r);
    o[5] = shadeComp(a1g, m1g, d0, d1, c0g, c1g);
    o[6] = shadeComp(a1b, m1b, d0, d1, c0b, c1b);
    o[7] = shadeComp(a1a, m1a, d0, d1, c0a, c1a);
}

void expandTightBody(int nverts, float* draw, float* cap, const std::uint32_t* indices, int slots,
                     int posSlot, int nrmSlot, int clr0Slot, int texSlot, const float* posMtx,
                     const float* nrmMtx, const Platform::CompatGx::LightingInputs& lin,
                     const float m0[12], const float m1[12]) {
    const FastSlot& sp = sFastSlot[posSlot];
    const FastSlot& sn = sFastSlot[nrmSlot];
    const FastSlot& sc = sFastSlot[clr0Slot];
    const FastSlot& st = sFastSlot[texSlot];
    const auto& L0 = lin.lights[0];
    const auto& L1 = lin.lights[1];
    const float c0r = L0.color[0], c0g = L0.color[1], c0b = L0.color[2], c0a = L0.color[3];
    const float c1r = L1.color[0], c1g = L1.color[1], c1b = L1.color[2], c1a = L1.color[3];
    const int a0r = lin.amb[0][0], a0g = lin.amb[0][1], a0b = lin.amb[0][2], a0a = lin.amb[0][3];
    const int a1r = lin.amb[1][0], a1g = lin.amb[1][1], a1b = lin.amb[1][2], a1a = lin.amb[1][3];
    const int m0r = lin.mat[0][0], m0g = lin.mat[0][1], m0b = lin.mat[0][2], m0a = lin.mat[0][3];
    const int m1r = lin.mat[1][0], m1g = lin.mat[1][1], m1b = lin.mat[1][2], m1a = lin.mat[1][3];
    const float pm0 = posMtx[0], pm1 = posMtx[1], pm2 = posMtx[2], pm3 = posMtx[3];
    const float pm4 = posMtx[4], pm5 = posMtx[5], pm6 = posMtx[6], pm7 = posMtx[7];
    const float pm8 = posMtx[8], pm9 = posMtx[9], pm10 = posMtx[10], pm11 = posMtx[11];
    const float nm0 = nrmMtx[0], nm1 = nrmMtx[1], nm2 = nrmMtx[2];
    const float nm3 = nrmMtx[3], nm4 = nrmMtx[4], nm5 = nrmMtx[5];
    const float nm6 = nrmMtx[6], nm7 = nrmMtx[7], nm8 = nrmMtx[8];
    const float t0_0 = m0[0], t0_1 = m0[1], t0_3 = m0[3], t0_4 = m0[4], t0_5 = m0[5], t0_7 = m0[7];
    const float t1_0 = m1[0], t1_1 = m1[1], t1_3 = m1[3], t1_4 = m1[4], t1_5 = m1[5], t1_7 = m1[7];
    for (int v = 0; v < nverts; ++v) {
        const std::uint32_t* vi = indices + static_cast<size_t>(v) * static_cast<size_t>(slots);
        float pf[3], nf[3], tf[2];
        std::memcpy(pf, sp.base + static_cast<size_t>(vi[posSlot]) * static_cast<size_t>(sp.stride), 12);
        std::memcpy(nf, sn.base + static_cast<size_t>(vi[nrmSlot]) * static_cast<size_t>(sn.stride), 12);
        std::memcpy(tf, st.base + static_cast<size_t>(vi[texSlot]) * static_cast<size_t>(st.stride), 8);
        const std::uint8_t* cb = sc.base + static_cast<size_t>(vi[clr0Slot]) * static_cast<size_t>(sc.stride);
        const float cr = cb[0] * (1.0f / 255.0f);
        const float cg = cb[1] * (1.0f / 255.0f);
        const float cbb = cb[2] * (1.0f / 255.0f);
        const float ca = cb[3] * (1.0f / 255.0f);
        const float px = pf[0], py = pf[1], pz = pf[2];
        const float nx = nf[0], ny = nf[1], nz = nf[2];
        const float u0 = t0_0 * tf[0] + t0_1 * tf[1] + t0_3;
        const float v0 = t0_4 * tf[0] + t0_5 * tf[1] + t0_7;
        const float u1 = t1_0 * tf[0] + t1_1 * tf[1] + t1_3;
        const float vv1 = t1_4 * tf[0] + t1_5 * tf[1] + t1_7;
        float* cout = cap + static_cast<size_t>(v) * 12;
        cout[0] = px; cout[1] = py; cout[2] = pz;
        cout[3] = nx; cout[4] = ny; cout[5] = nz;
        cout[6] = cr; cout[7] = cg; cout[8] = cbb; cout[9] = ca;
        cout[10] = u0; cout[11] = v0;
        if (draw == nullptr) continue;
        const float pv0 = px * pm0 + py * pm1 + pz * pm2 + pm3;
        const float pv1 = px * pm4 + py * pm5 + pz * pm6 + pm7;
        const float pv2 = px * pm8 + py * pm9 + pz * pm10 + pm11;
        float nv0 = nx * nm0 + ny * nm1 + nz * nm2;
        float nv1 = nx * nm3 + ny * nm4 + nz * nm5;
        float nv2 = nx * nm6 + ny * nm7 + nz * nm8;
        const float nLenSq = nv0 * nv0 + nv1 * nv1 + nv2 * nv2;
        if (nLenSq > 0.0f) {
            const float inv = 1.0f / std::sqrt(nLenSq);
            nv0 *= inv; nv1 *= inv; nv2 *= inv;
        }
        float* o = draw + static_cast<size_t>(v) * 35;
        o[0] = px; o[1] = py; o[2] = pz;
        lightTwoSpot(L0, L1, a0r, a0g, a0b, a0a, a1r, a1g, a1b, a1a, m0r, m0g, m0b, m0a, m1r, m1g,
                     m1b, m1a, c0r, c0g, c0b, c0a, c1r, c1g, c1b, c1a, pv0, pv1, pv2, nv0, nv1, nv2,
                     o + 3);
        o[11] = u0; o[12] = v0; o[13] = 1.0f;
        o[14] = u1; o[15] = vv1; o[16] = 1.0f;
        o[17] = 0; o[18] = 0; o[19] = 1;
        o[20] = 0; o[21] = 0; o[22] = 1;
        o[23] = 0; o[24] = 0; o[25] = 1;
        o[26] = 0; o[27] = 0; o[28] = 1;
        o[29] = 0; o[30] = 0; o[31] = 1;
        o[32] = 0; o[33] = 0; o[34] = 1;
    }
}

bool expandTightStrip(int nverts, bool render, std::vector<float>& drawData) {
    auto fail = [](const char*) { return false; };
    if (sFastSlots < 3 || sFastSlots > 6) {
        return fail("slotcount");
    }
    int posSlot = -1, nrmSlot = -1, clr0Slot = -1;
    int texSlot[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int ntex = 0;
    for (int i = 0; i < sFastSlots; ++i) {
        const FastSlot& f = sFastSlot[i];
        if (f.attr == GX_VA_POS) {
            if (f.type != GX_F32 || f.frac != 0 || f.comps != 3) return fail("pos");
            posSlot = i;
        } else if (f.attr == GX_VA_NRM) {
            if (f.type != GX_F32 || f.frac != 0 || f.comps != 3) return fail("nrm");
            nrmSlot = i;
        } else if (f.attr == GX_VA_CLR0) {
            if (!f.isColor || f.comps != 4) return fail("clr");
            clr0Slot = i;
        } else if (f.attr == GX_VA_CLR1) {
            return fail("clr1");
        } else if (f.attr >= GX_VA_TEX0 && f.attr <= GX_VA_TEX3) {
            if (f.type != GX_F32 || f.frac != 0 || f.comps != 2) return fail("tex");
            texSlot[f.attr - GX_VA_TEX0] = i;
            ++ntex;
        } else {
            return fail("attr");
        }
    }
    if (posSlot < 0 || nrmSlot < 0) return fail("missing");
    if (sFastHasColor) {
        if (sFastColorAttr != GX_VA_CLR0 || sFastColorComps != 4 || clr0Slot >= 0) return fail("dcolor");
    } else if (clr0Slot < 0) {
        // color-less strips stay on the general path (rare; not the planets)
        return fail("nocolor");
    }
    int capStride = 0;
    CapOp caps[8];
    int ncap = 0;
    if (static_cast<int>(sVcdOrder.size()) > 8) return fail("vcdsize");
    for (const auto& slot : sVcdOrder) {
        CapOp& c = caps[ncap];
        c.comps = slot.comps;
        if (slot.attr == GX_VA_POS) c.kind = 0;
        else if (slot.attr == GX_VA_NRM) c.kind = 1;
        else if (slot.attr == GX_VA_CLR0) c.kind = 2;
        else if (slot.attr >= GX_VA_TEX0 && slot.attr <= GX_VA_TEX3) c.kind = 4 + (slot.attr - GX_VA_TEX0);
        else return fail("vcdattr");
        capStride += slot.comps;
        ++ncap;
    }
    TgOp tgs[4];
    int ntg = 0;
    for (int c = 0; c < 8; ++c) {
        const Platform::CompatGx::TexGenUnit& g = sTexGenUnits[c];
        if (g.write == 0) continue;
        if (ntg >= 4) return fail("ntg");
        TgOp& t = tgs[ntg++];
        t.dst = c;
        t.proj3 = g.proj3;
        t.useMtx = g.useMtx;
        std::memcpy(t.m, g.m, sizeof(t.m));
        if (g.srcAttr == GX_VA_POS) t.src = -1;
        else if (g.srcAttr >= GX_VA_TEX0 && g.srcAttr <= GX_VA_TEX3) t.src = g.srcAttr - GX_VA_TEX0;
        else return fail("tgsrc");
    }
    const Platform::CompatGx::LightingInputs lin = Platform::CompatGx::lightingInputs();
    int uniAttn = 0, uniDiff = 0;
    unsigned uniMask = 0;
    if (!Platform::CompatGx::uniformLighting(lin.chan, &uniAttn, &uniDiff, &uniMask)) return fail("uni");
    if (uniMask != 3 || uniAttn != 1 || uniDiff != 2) return fail("mask");
    sVertexData.resize(static_cast<size_t>(nverts) * static_cast<size_t>(capStride));
    sVertexStride = capStride;
    float* draw = nullptr;
    if (render) {
        drawData.resize(static_cast<size_t>(nverts) * 35);
        draw = drawData.data();
    }
    // Only the baked layout: indexed RGBA color, one F32 tex, two MTX2x4 texgens
    // from TEX0, both channels enabled from registers, lights 0 and 1.
    if (ntex != 1 || ntg != 2 || texSlot[0] < 0 || tgs[0].src != 0 || tgs[1].src != 0 ||
        tgs[0].useMtx == 0 || tgs[1].useMtx == 0 || tgs[0].proj3 != 0 || tgs[1].proj3 != 0 ||
        capStride != 12 || clr0Slot < 0) {
        return fail("layout");
    }
    for (int j = 0; j < 4; ++j) {
        if (lin.chan[j].enable == 0 || lin.chan[j].matSrc != 0 || lin.chan[j].ambSrc != 0) {
            return fail("chan");
        }
    }
    expandTightBody(nverts, draw, sVertexData.data(), sFastIndices.data(), sFastSlots, posSlot,
                    nrmSlot, clr0Slot, texSlot[0], Platform::CompatGx::currentPosMtx(),
                    Platform::CompatGx::currentNrmMtx(), lin, tgs[0].m, tgs[1].m);
    return true;
}

void expandFastStrip(int nverts, bool render, std::vector<float>& drawData) {
    if (expandTightStrip(nverts, render, drawData)) {
        return;
    }
    int capStride = 0;
    CapOp caps[16];
    int ncap = 0;
    for (const auto& slot : sVcdOrder) {
        CapOp& c = caps[ncap++];
        c.comps = slot.comps;
        if (slot.attr == GX_VA_POS) {
            c.kind = 0;
        } else if (slot.attr == GX_VA_NRM) {
            c.kind = 1;
        } else if (slot.attr == GX_VA_CLR0) {
            c.kind = 2;
        } else if (slot.attr == GX_VA_CLR1) {
            c.kind = 3;
        } else if (slot.attr >= GX_VA_TEX0 && slot.attr <= GX_VA_TEX7) {
            c.kind = 4 + (slot.attr - GX_VA_TEX0);
        } else {
            c.kind = -1;
        }
        capStride += slot.comps;
    }
    int posSlot = -1, nrmSlot = -1, clr0Slot = -1, clr1Slot = -1;
    int texSlot[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (int i = 0; i < sFastSlots; ++i) {
        if (sFastSlot[i].attr == GX_VA_POS) {
            posSlot = i;
        } else if (sFastSlot[i].attr == GX_VA_NRM) {
            nrmSlot = i;
        } else if (sFastSlot[i].attr == GX_VA_CLR0) {
            clr0Slot = i;
        } else if (sFastSlot[i].attr == GX_VA_CLR1) {
            clr1Slot = i;
        } else if (sFastSlot[i].attr >= GX_VA_TEX0 && sFastSlot[i].attr <= GX_VA_TEX7) {
            texSlot[sFastSlot[i].attr - GX_VA_TEX0] = i;
        }
    }
    sVertexData.resize(static_cast<size_t>(nverts) * static_cast<size_t>(capStride));
    sVertexStride = capStride;
    constexpr int kStride = 35;
    if (render) {
        drawData.resize(static_cast<size_t>(nverts) * kStride);
    }
    const float* posMtx = Platform::CompatGx::currentPosMtx();
    const float* nrmMtx = Platform::CompatGx::currentNrmMtx();
    const Platform::CompatGx::LightingInputs lin = Platform::CompatGx::lightingInputs();
    int uniAttn = 0;
    int uniDiff = 0;
    unsigned uniMask = 0;
    const bool uniLit = Platform::CompatGx::uniformLighting(lin.chan, &uniAttn, &uniDiff, &uniMask);
    TgOp tgs[8];
    int ntg = 0;
    for (int c = 0; c < 8; ++c) {
        const Platform::CompatGx::TexGenUnit& g = sTexGenUnits[c];
        if (g.write == 0) {
            continue;
        }
        TgOp& t = tgs[ntg++];
        t.dst = c;
        t.proj3 = g.proj3;
        t.useMtx = g.useMtx;
        std::memcpy(t.m, g.m, sizeof(t.m));
        if (g.srcAttr == GX_VA_POS) {
            t.src = -1;
        } else if (g.srcAttr >= GX_VA_TEX0 && g.srcAttr <= GX_VA_TEX7) {
            t.src = g.srcAttr - GX_VA_TEX0;
        } else {
            t.src = -2;
        }
    }
    const int posComps = (posSlot >= 0) ? sFastSlot[posSlot].comps : 0;
    const int nrmComps = (nrmSlot >= 0) ? sFastSlot[nrmSlot].comps : 0;
    float* cap = sVertexData.data();
    float* draw = render ? drawData.data() : nullptr;
    const std::uint32_t* indices = sFastIndices.data();
    const int slots = sFastSlots;
    if (!uniLit) {
        expandHotBody<2, 0, false>(nverts, render, draw, cap, capStride, indices, slots, posSlot,
                                   nrmSlot, clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx,
                                   nrmMtx, lin, uniMask, posComps, nrmComps);
        return;
    }
    switch (uniAttn * 4 + uniDiff) {
    case 0 * 4 + 0:
        expandHotBody<0, 0, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    case 0 * 4 + 1:
        expandHotBody<0, 1, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    case 0 * 4 + 2:
        expandHotBody<0, 2, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    case 1 * 4 + 0:
        expandHotBody<1, 0, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    case 1 * 4 + 1:
        expandHotBody<1, 1, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    case 1 * 4 + 2:
        expandHotBody<1, 2, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    case 2 * 4 + 1:
        expandHotBody<2, 1, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    case 2 * 4 + 2:
        expandHotBody<2, 2, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    default:
        expandHotBody<2, 0, true>(nverts, render, draw, cap, capStride, indices, slots, posSlot, nrmSlot,
                                  clr0Slot, clr1Slot, texSlot, caps, ncap, tgs, ntg, posMtx, nrmMtx, lin,
                                  uniMask, posComps, nrmComps);
        break;
    }
}

void flushFastIndexImpl();

bool noteFastColorByte(std::uint8_t b) {
    if (!sFastIndex || !sFastHasColor) {
        return false;
    }
    if (sFastColorCount < static_cast<int>(sFastColorBytes.size())) {
        sFastColorBytes[static_cast<size_t>(sFastColorCount++)] = b;
    }
    if (sFastColorCount >= static_cast<int>(sFastColorBytes.size()) &&
        gGxIndexCapture.count >= gGxIndexCapture.expected) {
        flushFastIndexImpl();
    }
    return true;
}

void flushFastIndexImpl() {
    if (!sFastIndex) {
        return;
    }
    if (gGxIndexCapture.count < gGxIndexCapture.expected) {
        return;
    }
    if (sFastHasColor && sFastColorCount < static_cast<int>(sFastColorBytes.size())) {
        return;
    }
    gGxIndexCapture.active = false;
    sFastCount = gGxIndexCapture.count;
    sNvertsDone = (sFastSlots > 0) ? sFastCount / sFastSlots : 0;
    flushDraw();
}

void flushDraw() {
    if (sFastIndex) {
        gGxIndexCapture.active = false;
        if (sFastCount == 0) {
            sFastCount = gGxIndexCapture.count;
        }
        const int slots = sFastSlots;
        const int n = (slots > 0) ? sFastCount / slots : 0;
        const bool render = Platform::Renderer::instance().isInitialized() &&
                            Platform::Renderer::instance().inPass();
        if (n > 0) {
            expandFastStrip(n, render, sFastDrawOut);
            sFastDrawReady = render;
        }
        sFastIndex = false;
        sFastCount = 0;
        gGxIndexCapture.count = 0;
        sNvertsDone = n;
    }
    if (sVertexData.empty()) {
        sInBegin = false;
        sNvertsDone = 0;
        sFastDrawReady = false;
        return;
    }
    logTcSpan();
    resetTcSpan();

    const int nverts = static_cast<int>(sNvertsDone);
    sInBegin = false;
    sNvertsDone = 0;

    // --- locate the VCD slots (source layout) -------------------------------
    int stride = 0;
    int posOffset = -1, posComps = 0;
    int nrmOffset = -1, nrmComps = 0;
    int clr0Offset = -1, clr1Offset = -1;
    int texOffset[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    for (const auto& slot : sVcdOrder) {
        if (slot.attr == GX_VA_POS) {
            posOffset = stride;
            posComps = slot.comps;
        } else if (slot.attr == GX_VA_NRM) {
            nrmOffset = stride;
            nrmComps = slot.comps;
        } else if (slot.attr == GX_VA_CLR0) {
            clr0Offset = stride;
        } else if (slot.attr == GX_VA_CLR1) {
            clr1Offset = stride;
        } else if (slot.attr >= GX_VA_TEX0 && slot.attr <= GX_VA_TEX7) {
            texOffset[slot.attr - GX_VA_TEX0] = stride;
        }
        stride += slot.comps;
    }
    sVertexStride = stride;
    if (posOffset < 0) {
        PL_LOG_WARN("gx", "primitive without position attribute — dropped");
        sVertexData.clear();
        sVertexTexCoords.clear();
        sVertexTexQ.clear();
        return;
    }

    // Debug snapshot (tests/dump): keep a copy of the captured stream, with
    // the stride of THIS primitive (not a stale value from a previous flush).
    // Taken BEFORE the renderer early-out so headless unit tests can inspect
    // the captured vertices.
    sDebugData = sVertexData;
    sDebugStride = sVertexStride;

    if (!Platform::Renderer::instance().isInitialized()) {
        sVertexData.clear();
        sVertexTexCoords.clear();
        sVertexTexQ.clear();
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();

    // PC_PORT (M9.5.4 v8): the Z-texture op has no host equivalent (the fragment
    // depth cannot come from a texture). The in-tree users (MainLoopFramework::
    // clearEfb, DrawUtil's clearZBuffer) draw a full-screen quad whose Z24X8
    // texel is the far plane — i.e. a depth clear, which the pass already did —
    // and the host patches skip that quad. Warn once if anything else slips
    // through: it would write its own rasterized depth instead.
    if (Platform::CompatGx::zTexReplaceActive()) {
        static bool sWarnedZTex = false;
        if (!sWarnedZTex) {
            sWarnedZTex = true;
            PL_LOG_WARN("gx", "flushDraw: primitive drawn with GXSetZTexture(GX_ZT_REPLACE) — "
                              "the host writes rasterized depth, not the Z texture");
        }
    }

    // PC_PORT (M9.4): draws outside an active pass have no command buffer —
    // e.g. the boot frame loop skipped beginFrame this frame (swapchain
    // out-of-date) or a headless test emits geometry without a pass. Drop the
    // primitive like the uninitialized case above (the debug snapshot was
    // already taken, so capture tests still see the vertices).
    if (!r.inPass()) {
        sVertexData.clear();
        sVertexTexCoords.clear();
        sVertexTexQ.clear();
        return;
    }

    // --- expand to the fixed TEV vertex layout (35 floats) ------------------
    // pos(3) clr0(4) clr1(4) tex0..7(3 each: s, t, q). Missing attributes are filled:
    // colors default per channel state (M5.7a: with the channel disabled the
    // GX default SRC_VTX material keeps the vertex color; with SRC_REG it is
    // the material color — computed below), normals default to (0,0,1) like
    // the GX vertex processor, texcoords (0,0). One vertex format serves the
    // whole GX path, so the TEV pipeline is cache-friendly.
    //
    // M5.7a: the position/normal are transformed to view space by the current
    // pos/nrm matrices (GX row-vector: out[r] = v * m[r] + m[r][3]) and the
    // channel lighting replaces the vertex colors that the TEV shader reads as
    // RASC/APREV. The layout position stays in MODEL space — the shader
    // applies the MVP (proj * posMtx, transposed for the column-vector
    // multiply; see buildMvp).
    const float* posMtx = Platform::CompatGx::currentPosMtx();
    const float* nrmMtx = Platform::CompatGx::currentNrmMtx();
    constexpr int kFixedStride = 35;   // pos(3) clr0(4) clr1(4) tex0..7(3: s,t,q)
    const auto buildVertex = [&](int srcIdx, float* out) {
        const float* src = sVertexData.data() + static_cast<size_t>(srcIdx) * stride;
        out[0] = (posOffset >= 0) ? src[posOffset] : 0.0f;
        out[1] = (posOffset >= 0) ? src[posOffset + 1] : 0.0f;
        out[2] = (posOffset >= 0 && posComps == 3) ? src[posOffset + 2] : 0.0f;
        float clr0[4], clr1[4];
        for (int c = 0; c < 4; ++c) {
            clr0[c] = (clr0Offset >= 0) ? src[clr0Offset + c] : 1.0f;
            clr1[c] = (clr1Offset >= 0) ? src[clr1Offset + c] : 1.0f;
        }
        // View-space position (row-vector 3x4, w = 1).
        const float px = out[0], py = out[1], pz = out[2];
        const float posView[3] = {
            px * posMtx[0] + py * posMtx[1] + pz * posMtx[2] + posMtx[3],
            px * posMtx[4] + py * posMtx[5] + pz * posMtx[6] + posMtx[7],
            px * posMtx[8] + py * posMtx[9] + pz * posMtx[10] + posMtx[11],
        };
        // View-space normal (row-vector 3x3, normalized). Missing normal
        // defaults to (0,0,1) (GX vertex-processor default).
        float nx = 0.0f, ny = 0.0f, nz = 1.0f;
        if (nrmOffset >= 0) {
            nx = src[nrmOffset];
            ny = src[nrmOffset + 1];
            nz = (nrmComps == 3) ? src[nrmOffset + 2] : 1.0f;
        }
        float nrmView[3] = {
            nx * nrmMtx[0] + ny * nrmMtx[1] + nz * nrmMtx[2],
            nx * nrmMtx[3] + ny * nrmMtx[4] + nz * nrmMtx[5],
            nx * nrmMtx[6] + ny * nrmMtx[7] + nz * nrmMtx[8],
        };
        const float nLenSq =
            nrmView[0] * nrmView[0] + nrmView[1] * nrmView[1] + nrmView[2] * nrmView[2];
        if (nLenSq > 0.0f) {
            const float inv = 1.0f / std::sqrt(nLenSq);
            nrmView[0] *= inv;
            nrmView[1] *= inv;
            nrmView[2] *= inv;
        }
        float lit0[4], lit1[4];
        Platform::CompatGx::applyChannelLighting(posView, nrmView, clr0, clr1, lit0, lit1);
        for (int c = 0; c < 4; ++c) {
            out[3 + c] = lit0[c];
            out[7 + c] = lit1[c];
        }
        // Generated texcoords (all eight), with their projective component:
        // the GX vertex processor produces them for every vertex regardless of
        // which attributes the shape stores, and the texture unit divides by q
        // per pixel (gx_tev_frag.frag).
        const float* tc = sVertexTexCoords.data() + static_cast<size_t>(srcIdx) * 16;
        const float* tq = sVertexTexQ.data() + static_cast<size_t>(srcIdx) * 8;
        for (int t = 0; t < 8; ++t) {
            out[11 + 3 * t] = tc[2 * t];
            out[12 + 3 * t] = tc[2 * t + 1];
            out[13 + 3 * t] = tq[t];
        }
    };
    std::vector<float> drawData;
    // M9.8 expanded STRIP/FAN into flat triangle lists so the M9.7 batcher
    // could coalesce them (the fileselect field — planets + character heads —
    // is strip-based; ~5000 loose draws/frame had pegged it at 6 FPS). That
    // fixed the draw count but TRIPLED the vertex throughput: an N-vertex
    // strip emits 3(N-2) list vertices, each paying the full buildVertex
    // transform + lighting + 140-byte write. The user's log confirmed it:
    // draws dropped 5009 -> 92 but cpu-render only 160 -> 105 ms (10 FPS) —
    // the cost had moved to per-vertex work.
    // M9.9: strips stay NATIVE strips again (buildVertex once per source
    // vertex) and coalesce anyway through degenerate-vertex stitching:
    // appending [lastRunVert (x1 or x2), firstNewVert] between two strips
    // emits only zero-area triangles at the seam while the run stays one
    // TriangleStrip draw. Parity: the hardware flips the winding of odd-indexed
    // strip triangles, so the junction inserts 2 vertices when the running
    // vertex count is even and 3 when it is odd — the first real triangle of
    // the appended strip always lands on an EVEN index and keeps its winding
    // (back-face culling unchanged). QUADS/TRIANGLES/FANS remain lists.
    // M9.10 perf: buildVertex writes DIRECTLY into the (exactly sized)
    // drawData buffer. The old code built each vertex in a local and
    // insert()ed it — the iterator-range insert machinery cost ~170
    // instructions per vertex on the planets. The resize() zero-fill is
    // fully overwritten below and costs one linear pass.
    bool triList = false;   // flat TriangleList, coalescible with lists
    bool triStrip = false;  // native TriangleStrip, coalescible with strips
    if (sFastDrawReady) {
        drawData.swap(sFastDrawOut);
        sFastDrawReady = false;
        triStrip = true;
    } else if (sPrimitive == GX_QUADS && (nverts % 4) == 0) {
        drawData.resize(static_cast<size_t>(nverts / 4 * 6) * kFixedStride);
        float* dst = drawData.data();
        for (int q = 0; q < nverts; q += 4) {
            for (int idx : {0, 1, 2, 0, 2, 3}) {
                buildVertex(q + idx, dst);
                dst += kFixedStride;
            }
        }
        triList = true;
    } else if (sPrimitive == GX_TRIANGLESTRIP && nverts >= 3) {
        drawData.resize(static_cast<size_t>(nverts) * kFixedStride);
        float* dst = drawData.data();
        for (int i = 0; i < nverts; ++i) {
            buildVertex(i, dst);
            dst += kFixedStride;
        }
        triStrip = true;
    } else if (sPrimitive == GX_TRIANGLEFAN && nverts >= 3) {
        drawData.resize(static_cast<size_t>((nverts - 2) * 3) * kFixedStride);
        float* dst = drawData.data();
        for (int i = 1; i + 1 < nverts; ++i) {
            for (int idx : {0, i, i + 1}) {
                buildVertex(idx, dst);
                dst += kFixedStride;
            }
        }
        triList = true;
    } else {
        drawData.resize(static_cast<size_t>(nverts) * kFixedStride);
        for (int i = 0; i < nverts; ++i) {
            buildVertex(i, drawData.data() + static_cast<size_t>(i) * kFixedStride);
        }
        // A plain GX_TRIANGLES primitive is already a list (coalescible);
        // LINES / LINESTRIP / POINTS keep their own topology and don't merge.
        triList = (sPrimitive == GX_TRIANGLES);
    }
    sVertexData.clear();
    sVertexTexCoords.clear();
    sVertexTexQ.clear();

    // --- pipeline: universal TEV variant ------------------------------------
    Platform::PipelineDesc desc;
    // M9.9: QUADS/TRIANGLES/FAN arrive as flat triangle lists; STRIP stays a
    // native strip (degenerate-stitched into the batch, see above). Only the
    // line/point primitives keep their own topology.
    desc.topology = triStrip
                        ? Platform::PrimitiveTopology::TriangleStrip
                        : (sPrimitive == GX_LINES)
                              ? Platform::PrimitiveTopology::LineList
                              : (sPrimitive == GX_LINESTRIP)
                                    ? Platform::PrimitiveTopology::LineStrip
                                    : (sPrimitive == GX_POINTS)
                                          ? Platform::PrimitiveTopology::PointList
                                          : Platform::PrimitiveTopology::TriangleList;
    const Platform::PrimitiveTopology primTopo = desc.topology;
    desc.vertexLayout.stride = kFixedStride * sizeof(float);
    desc.vertexLayout.attribs = {
        {0, 0, Platform::VertexFormat::R32G32B32_SFLOAT},
        {1, 12, Platform::VertexFormat::R32G32B32A32_SFLOAT},
        {2, 28, Platform::VertexFormat::R32G32B32A32_SFLOAT},
        {3, 44, Platform::VertexFormat::R32G32B32_SFLOAT},
        {4, 56, Platform::VertexFormat::R32G32B32_SFLOAT},
        {5, 68, Platform::VertexFormat::R32G32B32_SFLOAT},
        {6, 80, Platform::VertexFormat::R32G32B32_SFLOAT},
        {7, 92, Platform::VertexFormat::R32G32B32_SFLOAT},
        {8, 104, Platform::VertexFormat::R32G32B32_SFLOAT},
        {9, 116, Platform::VertexFormat::R32G32B32_SFLOAT},
        {10, 128, Platform::VertexFormat::R32G32B32_SFLOAT},
    };
    // M5.5: map the pixel-engine state mirror onto the pipeline desc.
    desc.cullMode = cullModeFromGx(sCullMode);
    desc.logicOpEnable = (sBlendMode == GX_BM_LOGIC);
    desc.logicOp = static_cast<Platform::LogicOp>(sBlendLogicOp);
    desc.blendEnable = ((sBlendMode & 1) != 0);  // BLEND and SUBTRACT
    desc.srcBlendFactor = blendFactorFromGx(sBlendSrc);
    desc.dstBlendFactor = blendFactorFromGx(sBlendDst);
    desc.blendOp = (sBlendMode == GX_BM_SUBTRACT) ? Platform::BlendOp::ReverseSubtract
                                                  : Platform::BlendOp::Add;
    desc.dstAlphaEnable = sDstAlphaEnable;
    // (the constant alpha itself is dynamic state — set after bindPipeline)
    desc.depthTest = sZTest;
    desc.depthWrite = sZWrite;
    desc.depthCompare = compareFromGx(sZFunc);
    desc.colorWrite = sColorUpdate;
    desc.alphaWrite = sAlphaUpdate;
    desc.colorFormat = r.passColorFormat();
    desc.depthFormat = r.passDepthFormat();
    desc.textureCount = 8;
    desc.fragmentUbo = true;
    desc.vertSpv = kGxTevVertSpv;
    desc.vertSpvSize = sizeof(kGxTevVertSpv);
    desc.fragSpv = kGxTevFragSpv;
    desc.fragSpvSize = sizeof(kGxTevFragSpv);

    Platform::PipelineHandle pipe = r.getOrCreatePipeline(desc);
    if (!pipe) {
        return;
    }

    // --- textures: TEXMAP0..7 or the white fallback --------------------------
    void* texRaw[8] = {};
    void* samRaw[8] = {};
    Platform::CompatGx::getTexMaps(texRaw, samRaw);
    Platform::TextureHandle tex[8] = {};
    Platform::SamplerHandle sam[8] = {};
    for (int i = 0; i < 8; ++i) {
        tex[i] = static_cast<Platform::TextureHandle>(texRaw[i]);
        sam[i] = static_cast<Platform::SamplerHandle>(samRaw[i]);
        if (!tex[i] || !sam[i]) {
            ensureWhiteFallback();
            tex[i] = sWhiteTex;
            sam[i] = sWhiteSam;
        }
    }

    // --- TEV constants (fragment UBO) ----------------------------------------
    Platform::CompatGx::TevUboData ubo;
    Platform::CompatGx::buildTevUbo(ubo);
    // M5.7b: the indirect warp normalizes its offset by the direct map's texel
    // size — fill the texDims field (the UBO builder doesn't know the bound
    // TEXMAPs).
    {
        float dims[8][2] = {};
        Platform::CompatGx::getTexMapDims(dims);
        for (int t = 0; t < 8; ++t) {
            ubo.texDims[t][0] = dims[t][0];
            ubo.texDims[t][1] = dims[t][1];
        }
    }

    // --- MVP (push constant) --------------------------------------------------
    float mvp[16];
    if (sHasProjection) {
        // M5.7a: clip = posView * proj = pos * (posMtx * proj); the shader
        // multiplies column-vectors, so push the transpose (see buildMvp).
        // With the identity pos matrix this is exactly proj^T — the first
        // real projection exercised by the offscreen test.
        Platform::CompatGx::buildMvp(Platform::CompatGx::currentPosMtx(), sProjection,
                                     mvp);
    } else {
        std::memset(mvp, 0, sizeof(mvp));
        mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f;
    }
    // M9.5.3c: GX viewport depth transform. GX maps visible ortho geometry to
    // clip z in [-1, 0] and the viewport stage remaps clip z [-1,1] into the
    // depth range set by GXSetViewport's nearZ/farZ (virtually always 0..1):
    //   z_vk = (z_clip + w) * (farZ - nearZ) / 2 + nearZ * w
    // The host shader passes clip z straight through and Vulkan's depth range
    // is [0,1], so without this fold every layout/2D quad (negative depth)
    // disappeared — the boot's black screen. Fold it into row 2 of the
    // uploaded matrix (column-major flat: row r lives at mvp[c*4+r]).
    {
        const float a = (sViewportFarZ - sViewportNearZ) * 0.5f;
        const float b = a + sViewportNearZ;
        for (int c = 0; c < 4; ++c) {
            mvp[c * 4 + 2] = a * mvp[c * 4 + 2] + b * mvp[c * 4 + 3];
        }
    }
    // PC_PORT (M9.5.3c): NDC Y-flip. GX/GL clip space has Y pointing UP, but
    // Vulkan's NDC points DOWN and gx_tev_vert.vert passes clip.y straight
    // through — every GX primitive rendered vertically flipped (the user's
    // strap screen appeared upside down; pinned by lyt_draw_lands_on_efb's
    // green top-marker, which rasterized at the EFB BOTTOM rows). Negate row
    // 1 of the uploaded matrix here — the single point every GX path
    // (immediate + display lists) funnels through — so the EFB stores
    // console orientation (row 0 = top), keeping GXCopyDisp's blit,
    // readRenderTarget probes and GXCopyTex readbacks all console-faithful.
    // NOTE: a partial GXSetViewport's y offset is bottom-left on GX (GL
    // convention); the game only ever uses full-screen viewports, whose
    // mapping is unaffected by the origin. If a partial viewport ever shows
    // misplaced, flip its y here too: y_vk = y_gx_bottom_left_space.
    {
        for (int c = 0; c < 4; ++c) {
            mvp[c * 4 + 1] = -mvp[c * 4 + 1];
        }
    }

    // M9.7: capture this primitive's full render state as a batch candidate and
    // coalesce it into the pending run when the state is bit-identical (see the
    // batching notes above flushPendingBatch). The actual bind+draw is deferred
    // to flushPendingBatch (state change here, or endPass via the hook).
    PendingBatch cand;
    cand.pipe = pipe;
    std::memcpy(cand.tex, tex, sizeof(tex));
    std::memcpy(cand.sam, sam, sizeof(sam));
    cand.ubo = ubo;
    std::memcpy(cand.mvp, mvp, sizeof(mvp));
    cand.vp[0] = sViewportX;
    cand.vp[1] = sViewportY;
    cand.vp[2] = sViewportW;
    cand.vp[3] = sViewportH;
    cand.scissorSet = sScissorSet;
    if (sScissorSet) {
        cand.scissor[0] = sScissor[0];
        cand.scissor[1] = sScissor[1];
        cand.scissor[2] = sScissor[2];
        cand.scissor[3] = sScissor[3];
    }
    cand.blendAlpha = sDstAlphaValue;
    cand.topo = primTopo;

    // M9.9: triangle lists AND triangle strips coalesce (strips via the
    // degenerate junction below); line/point primitives draw alone.
    const bool mergeable = triList || triStrip;
    const bool canMerge = mergeable && sBatch.valid && batchMatches(sBatch, cand);

    // M9.9: seam between two coalesced strips — repeat the run's last vertex
    // (twice when the running vertex count is odd, so the appended strip's
    // first real triangle keeps an EVEN index and its winding) plus the new
    // strip's first vertex. Every triangle touching a junction vertex is
    // zero-area, so nothing rasterizes at the seam. See the expansion notes.
    if (canMerge && triStrip) {
        static_assert(kBatchFixedStride == 35, "junction stride must match kFixedStride");
        float junction[3 * kBatchFixedStride];
        int jn = 0;
        std::memcpy(junction + jn * kBatchFixedStride, sLastVertOut,
                    kBatchFixedStride * sizeof(float));
        ++jn;
        if ((sBatch.vertexCount & 1u) != 0u) {
            std::memcpy(junction + jn * kBatchFixedStride, sLastVertOut,
                        kBatchFixedStride * sizeof(float));
            ++jn;
        }
        std::memcpy(junction + jn * kBatchFixedStride, drawData.data(),
                    kBatchFixedStride * sizeof(float));
        ++jn;
        drawData.insert(drawData.begin(), junction, junction + jn * kBatchFixedStride);
    }

    // M5.2: append to the shared dynamic vertex buffer instead of allocating a
    // per-primitive buffer. Grows on demand (old allocations are retired and
    // destroyed at the next endFrame, after the frame fence). Written AFTER
    // the merge decision so a strip junction lands contiguously in the run.
    const uint64_t bytes = drawData.size() * sizeof(float);
    if (!sDynVb) {
        sDynVb = r.createDynamicBuffer(bytes);
        if (!sDynVb) {
            PL_LOG_WARN("gx", "createDynamicBuffer failed — primitive dropped");
            return;
        }
    }
    if (!r.ensureBufferCapacity(sDynVb, sDynUsedBytes + bytes)) {
        PL_LOG_WARN("gx", "ensureBufferCapacity failed — primitive dropped");
        return;
    }
    if (!r.updateDynamicBuffer(sDynVb, sDynUsedBytes, bytes, drawData.data())) {
        PL_LOG_WARN("gx", "updateDynamicBuffer failed — primitive dropped");
        return;
    }

    // M9.7: advance the vertex cursor now and remember where this primitive's
    // vertices begin, so a coalesced run draws the whole contiguous span.
    const uint32_t firstVertex =
        static_cast<uint32_t>(sDynUsedBytes / (static_cast<uint64_t>(kFixedStride) * sizeof(float)));
    sDynUsedBytes += bytes;

    // M9.9: remember this primitive's last vertex for the next strip junction.
    std::memcpy(sLastVertOut, drawData.data() + drawData.size() - kBatchFixedStride,
                kBatchFixedStride * sizeof(float));

    cand.firstVertex = firstVertex;
    cand.vertexCount = static_cast<uint32_t>(drawData.size() / kFixedStride);

    if (canMerge) {
        sBatch.vertexCount += cand.vertexCount;  // extend the pending run
    } else {
        flushPendingBatch();  // emit the previous run (if any)
        sBatch = cand;
        sBatch.valid = true;
        if (!mergeable) {
            flushPendingBatch();
        }
    }

    // Register the end-pass flush hook once so the frame's final run is emitted
    // before the pass closes (covers the game loop, the demo and the tests).
    if (!sEndPassHookSet) {
        r.setEndPassHook([] { flushPendingBatch(); });
        sEndPassHookSet = true;
    }
}

// =============================================================================
// M5.6 — display-list interpreter.
//
// The game's J3D renderer builds display lists with the GD writers
// (GDWriteBPCmd / GDWriteCPCmd / GDWriteXFCmd / GDWrite_f32 — pure memory
// writes into the game's own buffer; no PC interception needed) and replays
// them with GXCallDisplayList. This interpreter walks the console FIFO opcode
// stream byte-exactly (layouts confirmed against Dolphin's OpcodeDecoding):
//
//   0x00 NOP         0x08 CP  (6 bytes)     0x10 XF  (5 + 4*count bytes)
//   0x20..0x38 XF indexed loads (M5.7)      0x40 CALL_DL (9 bytes, align 32)
//   0x44/0x48 no-ops (trace)                0x61 BP  (5 bytes)
//   0x80..0xBF primitive: byte | vat<<0, u16 nverts, then nverts × vertex
//
// State commands update the same mirrors the GXSet* API path drives, so a DL
// carrying baked state (VCD/VAT/texgen/TEV/PE) renders identically whether
// the state was set through the API or baked into the list.
//
// PC limitations (documented in GXCompat.h §M5.6):
//   * CP ARRAY_BASE carries a 32-bit *physical* address in the DL; the vertex
//     arrays are host pointers. The interpreter resolves the physical base
//     through the registry populated by GXSetArray (which registers the same
//     GX_PHY_ADDR value the console would compute); an unresolvable base
//     keeps the last GXSetArray pointer.
//   * 0x40 CALL_DL inside a list carries a 32-bit address that cannot be
//     translated on PC — the game drives nested lists with GXCallDisplayList
//     directly. Warn and skip.
// =============================================================================

// Bounds-checked big-endian reader over a DL.
struct DlReader {
    const u8* p = nullptr;
    const u8* end = nullptr;
    bool ok = true;

    u8 readU8() {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }
    u16 readU16() {
        if (p + 2 > end) { ok = false; return 0; }
        const u16 v = static_cast<u16>((p[0] << 8) | p[1]);
        p += 2;
        return v;
    }
    u32 readU32() {
        if (p + 4 > end) { ok = false; return 0; }
        const u32 v = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
                      (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
        p += 4;
        return v;
    }
    float readF32() {
        const u32 v = readU32();
        float f;
        std::memcpy(&f, &v, sizeof(f));
        return f;
    }
    size_t remaining() const { return static_cast<size_t>(end - p); }
};

// BP write mask (BPMEM_BP_MASK, RID 0xFE): applies to the NEXT BP write only.
// 0xFFFFFF = no masking (the norm — J3D never uses the mask).
uint32_t sBpMask = 0xFFFFFF;

// CP MATINDEX_B (register 0x40): tex matrix indices 4-7. Stored for the
// M5.7b ind stages; nothing consumes it yet.
std::uint32_t sMatIdxB = 0;

// Physical-address registry for vertex arrays: GXSetArray registers
// GX_PHY_ADDR(hostPtr) -> hostPtr; the DL ARRAY_BASE command (which carries
// the physical base) resolves through it. See the M5.6 notes above.
std::map<uint32_t, const u8*> sArrayPhyMap;

// Last XF_TEXn / XF_DUALTEXn values (0x1040/0x1050 + n). The J3D baked format
// writes the two register groups in separate XF loads, so a texgen is applied
// with whichever half arrived last.
std::uint32_t sXfTex[8] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                           0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
std::uint32_t sXfDual[8] = {};

// --- CP stream registers (command 0x08) --------------------------------------

// VCD 2-bit fields map 1:1 to GXAttrType (NONE=0, DIRECT=1, INDEX8=2, INDEX16=3).
GXAttrType vcdAttrType(int field) { return static_cast<GXAttrType>(field); }

void dlApplyVcdLo(std::uint32_t v) {
    // CP_VCD_REG_LO: POS 9, NRM 11, COL0 13, COL1 15 (2 bits each). The
    // matrix-index fields (bits 0-8, for skinning) are stored but not used by
    // rebuildVcd yet (skin weights/matrices arrive with a later milestone).
    sVtxDesc[GX_VA_POS] = vcdAttrType((v >> 9) & 0x3);
    sVtxDesc[GX_VA_NRM] = vcdAttrType((v >> 11) & 0x3);
    sVtxDesc[GX_VA_CLR0] = vcdAttrType((v >> 13) & 0x3);
    sVtxDesc[GX_VA_CLR1] = vcdAttrType((v >> 15) & 0x3);
    for (int i = GX_VA_POS; i <= GX_VA_CLR1; ++i) {
        sVtxDescSet[i] = true;
    }
    rebuildVcd();
}

void dlApplyVcdHi(std::uint32_t v) {
    // CP_VCD_REG_HI: TEX0..TEX7 at shifts 0,2,...,14 (2 bits each).
    for (int t = 0; t < 8; ++t) {
        const int attr = GX_VA_TEX0 + t;
        sVtxDesc[attr] = vcdAttrType((v >> (2 * t)) & 0x3);
        sVtxDescSet[attr] = true;
    }
    rebuildVcd();
}

void setVatAttr(GXVtxFmt fmt, GXAttr attr, int cnt, int type, int frac) {
    if (fmt < GX_VTXFMT0 || fmt > GX_VTXFMT7 || attr < 0 || attr >= GX_VA_MAX_ATTR) {
        return;
    }
    sAttrFmt[fmt][attr] = {static_cast<GXCompCnt>(cnt), static_cast<GXCompType>(type),
                           static_cast<u8>(frac), true};
}

void dlApplyVatA(int fmt, std::uint32_t v) {
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_POS, CP_VAT_REG_A_GET_POSCNT(v),
               CP_VAT_REG_A_GET_POSFMT(v), CP_VAT_REG_A_GET_POSSHFT(v));
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_NRM, CP_VAT_REG_A_GET_NRMCNT(v),
               CP_VAT_REG_A_GET_NRMFMT(v), 0);
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_CLR0, CP_VAT_REG_A_GET_COL0CNT(v),
               CP_VAT_REG_A_GET_COL0FMT(v), 0);
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_CLR1, CP_VAT_REG_A_GET_COL1CNT(v),
               CP_VAT_REG_A_GET_COL1FMT(v), 0);
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX0, CP_VAT_REG_A_GET_TEX0CNT(v),
               CP_VAT_REG_A_GET_TEX0FMT(v), CP_VAT_REG_A_GET_TEX0SHFT(v));
    rebuildVcd();
}

void dlApplyVatB(int fmt, std::uint32_t v) {
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX1, CP_VAT_REG_B_GET_TEX1CNT(v),
               CP_VAT_REG_B_GET_TEX1FMT(v), CP_VAT_REG_B_GET_TEX1SHFT(v));
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX2, CP_VAT_REG_B_GET_TEX2CNT(v),
               CP_VAT_REG_B_GET_TEX2FMT(v), CP_VAT_REG_B_GET_TEX2SHFT(v));
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX3, CP_VAT_REG_B_GET_TEX3CNT(v),
               CP_VAT_REG_B_GET_TEX3FMT(v), CP_VAT_REG_B_GET_TEX3SHFT(v));
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX4, CP_VAT_REG_B_GET_TEX4CNT(v),
               CP_VAT_REG_B_GET_TEX4FMT(v), 0);  // TEX4SHFT lives in group C
    rebuildVcd();
}

void dlApplyVatC(int fmt, std::uint32_t v) {
    // Group C holds TEX4's SHFT only (its CNT/FMT live in group B), so update
    // just the fraction of the existing TEX4 record.
    if (fmt < GX_VTXFMT0 || fmt > GX_VTXFMT7) {
        return;
    }
    sAttrFmt[fmt][GX_VA_TEX4].frac = static_cast<u8>(CP_VAT_REG_C_GET_TEX4SHFT(v));
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX5, CP_VAT_REG_C_GET_TEX5CNT(v),
               CP_VAT_REG_C_GET_TEX5FMT(v), CP_VAT_REG_C_GET_TEX5SHFT(v));
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX6, CP_VAT_REG_C_GET_TEX6CNT(v),
               CP_VAT_REG_C_GET_TEX6FMT(v), CP_VAT_REG_C_GET_TEX6SHFT(v));
    setVatAttr(static_cast<GXVtxFmt>(fmt), GX_VA_TEX7, CP_VAT_REG_C_GET_TEX7CNT(v),
               CP_VAT_REG_C_GET_TEX7FMT(v), CP_VAT_REG_C_GET_TEX7SHFT(v));
    rebuildVcd();
}

void dlApplyArrayBase(int arrayIdx, std::uint32_t phyBase) {
    const int attr = arrayIdx + GX_VA_POS;  // CPArray Position=0 -> GX_VA_POS=9
    if (attr < 0 || attr >= GX_VA_MAX_ATTR) {
        return;
    }
    const auto it = sArrayPhyMap.find(phyBase);
    if (it != sArrayPhyMap.end()) {
        sArrays[attr].base = it->second;
        sArrays[attr].set = (it->second != nullptr);
        PL_LOG_TRACE("gx", "dlApplyArrayBase(attr %d, phy 0x%08X -> %p)", attr, phyBase,
                     static_cast<const void*>(it->second));
    } else {
        // Unresolvable physical base: keep the last GXSetArray pointer (the
        // common PC flow re-asserts arrays through the API before drawing).
        PL_LOG_TRACE("gx", "dlApplyArrayBase: no host mapping for phy 0x%08X (attr %d) "
                     "— keeping GXSetArray base", phyBase, attr);
    }
}

void dlApplyArrayStride(int arrayIdx, std::uint32_t stride) {
    const int attr = arrayIdx + GX_VA_POS;
    if (attr < 0 || attr >= GX_VA_MAX_ATTR) {
        return;
    }
    sArrays[attr].stride = static_cast<u8>(stride & 0xFF);
}

// One CP stream register write (command 0x08, then addr byte + u32 value).
void dlApplyCp(u8 command, std::uint32_t value) {
    const u8 group = command & 0xF0;
    const u8 idx = command & 0x0F;
    switch (group) {
        case 0x30:  // MATINDEX_A: GXSetCurrentMtx's CP image (low 6 bits =
                    // GX_PNMTX index of the pos/nrm matrix pair).
            Platform::CompatGx::dlMatIdxA(value);
            break;
        case 0x40:  // MATINDEX_B: tex matrix indices 4-7 (ind stages, M5.7b).
            sMatIdxB = value;
            break;
        case 0x50: dlApplyVcdLo(value); break;
        case 0x60: dlApplyVcdHi(value); break;
        case 0x70: dlApplyVatA(idx, value); break;
        case 0x80: dlApplyVatB(idx, value); break;
        case 0x90: dlApplyVatC(idx, value); break;
        case 0xA0: dlApplyArrayBase(idx, value); break;
        case 0xB0: dlApplyArrayStride(idx, value); break;
        default:
            PL_LOG_TRACE("gx", "dlApplyCp: unknown CP group 0x%02X (command 0x%02X)",
                         group, command);
            break;
    }
}

// --- XF registers (command 0x10) ---------------------------------------------

void dlApplyXf(std::uint16_t addr, std::uint32_t value) {
    if (addr >= 0x1040 && addr <= 0x1047) {
        const int coord = addr - 0x1040;
        sXfTex[coord] = value;
        Platform::CompatGx::dlApplyXfTexGen(coord, value, sXfDual[coord]);
    } else if (addr >= 0x1050 && addr <= 0x1057) {
        const int coord = addr - 0x1050;
        sXfDual[coord] = value;
        if (sXfTex[coord] != 0xFFFFFFFF) {
            Platform::CompatGx::dlApplyXfTexGen(coord, sXfTex[coord], value);
        }
    } else if (addr < 0x100) {
        // Position matrices (GXLoadPosMtxImm / J3DFifoLoadPosMtxImm: one u32
        // per reg, the bit pattern of a float).
        const float f = std::bit_cast<float>(value);
        Platform::CompatGx::dlXfPosReg(addr, f);
    } else if (addr >= 0x400 && addr < 0x460) {
        // Normal matrices (GXLoadNrmMtxImm writes 0x400+3*id; the DL may
        // carry padding regs past a matrix's 9 — they land in the next slot
        // like on the console, and get overwritten when that matrix loads).
        const float f = std::bit_cast<float>(value);
        Platform::CompatGx::dlXfNrmReg(addr, f);
    } else if (addr >= 0x600 && addr < 0x680) {
        // Lights (GXLoadLightObjImm writes 0x600+idx*0x10, 16 regs).
        const int rel = addr - 0x600;
        Platform::CompatGx::dlLightReg(rel / 16, rel % 16, std::bit_cast<float>(value));
    } else if (addr == 0x1009) {
        Platform::CompatGx::dlSetNumChans(value);
    } else if (addr == 0x100A || addr == 0x100B) {
        Platform::CompatGx::dlAmbColorReg(addr - 0x100A, value);
    } else if (addr == 0x100C || addr == 0x100D) {
        Platform::CompatGx::dlMatColorReg(addr - 0x100C, value);
    } else if (addr >= 0x100E && addr <= 0x1011) {
        // COLOR0/COLOR1/ALPHA0/ALPHA1 channel control (the XF image of
        // GXSetChanCtrl).
        Platform::CompatGx::dlChanCtrlReg(addr - 0x100E, value);
    } else if (addr == 0x1018 || addr == 0x1019) {
        // XF matrix-index registers (GXSetCurrentMtx's XF image; the CP
        // MATINDEX_A path also arrives through command 0x30). MatrixIndexA's
        // low 6 bits select the pos/nrm matrix pair.
        if (addr == 0x1018) {
            Platform::CompatGx::dlMatIdxA(value);
        }
        // MatrixIndexB (tex 4-7) is consumed by M5.7b ind stages.
    } else {
        PL_LOG_TRACE("gx", "dlApplyXf: addr 0x%04X ignored", addr);
    }
}

// --- BP registers (command 0x61) ---------------------------------------------

// GXSetCullMode stores the hardware cull mode in GEN_MODE bits 14-15 with a
// bit-swap (hw = ((mode&1)<<1)|((mode&2)>>1)); invert it back.
GXCullMode cullFromHwMode(int hw) {
    const int mode = ((hw & 1) << 1) | ((hw & 2) >> 1);
    return static_cast<GXCullMode>(mode);
}

void dlApplyBp(u8 rid, std::uint32_t value) {
    switch (rid) {
        case 0x00:  // GEN_MODE: num-tev-stages, num-texgens, cull mode
            Platform::CompatGx::dlApplyGenMode(value);
            sCullMode = cullFromHwMode((value >> 14) & 0x3);
            break;
        case 0x40: {  // ZMODE: test_enable 0, func 1-3, update_enable 4
            sZTest = (value & 0x1) != 0;
            sZFunc = static_cast<GXCompare>((value >> 1) & 0x7);
            sZWrite = ((value >> 4) & 0x1) != 0;
            break;
        }
        case 0x41: {  // CMODE0 / BLENDMODE
            sBlendMode = ((value >> 1) & 0x1) ? GX_BM_LOGIC
                         : ((value >> 11) & 0x1) ? GX_BM_SUBTRACT
                         : ((value >> 0) & 0x1) ? GX_BM_BLEND
                                                : GX_BM_NONE;
            sBlendSrc = static_cast<GXBlendFactor>((value >> 8) & 0x7);
            sBlendDst = static_cast<GXBlendFactor>((value >> 5) & 0x7);
            sBlendLogicOp = static_cast<GXLogicOp>((value >> 12) & 0xF);
            sColorUpdate = ((value >> 3) & 0x1) != 0;
            sAlphaUpdate = ((value >> 4) & 0x1) != 0;
            sDither = ((value >> 2) & 0x1) != 0;
            break;
        }
        case 0x42: {  // CMODE1 / CONSTANTALPHA (GXSetDstAlpha)
            sDstAlphaValue = static_cast<std::int32_t>(value & 0xFF);
            sDstAlphaEnable = ((value >> 8) & 0x1) != 0;
            break;
        }
        default:
            // TEV/PE-family registers (0x28..0x2F TREF, 0xC0..0xDF TEVC/TEVA,
            // 0xE0..0xE7 TEV colors/constants, 0xEE..0xF3 fog + alpha compare,
            // 0xF6..0xFD KSEL + swap tables) and everything else.
            Platform::CompatGx::dlApplyBpTev(rid, value);
            break;
    }
}

// --- the interpreter ----------------------------------------------------------

// Maximum nesting depth for GXCallDisplayList (defensive; the game drives
// nested lists from C++, never through the 0x40 opcode on PC).
inline constexpr int kMaxDlDepth = 8;
int sDlDepth = 0;

// Reads one vertex (nverts handled by the caller): one value per VCD slot —
// DIRECT attrs consume one component per VAT width (colors: 1 byte each,
// RGBA8), indexed attrs one index (width from the VCD type). Values feed the
// same captureWrite/finishVertex machinery the immediate path uses.
void dlReadVertex(DlReader& r) {
    for (const VcdSlot& slot : sVcdOrder) {
        if (slot.source == GX_INDEX8) {
            captureWrite(r.readU8(), 1);
        } else if (slot.source == GX_INDEX16) {
            captureWrite(r.readU16(), 2);
        } else {
            const bool isColor = (slot.attr == GX_VA_CLR0 || slot.attr == GX_VA_CLR1);
            for (int c = 0; c < slot.comps; ++c) {
                if (isColor) {
                    // Direct colors are RGBA8 in the game (1 byte/component).
                    captureWrite(r.readU8(), 1);
                } else {
                    switch (sAttrFmt[sVtxFmt][slot.attr].type) {
                        case GX_U8:  captureWrite(r.readU8(), 1); break;
                        case GX_S8:  captureWrite(static_cast<float>(static_cast<s8>(r.readU8())), 1); break;
                        case GX_U16: captureWrite(r.readU16(), 2); break;
                        case GX_S16: captureWrite(static_cast<float>(static_cast<s16>(r.readU16())), 2); break;
                        case GX_F32:
                        default:     captureWrite(r.readF32(), 4); break;
                    }
                }
            }
        }
    }
}

// Executes one display list from `data` (size bytes). Walks opcodes until the
// list ends; state commands apply to the live mirrors.
void dlRun(const u8* data, size_t size) {
    if (!data || size == 0) {
        return;
    }
    if (sDlDepth >= kMaxDlDepth) {
        PL_LOG_WARN("gx", "GXCallDisplayList: recursion depth exceeded — aborting DL");
        return;
    }
    ++sDlDepth;
    DlReader r;
    r.p = data;
    r.end = data + size;

    while (r.ok && r.remaining() > 0) {
        const u8 cmd = r.readU8();
        switch (cmd) {
            case 0x00:  // NOP
                break;
            case 0x08: {  // CP stream register
                const u8 command = r.readU8();
                const u32 value = r.readU32();
                dlApplyCp(command, value);
                break;
            }
            case 0x10: {  // XF load
                const u32 hdr = r.readU32();
                const u16 baseAddr = static_cast<u16>(hdr & 0xFFFF);
                const int count = static_cast<int>((hdr >> 16) & 0xF) + 1;
                for (int i = 0; i < count; ++i) {
                    dlApplyXf(static_cast<u16>(baseAddr + i), r.readU32());
                }
                break;
            }
            case 0x40: {  // CALL_DL — see M5.6 notes (PC cannot translate the
                          // 32-bit address; the game nests lists from C++).
                const u32 addr = r.readU32();
                const u32 sizeBytes = r.readU32();
                PL_LOG_WARN("gx", "DL 0x40 CALL_DL (addr 0x%08X, size %u) inside a "
                            "display list — skipped (PC: nest via GXCallDisplayList)",
                            addr, sizeBytes);
                break;
            }
            case 0x44:  // (GXClearVCacheMetric path — no-op)
            case 0x48:  // GXInvalidateVtxCache — no-op (no vertex cache)
                break;
            case 0x61: {  // BP register
                const u32 raw = r.readU32();
                const u8 rid = static_cast<u8>(raw >> 24);
                if (rid == 0xFE) {  // BPMEM_BP_MASK: masks the NEXT BP write
                    sBpMask = raw & 0xFFFFFF;
                    break;
                }
                // (old & ~mask) | (value & mask) with the mirror's old value
                // unknown — masked writes are not used by the game; the
                // unmasked bits of the incoming value are applied as-is.
                if (sBpMask != 0xFFFFFF) {
                    PL_LOG_WARN("gx", "DL BP masked write (mask 0x%06X) — applying "
                                "masked bits only", sBpMask);
                    dlApplyBp(rid, raw & sBpMask);
                    sBpMask = 0xFFFFFF;
                } else {
                    dlApplyBp(rid, raw & 0xFFFFFF);
                }
                break;
            }
            default:
                if (cmd >= 0x20 && cmd <= 0x38) {
                    // XF indexed load (LOADINDX; GXLoad*MtxIdx commands).
                    // Payload: one u32 = (index<<16)|(size-1<<12)|address
                    // (J3DFifoLoadIndx / CP_XF_LOADINDEX). The array is
                    // selected by the opcode: 0x20->XF_A (pos), 0x28->XF_B
                    // (nrm), 0x30->XF_C (tex), 0x38->XF_D (light); the
                    // interpreter copies `size` floats from the CP array to
                    // the XF matrix mirror (Dolphin's LoadIndexedXF).
                    const u32 v = r.readU32();
                    const u32 index = v >> 16;
                    const u32 xfAddr = v & 0xFFF;
                    const int size = static_cast<int>((v >> 12) & 0xF) + 1;
                    const int arrayIdx = (cmd >> 3) + 8;  // 0x20->12 .. 0x38->15
                    const int attr = arrayIdx + GX_VA_POS;  // 21..24
                    if (attr < kMaxArrayAttr && sArrays[attr].set && sArrays[attr].base) {
                        const u8* src = sArrays[attr].base +
                                        static_cast<size_t>(index) * sArrays[attr].stride;
                        Platform::CompatGx::dlCopyMtxRegs(xfAddr, size,
                                                          reinterpret_cast<const float*>(src));
                    } else {
                        PL_LOG_WARN("gx", "DL LOADINDX 0x%02X: matrix array %d not "
                                    "registered — skipping", cmd, attr);
                    }
                    break;
                }
                if (cmd >= 0x80 && cmd <= 0xBF) {
                    // Primitive: (cmd >> 3) is the CP primitive command.
                    const u8 vat = cmd & 0x07;
                    const u16 nverts = r.readU16();
                    const u8 primCmd = cmd >> 3;
                    GXPrimitive prim = GX_QUADS;
                    switch (primCmd) {
                        case 0x10: prim = GX_QUADS; break;
                        case 0x12: prim = GX_TRIANGLES; break;
                        case 0x13: prim = GX_TRIANGLESTRIP; break;
                        case 0x14: prim = GX_TRIANGLEFAN; break;
                        case 0x15: prim = GX_LINES; break;
                        case 0x16: prim = GX_LINESTRIP; break;
                        case 0x17: prim = GX_POINTS; break;
                        default:   prim = GX_TRIANGLES; break;
                    }
                    // Internal begin: same state as GXBegin (flushes any
                    // dangling primitive first — GXBegin does that too).
                    if (sInBegin) {
                        flushDraw();
                    }
                    sPrimitive = prim;
                    sVtxFmt = static_cast<GXVtxFmt>(vat);
                    sNverts = nverts;
                    sNvertsDone = 0;
                    sVertexData.clear();
                    sVertexTexCoords.clear();
                    sVertexTexQ.clear();
                    sVtxWriteIndex = 0;
                    rebuildVcd();
                    const bool fastStrip = tryArmFastIndex(nverts, false);
                    sInBegin = (nverts > 0);
                    if (fastStrip) {
                        int w = 0;
                        int cw = 0;
                        for (int v = 0; v < nverts && r.ok; ++v) {
                            for (const VcdSlot& slot : sVcdOrder) {
                                if (slot.source == GX_DIRECT) {
                                    for (int c = 0; c < slot.comps; ++c) {
                                        const std::uint8_t b = r.readU8();
                                        if (cw < static_cast<int>(sFastColorBytes.size())) {
                                            sFastColorBytes[static_cast<size_t>(cw)] = b;
                                        }
                                        ++cw;
                                    }
                                } else {
                                    const std::uint32_t ix = (slot.source == GX_INDEX16) ? r.readU16()
                                                                                         : r.readU8();
                                    if (w < static_cast<int>(sFastIndices.size())) {
                                        sFastIndices[static_cast<size_t>(w)] = ix;
                                    }
                                    ++w;
                                }
                            }
                        }
                        sFastCount = w;
                        sFastColorCount = cw;
                        gGxIndexCapture.active = false;
                        sNvertsDone = (sFastSlots > 0) ? w / sFastSlots : 0;
                        flushDraw();
                    } else {
                        for (u16 i = 0; i < nverts && r.ok; ++i) {
                            dlReadVertex(r);
                        }
                    }
                    break;
                }
                PL_LOG_WARN("gx", "DL unknown opcode 0x%02X at offset %td — stopping",
                            cmd, static_cast<ptrdiff_t>(r.p - data) - 1);
                r.ok = false;
                break;
        }
    }
    if (!r.ok) {
        PL_LOG_WARN("gx", "GXCallDisplayList: truncated or invalid display list");
    }
    --sDlDepth;
}

} // namespace

void gxFlushFastIndex() {
    flushFastIndexImpl();
}

// --- GX API implementations ---------------------------------------------------

extern "C" {

GXFifoObj* GXInit(void* fifoPtr, u32 fifoSize) {
    PL_LOG_TRACE("gx", "GXInit(%p, %u)", fifoPtr, static_cast<unsigned>(fifoSize));
    // M5.1: no hardware FIFO — reset the state mirror. The FIFO object the
    // game receives is unused by the port (returns nullptr).
    // TODO(PC_PORT, M5.2+): if game code stores/uses the GXFifoObj, provide a
    // real (no-op) object here.
    (void)fifoPtr;
    (void)fifoSize;
    std::memset(sVtxDesc, 0, sizeof(sVtxDesc));
    std::memset(sVtxDescSet, 0, sizeof(sVtxDescSet));
    for (auto& fmt : sAttrFmt) {
        for (auto& a : fmt) a = AttrFmt();
    }
    sHasProjection = false;
    // Pixel-engine reset (mirrors the vendored GXInit.c order):
    // GXSetCullMode(BACK) -> GXSetAlphaCompare(ALWAYS,0,AND,ALWAYS,0) ->
    // GXSetFog(NONE) -> GXSetFogRangeAdj(DISABLE) -> GXSetBlendMode(NONE,
    // SRCALPHA, INVSRCALPHA, CLEAR) -> GXSetColorUpdate(ENABLE) ->
    // GXSetAlphaUpdate(ENABLE) -> GXSetZMode(TRUE, LEQUAL, TRUE) ->
    // GXSetZCompLoc(TRUE) -> GXSetDither(ENABLE) -> GXSetDstAlpha(DISABLE, 0)
    // -> GXSetPixelFmt(RGB8_Z24, ZC_LINEAR).
    sCullMode = GX_CULL_BACK;
    sBlendMode = GX_BM_NONE;
    sBlendSrc = GX_BL_SRCALPHA;
    sBlendDst = GX_BL_INVSRCALPHA;
    sBlendLogicOp = GX_LO_CLEAR;
    sZTest = true;
    sZWrite = true;
    sZFunc = GX_LEQUAL;
    sZCompLoc = true;
    sColorUpdate = true;
    sAlphaUpdate = true;
    sDstAlphaEnable = false;
    sDstAlphaValue = 0;
    sDither = true;
    sPixelFmt = GX_PF_RGB8_Z24;
    sZFormat = GX_ZC_LINEAR;
    sInBegin = false;
    sVertexData.clear();
    sVertexTexCoords.clear();
    sVertexTexQ.clear();
    sVcdOrder.clear();
    sVcdTotalWrites = 0;
    sDebugData.clear();
    sDebugStride = 0;
    sDynUsedBytes = 0;
    sBatch.valid = false;  // M9.7: drop any pending draw batch on full reset
    for (auto& a : sArrays) {
        a = ArraySlot();
    }
    Platform::CompatGx::resetTextureState();
    Platform::CompatGx::resetTevState();
    // M5.7a: channel lighting (GXSetNumChans/GXSetChanCtrl/amb/mat/lights +
    // identity pos/nrm matrices, current 0).
    Platform::CompatGx::resetLightingState();
    return nullptr;
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    PL_LOG_TRACE("gx", "GXSetVtxDesc(%d, %d)", static_cast<int>(attr), static_cast<int>(type));
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
        sVtxDesc[attr] = type;
        sVtxDescSet[attr] = true;
        rebuildVcd();
    }
}

void GXClearVtxDesc(void) {
    PL_LOG_TRACE("gx", "GXClearVtxDesc");
    std::memset(sVtxDesc, 0, sizeof(sVtxDesc));
    std::memset(sVtxDescSet, 0, sizeof(sVtxDescSet));
    rebuildVcd();
}

void GXSetVtxAttrFmtv(GXVtxFmt vtxfmt, const GXVtxAttrFmtList* list) {
    PL_LOG_TRACE("gx", "GXSetVtxAttrFmtv(%d)", static_cast<int>(vtxfmt));
    // The list is terminated by attr == GX_VA_NULL (0xff).
    for (const GXVtxAttrFmtList* it = list; it && it->attr != GX_VA_NULL; ++it) {
        GXSetVtxAttrFmt(vtxfmt, it->attr, it->cnt, it->type, it->frac);
    }
}

void GXSetArray(GXAttr attr, const void* base, u8 stride) {
    PL_LOG_TRACE("gx", "GXSetArray(%d, %p, %u)", static_cast<int>(attr), base,
                 static_cast<unsigned>(stride));
    // GX_VA_NBT arrays are the normals array on the console (GXAttr.c).
    if (attr == GX_VA_NBT) {
        attr = GX_VA_NRM;
    }
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
        sArrays[attr].base = static_cast<const u8*>(base);
        sArrays[attr].stride = stride;
        sArrays[attr].set = (base != nullptr);
        // Register the physical base (the same GX_PHY_ADDR value the console
        // bakes into display lists) so the M5.6 DL interpreter can resolve
        // ARRAY_BASE commands back to this host pointer. Computed manually
        // (no GX_PHY_ADDR cast, which truncates on 64-bit hosts with a
        // warning): PHY_ADDR_MASK is 0x3FFFFFFF.
        const u32 phy = static_cast<u32>(reinterpret_cast<uintptr_t>(base)) & 0x3FFFFFFFu;
        sArrayPhyMap[phy] = static_cast<const u8*>(base);
    }
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    PL_LOG_TRACE("gx", "GXSetVtxAttrFmt(%d, %d, %d, %d, %u)",
                 static_cast<int>(vtxfmt), static_cast<int>(attr), static_cast<int>(cnt),
                 static_cast<int>(type), static_cast<unsigned>(frac));
    if (vtxfmt >= GX_VTXFMT0 && vtxfmt <= GX_VTXFMT7 && attr >= 0 && attr < GX_VA_MAX_ATTR) {
        sAttrFmt[vtxfmt][attr] = {cnt, type, frac, true};
        rebuildVcd();
    }
}

void GXBegin(GXPrimitive prim, GXVtxFmt vtxfmt, u16 nverts) {
    PL_LOG_TRACE("gx", "GXBegin(%d, %d, %u)", static_cast<int>(prim), static_cast<int>(vtxfmt),
                 static_cast<unsigned>(nverts));
    if (sInBegin) {
        // Flush any dangling primitive (defensive; the game always pairs
        // GXBegin/GXEnd correctly).
        flushDraw();
    }
    sPrimitive = prim;
    sVtxFmt = vtxfmt;
    sNverts = nverts;
    sNvertsDone = 0;
    sVertexData.clear();
    sVertexTexCoords.clear();
    sVertexTexQ.clear();
    sVtxWriteIndex = 0;
    rebuildVcd();
    // M9.10 perf: size the capture vectors for the whole primitive up front —
    // the per-vertex push_backs then never reallocate (and their capacity
    // checks stay predictable). Vertex stride = sum of the VCD components.
    int strideTotal = 0;
    for (const auto& slot : sVcdOrder) {
        strideTotal += slot.comps;
    }
    sVertexData.reserve(static_cast<size_t>(nverts) * static_cast<size_t>(strideTotal));
    sVertexTexCoords.reserve(static_cast<size_t>(nverts) * 16);
    sVertexTexQ.reserve(static_cast<size_t>(nverts) * 8);
    tryArmFastIndex(nverts, true);
    sInBegin = (nverts > 0);
}

// --- M5.6: display lists ------------------------------------------------------

// Replays a display list built with the GD writers (J3DDisplayListObj /
// GDWriteBPCmd / GDWriteCPCmd / GDWriteXFCmd / raw vertex writes). See the
// interpreter notes in GXCompat.cpp: state commands (CP/XF/BP) update the
// same mirrors the GXSet* APIs drive, primitives feed the internal vertex
// machinery (no external GXBegin). GXBeginDisplayList/GXEndDisplayList (the
// RVLFaceLib recording path) are not needed by the PC build — RVLFaceLib is
// not compiled; J3D builds lists by recording GD writes into its own buffer.
void GXCallDisplayList(const void* list, u32 nbytes) {
    PL_LOG_TRACE("gx", "GXCallDisplayList(%p, %u)", list, static_cast<unsigned>(nbytes));
    dlRun(static_cast<const u8*>(list), nbytes);
}

// PC_PORT diagnostics: the mirrored viewport the next flushDraw will apply.
// A 3D pass inherits whatever the previous pass set, and "the sky was drawn into
// a 456-row viewport" is exactly the kind of bug a still image cannot tell apart
// from a projection bug (both squash the image vertically), so the sky logs it.
void debugViewport(f32* outX, f32* outY, f32* outW, f32* outH) {
    if (outX != nullptr) {
        *outX = sViewportX;
    }
    if (outY != nullptr) {
        *outY = sViewportY;
    }
    if (outW != nullptr) {
        *outW = sViewportW;
    }
    if (outH != nullptr) {
        *outH = sViewportH;
    }
}

void GXSetProjection(const f32 mtx[4][4], GXProjectionType type) {
    PL_LOG_TRACE("gx", "GXSetProjection");

    // PC_PORT diagnostic: a 3D pass inherits whatever viewport the previous pass
    // set, and a still image cannot tell "wrong projection" and "viewpoint
    // squashed into a 456-row viewport" apart. Log the mirrored rect every 600
    // perspective setups (~10 s) next to the depth range, so a capture carries
    // the framing the pass was drawn with.
    static u32 sProjectionLogCount = 0;
    ++sProjectionLogCount;
    if (type == GX_PERSPECTIVE && sProjectionLogCount % 600 == 0) {
        PL_LOG_INFO("gx",
                    "GXSetProjection #%u (perspective): viewport=(%.0f,%.0f %.0fx%.0f) "
                    "depth=%.2f..%.2f",
                    static_cast<unsigned>(sProjectionLogCount), sViewportX, sViewportY, sViewportW,
                    sViewportH, sViewportNearZ, sViewportFarZ);
    }
    std::memcpy(sProjection, mtx, sizeof(sProjection));
    sHasProjection = true;
}

void GXSetScissor(u32 x, u32 y, u32 w, u32 h) {
    PL_LOG_TRACE("gx", "GXSetScissor(%u, %u, %u, %u)", x, y, w, h);
    sScissor[0] = x;
    sScissor[1] = y;
    sScissor[2] = w;
    sScissor[3] = h;
    sScissorSet = true;
    if (Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().setScissor(x, y, w, h);
    }
}

void GXSetCullMode(GXCullMode mode) {
    PL_LOG_TRACE("gx", "GXSetCullMode(%d)", static_cast<int>(mode));
    sCullMode = mode;
}

void GXSetBlendMode(GXBlendMode mode, GXBlendFactor srcFactor, GXBlendFactor dstFactor,
                    GXLogicOp op) {
    PL_LOG_TRACE("gx", "GXSetBlendMode(%d, %d, %d, %d)", static_cast<int>(mode),
                 static_cast<int>(srcFactor), static_cast<int>(dstFactor),
                 static_cast<int>(op));
    // Mirror the vendored GXPixel.c: blend_en = mode & 1 (GX_BM_NONE=0,
    // GX_BM_BLEND=1); SUBTRACT and LOGIC are separate enables with their own
    // op; the factors are stored raw and mapped at draw time.
    sBlendMode = mode;
    sBlendSrc = srcFactor;
    sBlendDst = dstFactor;
    sBlendLogicOp = op;
}

void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    PL_LOG_TRACE("gx", "GXSetZMode(%d, %d, %d)", static_cast<int>(compare_enable),
                 static_cast<int>(func), static_cast<int>(update_enable));
    sZTest = (compare_enable != GX_FALSE);
    sZFunc = func;
    sZWrite = (update_enable != GX_FALSE);
}

void GXSetZCompLoc(GXBool before_tex) {
    PL_LOG_TRACE("gx", "GXSetZCompLoc(%d)", static_cast<int>(before_tex));
    // The Z comparison location (before/after the TEV) affects only Z-texture
    // emulation (M5.7); both orders write the same depth for the current
    // pipeline, so the state is mirrored for API completeness.
    sZCompLoc = (before_tex != GX_FALSE);
}

void GXSetColorUpdate(GXBool update_enable) {
    PL_LOG_TRACE("gx", "GXSetColorUpdate(%d)", static_cast<int>(update_enable));
    sColorUpdate = (update_enable != GX_FALSE);
}

void GXSetAlphaUpdate(GXBool update_enable) {
    PL_LOG_TRACE("gx", "GXSetAlphaUpdate(%d)", static_cast<int>(update_enable));
    sAlphaUpdate = (update_enable != GX_FALSE);
}

void GXSetDstAlpha(GXBool enable, u8 alpha) {
    PL_LOG_TRACE("gx", "GXSetDstAlpha(%d, %u)", static_cast<int>(enable),
                 static_cast<unsigned>(alpha));
    // The constant alpha is used by the DSTALPHA/INVDSTALPHA blend factors
    // (mapped to CONSTANT_ALPHA in the pipeline) and by GXCopyDisp later.
    sDstAlphaEnable = (enable != GX_FALSE);
    sDstAlphaValue = alpha;
}

void GXSetDither(GXBool dither) {
    PL_LOG_TRACE("gx", "GXSetDither(%d)", static_cast<int>(dither));
    // Mirrored for API completeness. Dithering on the Flipper is a 2x2 Bayer
    // pattern applied for 6-bit EFB formats (RGB565/RGBA6); the PC render
    // target is RGBA8, where the hardware does not dither either.
    sDither = (dither != GX_FALSE);
}

void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) {
    PL_LOG_TRACE("gx", "GXSetPixelFmt(%d, %d)", static_cast<int>(pix_fmt),
                 static_cast<int>(z_fmt));
    // Mirrored for API completeness. The EFB format is fixed at RGBA8 on PC
    // (RGB8_Z24-equivalent); format-dependent behavior (6-bit dithering,
    // YUV modes, EFB copy formats) arrives with the EFB in M5.7.
    sPixelFmt = pix_fmt;
    sZFormat = z_fmt;
}

void GXSetViewport(f32 x, f32 y, f32 w, f32 h, f32 nearZ, f32 farZ) {
    PL_LOG_TRACE("gx", "GXSetViewport(%.0f, %.0f, %.0f, %.0f, %.2f, %.2f)", x, y, w, h, nearZ, farZ);
    sViewportX = x;
    sViewportY = y;
    sViewportW = w;
    sViewportH = h;
    // M9.5.3c: the depth-range arguments feed the viewport depth transform,
    // folded into the MVP at flushDraw (GX clip z [-1,1] -> VK depth [0,1]).
    sViewportNearZ = nearZ;
    sViewportFarZ = farZ;
    if (Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().setViewport(x, y, w, h);
    }
}

void GXClearColor(GXColor color) {
    PL_LOG_TRACE("gx", "GXClearColor(%u, %u, %u, %u)", color.r, color.g, color.b, color.a);
    sClearColor[0] = color.r / 255.0f;
    sClearColor[1] = color.g / 255.0f;
    sClearColor[2] = color.b / 255.0f;
    sClearColor[3] = color.a / 255.0f;
    if (Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().setClearColor(sClearColor[0], sClearColor[1],
                                                     sClearColor[2], sClearColor[3]);
    }
}

void GXClear(u32 clrMask) {
    PL_LOG_TRACE("gx", "GXClear(0x%x)", static_cast<unsigned>(clrMask));
    (void)clrMask; // the pass is always cleared with the current color
}

void GXCompatEndFrame() {
    // Reset the dynamic-buffer cursor for the next frame. The backing buffer
    // is reused: endFrame() waited the frame fence, so no command buffer
    // references the previous contents anymore and rewriting is safe.
    sDynUsedBytes = 0;
    // M9.7: the endPass hook already emitted any pending batch (the pass is
    // closed by now), so just drop the stale candidate for a clean next frame.
    sBatch.valid = false;
    // Re-register the endPass hook on the next frame's first draw: the Renderer
    // may have been torn down and re-created (tests, device loss), which drops
    // the std::function while this static would otherwise stay true.
    sEndPassHookSet = false;
}

void GXCompatShutdown() {
    if (sDynVb && Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().destroyBuffer(sDynVb);
    }
    sDynVb = nullptr;
    sDynUsedBytes = 0;
    destroyWhiteFallback();
    Platform::CompatGx::shutdownTextures();
    Platform::CompatGx::shutdownEfb(); // M5.7c: release the EFB render target
}

const float* GXCompatDebugVertices(int* outCount, int* outStride) {
    if (outCount) *outCount = static_cast<int>(sDebugData.size()) / (sDebugStride > 0 ? sDebugStride : 1);
    if (outStride) *outStride = sDebugStride;
    return sDebugData.empty() ? nullptr : sDebugData.data();
}

bool GXCompatDebugTexGenQ(int coord, float* outQ) {
    if (coord < 0 || coord >= 8 || outQ == nullptr) {
        return false;
    }
    *outQ = Platform::CompatGx::texGenW(coord);
    return true;
}

void GXCompatDebugPeState(GxPeDebugState& out) {
    out.cullMode = sCullMode;
    out.blendMode = sBlendMode;
    out.blendSrc = sBlendSrc;
    out.blendDst = sBlendDst;
    out.blendLogicOp = sBlendLogicOp;
    out.zTest = sZTest;
    out.zFunc = sZFunc;
    out.zWrite = sZWrite;
    out.zCompLoc = sZCompLoc;
    out.colorUpdate = sColorUpdate;
    out.alphaUpdate = sAlphaUpdate;
    out.dstAlphaEnable = sDstAlphaEnable;
    out.dstAlphaValue = sDstAlphaValue;
    out.dither = sDither;
    out.pixelFmt = sPixelFmt;
    out.zFormat = sZFormat;
}

} // extern "C"
