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
u32 sScissor[4] = {0, 0, 0, 0};
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

CullMode cullModeFromGx(GXCullMode m) {
    switch (m) {
        case GX_CULL_NONE:  return CullMode::None;
        case GX_CULL_FRONT: return CullMode::Front;
        case GX_CULL_BACK:  return CullMode::Back;
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
};
std::vector<VcdSlot> sVcdOrder;
int sVcdTotalWrites = 0; // FIFO words per vertex (DIRECT: comps, indexed: 1)

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
                sVcdOrder.push_back({attr, comps, sVtxDesc[attr]});
            }
        }
    }
    sVcdTotalWrites = 0;
    for (const auto& slot : sVcdOrder) {
        sVcdTotalWrites += (slot.source == GX_DIRECT) ? slot.comps : 1;
    }
}

// Converts a raw GX component value to float according to the attribute's
// comp type (GX_VAT). Colors (u8/rgba) are normalized to 0..1; numeric
// formats scale by the fraction (value / 2^frac).
float convertComponent(int attr, float value, bool isColorByte) {
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        return isColorByte ? value / 255.0f : value;
    }
    const AttrFmt& fmt = sAttrFmt[sVtxFmt][attr];
    switch (fmt.type) {
        case GX_U8:
        case GX_S8:
        case GX_U16:
        case GX_S16:
            return value / static_cast<float>(1 << fmt.frac);
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
// sCurAttr[attr][0..comps). Colors RGBA8 normalize to 0..1.
void fetchArrayAttr(int attr, u32 index) {
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
    const AttrFmt& fmt = sAttrFmt[sVtxFmt][attr];
    const int comps = attrComponentCount(sVtxFmt, attr);
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        // M5.2 handles RGBA8 arrays; other color encodings (RGB565, ...)
        // arrive with the color pipeline (M5.5).
        for (int c = 0; c < comps; ++c) {
            sCurAttr[attr][c] = p[c] / 255.0f;
        }
        return;
    }
    const int elemSize = compByteSize(fmt.type);
    for (int c = 0; c < comps; ++c) {
        sCurAttr[attr][c] = readArrayComp(p + c * elemSize, fmt.type, fmt.frac);
    }
}

// Submits the completed primitive to the renderer (if initialized).
void flushDraw();

void finishVertex() {
    // M5.3: resolve the texcoord generators (GXSetTexCoordGen2) on the CPU
    // using the completed vertex attributes before serializing.
    for (const auto& slot : sVcdOrder) {
        if (slot.attr >= GX_VA_TEX0 && slot.attr <= GX_VA_TEX7) {
            Platform::CompatGx::resolveTexGen(slot.attr - GX_VA_TEX0, sCurAttr);
        }
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

// Maps the global write index to the VCD slot and the component within it.
// Direct slots consume `comps` writes, indexed slots consume 1 (the index).
// Returns nullptr on overflow (write past the end of the vertex layout).
const VcdSlot* mapWriteIndex(int* outComp) {
    const int wi = sVtxWriteIndex;
    int base = 0;
    for (const auto& slot : sVcdOrder) {
        const int writes = (slot.source == GX_DIRECT) ? slot.comps : 1;
        if (wi < base + writes) {
            *outComp = wi - base;
            return &slot;
        }
        base += writes;
    }
    return nullptr;
}

// writeSize = bytes this 32-bit FIFO word contributes to the stream (u8/s8 ->
// 1, u16/s16 -> 2, f32 -> 4). The stream is consumed positionally in VCD
// order, exactly like the PPC vertex loader: each write fills the next
// component(s) of the current vertex. For an indexed slot the FIFO word is
// the array index and the attribute data is fetched via fetchArrayAttr.
void captureWrite(float value, int writeSize) {
    if (!sInBegin) {
        // Writes outside GXBegin are illegal on the console too; the game
        // never does this.
        PL_LOG_WARN("gx", "vertex write outside GXBegin ignored");
        return;
    }
    int comp = -1;
    const VcdSlot* slot = mapWriteIndex(&comp);
    if (!slot) {
        PL_LOG_WARN("gx", "vertex write overflow (VCD writes %d)", sVcdTotalWrites);
        return;
    }
    if (slot->source != GX_DIRECT) {
        // Indexed attribute: one FIFO word = the array index (u8/u16 values
        // are exact in float). Resolve the data from the GXSetArray array.
        fetchArrayAttr(slot->attr, static_cast<u32>(value));
    } else {
        const bool isColor = (slot->attr == GX_VA_CLR0 || slot->attr == GX_VA_CLR1);
        // One FIFO word = one component of the vertex.
        sCurAttr[slot->attr][comp] =
            convertComponent(slot->attr, value, writeSize == 1 && isColor);
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
    int comp = -1;
    const VcdSlot* slot = mapWriteIndex(&comp);
    if (!slot) {
        PL_LOG_WARN("gx", "vertex write overflow (VCD writes %d)", sVcdTotalWrites);
        return;
    }
    if (slot->source != GX_DIRECT ||
        (slot->attr != GX_VA_CLR0 && slot->attr != GX_VA_CLR1)) {
        PL_LOG_WARN("gx", "u32 write into a non-direct-color attribute — dropped");
        return;
    }
    for (int c = 0; c < 4; ++c) {
        const u8 byte = static_cast<u8>((packed >> (24 - 8 * c)) & 0xFF);
        sCurAttr[slot->attr][comp + c] = byte / 255.0f;
    }
    sVtxWriteIndex += 4;
    if (sVtxWriteIndex >= sVcdTotalWrites) {
        finishVertex();
    }
}

} // namespace

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

void flushDraw() {
    if (sVertexData.empty()) {
        sInBegin = false;
        sNvertsDone = 0;
        return;
    }

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
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();

    // --- expand to the fixed TEV vertex layout (27 floats) ------------------
    // pos(3) clr0(4) clr1(4) tex0..7(2 each). Missing attributes are filled:
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
    constexpr int kFixedStride = 27;
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
        for (int t = 0; t < 8; ++t) {
            out[11 + 2 * t] = (texOffset[t] >= 0) ? src[texOffset[t]] : 0.0f;
            out[12 + 2 * t] = (texOffset[t] >= 0) ? src[texOffset[t] + 1] : 0.0f;
        }
    };
    std::vector<float> drawData;
    if (sPrimitive == GX_QUADS && (nverts % 4) == 0) {
        drawData.reserve(static_cast<size_t>(nverts / 4 * 6 * kFixedStride));
        for (int q = 0; q < nverts; q += 4) {
            for (int idx : {0, 1, 2, 0, 2, 3}) {
                float v[kFixedStride];
                buildVertex(q + idx, v);
                drawData.insert(drawData.end(), v, v + kFixedStride);
            }
        }
    } else {
        drawData.resize(static_cast<size_t>(nverts) * kFixedStride);
        for (int i = 0; i < nverts; ++i) {
            buildVertex(i, drawData.data() + static_cast<size_t>(i) * kFixedStride);
        }
    }
    sVertexData.clear();
    const int drawVerts = static_cast<int>(drawData.size()) / kFixedStride;

    // --- pipeline: universal TEV variant ------------------------------------
    Platform::PipelineDesc desc;
    desc.topology = (sPrimitive == GX_TRIANGLES)
                        ? Platform::PrimitiveTopology::TriangleList
                        : (sPrimitive == GX_TRIANGLESTRIP)
                              ? Platform::PrimitiveTopology::TriangleStrip
                              : (sPrimitive == GX_TRIANGLEFAN)
                                    ? Platform::PrimitiveTopology::TriangleFan
                                    : (sPrimitive == GX_LINES)
                                          ? Platform::PrimitiveTopology::LineList
                                          : (sPrimitive == GX_LINESTRIP)
                                                ? Platform::PrimitiveTopology::LineStrip
                                                : (sPrimitive == GX_POINTS)
                                                      ? Platform::PrimitiveTopology::PointList
                                                      : Platform::PrimitiveTopology::TriangleList; // QUADS expanded above
    desc.vertexLayout.stride = kFixedStride * sizeof(float);
    desc.vertexLayout.attribs = {
        {0, 0, Platform::VertexFormat::R32G32B32_SFLOAT},
        {1, 12, Platform::VertexFormat::R32G32B32A32_SFLOAT},
        {2, 28, Platform::VertexFormat::R32G32B32A32_SFLOAT},
        {3, 44, Platform::VertexFormat::R32G32_SFLOAT},
        {4, 52, Platform::VertexFormat::R32G32_SFLOAT},
        {5, 60, Platform::VertexFormat::R32G32_SFLOAT},
        {6, 68, Platform::VertexFormat::R32G32_SFLOAT},
        {7, 76, Platform::VertexFormat::R32G32_SFLOAT},
        {8, 84, Platform::VertexFormat::R32G32_SFLOAT},
        {9, 92, Platform::VertexFormat::R32G32_SFLOAT},
        {10, 100, Platform::VertexFormat::R32G32_SFLOAT},
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
    desc.dstAlphaValue = sDstAlphaValue / 255.0f;
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

    // M5.2: append to the shared dynamic vertex buffer instead of allocating a
    // per-primitive buffer. Grows on demand (old allocations are retired and
    // destroyed at the next endFrame, after the frame fence).
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

    r.bindPipeline(pipe);

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
    if (sWhiteTex) {
        r.bindFragmentTextures(tex, sam, 8);
    }

    // --- TEV constants (UBO, one region per draw) ----------------------------
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
    if (!r.uploadFragmentUbo(&ubo, sizeof(ubo))) {
        // Arena exhausted: drop the draw (the vertex bytes stay reserved;
        // next frame's cursor reset makes them reusable).
        sDynUsedBytes += bytes;
        return;
    }

    r.bindVertexBuffer(sDynVb, 0);
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
    r.setUniforms(mvp, sizeof(mvp));
    r.draw(static_cast<uint32_t>(drawVerts),
           static_cast<uint32_t>(sDynUsedBytes / (static_cast<uint64_t>(kFixedStride) * sizeof(float))));
    sDynUsedBytes += bytes;
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
                    sVtxWriteIndex = 0;
                    rebuildVcd();
                    sInBegin = (nverts > 0);
                    for (u16 i = 0; i < nverts && r.ok; ++i) {
                        dlReadVertex(r);
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
    sVcdOrder.clear();
    sVcdTotalWrites = 0;
    sDebugData.clear();
    sDebugStride = 0;
    sDynUsedBytes = 0;
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
    sVtxWriteIndex = 0;
    rebuildVcd();
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

void GXSetProjection(const f32 mtx[4][4], GXProjectionType /*type*/) {
    PL_LOG_TRACE("gx", "GXSetProjection");
    std::memcpy(sProjection, mtx, sizeof(sProjection));
    sHasProjection = true;
}

void GXSetScissor(u32 x, u32 y, u32 w, u32 h) {
    PL_LOG_TRACE("gx", "GXSetScissor(%u, %u, %u, %u)", x, y, w, h);
    sScissor[0] = x;
    sScissor[1] = y;
    sScissor[2] = w;
    sScissor[3] = h;
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
    (void)nearZ;
    (void)farZ;
    PL_LOG_TRACE("gx", "GXSetViewport(%.0f, %.0f, %.0f, %.0f)", x, y, w, h);
    sViewportX = x;
    sViewportY = y;
    sViewportW = w;
    sViewportH = h;
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
