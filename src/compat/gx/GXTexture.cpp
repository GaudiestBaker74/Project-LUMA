// =============================================================================
// compat/gx — texture objects, texgen and tex matrices (M5.3).
//
// Model:
//   * GXInitTexObj stores the raw GX tiled image (the game passes a pointer
//     into the loaded BTI/JUTTexture buffer). The renderer texture is created
//     lazily on the first GXLoadTexObj: the tiled data is decoded to RGBA8 by
//     Bti.cpp and uploaded via Platform::Renderer::createTexture.
//   * GXLoadTexObj binds a texture object to a GX_TEXMAP0..7 slot. M5.3
//     consumes slot 0 (TEXMAP0) in the fragment shader; the other slots are
//     stored and will be used by the TEV pipeline (M5.4).
//   * GXSetTexCoordGen2 stores the texcoord generator per GX_TEXCOORD0..7;
//     compat/gx resolves it on the CPU while serializing each vertex (identity
//     matrix, or the matrix loaded with GXLoadTexMtxImm). GX_TG_BUMP* /
//     SRTG and normal-matrix sources are documented TODOs (M5.7).
//
// The GXTexObj SDK struct (u32 dummy[8]) is used to store a pointer to our
// per-object state, so the game can keep texture objects embedded anywhere.
// =============================================================================

#include "compat/gx/GXCompat.h"

#include "compat/gx/Bti.h"

#include "platform/Log/Log.h"
#include "platform/Renderer/Renderer.h"

#include <cstring>
#include <vector>

namespace Platform::CompatGx {

// --- helpers exposed to GXCompat.cpp ------------------------------------------

// Resolves the texgen for `coordId` (GX_TEXCOORD0..7) using the current
// vertex attributes, writing the generated (u,v) back into attrs[GX_VA_TEX0+
// coordId][0..1]. attrs is sCurAttr (all attributes of the vertex being
// built). No-op when no texgen is configured for that coord (pass-through).
void resolveTexGen(int coordId, float attrs[GX_VA_MAX_ATTR][4]);

// Returns the texture/sampler currently bound to TEXMAP0 (or nulls).
void getTexMap0(Platform::TextureHandle* outTex, Platform::SamplerHandle* outSam);

// Resets the GX texture state (texgen config, tex matrices, loaded texmap
// bindings). Called from GXInit, mirroring the console's register reset.
void resetTextureState();

// Releases all renderer resources owned by the GX texture state (called from
// GXCompatShutdown, after the renderer is idle).
void shutdownTextures();

namespace {

// Per-GXTexObj state (the game owns the GXTexObj; we own this).
struct TexObjData {
    const u8* image = nullptr;      // raw GX tiled data
    u16 width = 0, height = 0;
    u8 format = 0;                  // GXTexFmt
    u8 wrapS = 0, wrapT = 0;        // GXTexWrapMode
    bool mipmap = false;
    GXTexFilter minFilter = GX_LINEAR;
    GXTexFilter magFilter = GX_LINEAR;
    f32 minLod = 0.0f, maxLod = 0.0f, lodBias = 0.0f;
    GXBool biasClamp = GX_FALSE, edgeLod = GX_FALSE;
    GXAnisotropy anisotropy = GX_ANISO_1;

    const u8* palette = nullptr;    // TLUT (C4/C8/C14X2)
    size_t paletteBytes = 0;
    u8 paletteFormat = 0;           // GXTlutFmt
    u32 paletteCount = 0;

    Platform::TextureHandle texture = nullptr; // lazily created at first load
    Platform::SamplerHandle sampler = nullptr;
    bool loaded = false;
};

std::vector<TexObjData*> sTexObjs;  // cleanup on shutdown

// TEXMAP0..7 -> renderer resources.
Platform::TextureHandle sTexMapTex[8] = {};
Platform::SamplerHandle sTexMapSam[8] = {};
// TEXMAP0..7 -> texel size of the bound texture (M5.7b texDims UBO field).
float sTexMapDims[8][2] = {};

struct TexGen {
    GXTexGenType type = GX_TG_MTX2x4;
    GXTexGenSrc src = GX_TG_TEX0;
    u32 mtxId = 0;
    bool normalize = false;
    bool set = false;
};
TexGen sTexGen[8];

// GX_TEXMTX0..9 (3x4 row-major).
f32 sTexMtx[10][3][4] = {};

TexObjData* texObjData(const GXTexObj* obj) {
    TexObjData* p = nullptr;
    if (obj) {
        std::memcpy(&p, obj->dummy, sizeof(p));
    }
    return p;
}
void setTexObjData(GXTexObj* obj, TexObjData* p) {
    std::memcpy(obj->dummy, &p, sizeof(p));
}

// GX sampler state -> renderer SamplerDesc.
Platform::SamplerDesc samplerFromGx(u8 wrapS, u8 wrapT, GXTexFilter minF,
                                    GXTexFilter magF) {
    Platform::SamplerDesc d;
    const auto addr = [](u8 w) {
        switch (w) {
            case GX_REPEAT: return Platform::SamplerAddressMode::Repeat;
            case GX_MIRROR: return Platform::SamplerAddressMode::MirrorRepeat;
            case GX_CLAMP:
            default: return Platform::SamplerAddressMode::ClampToEdge;
        }
    };
    d.addressU = addr(wrapS);
    d.addressV = addr(wrapT);
    // GX mag filters: NEAR / LINEAR.
    d.magFilter = (magF == GX_NEAR || magF == GX_NEAR_MIP_NEAR || magF == GX_NEAR_MIP_LIN)
                      ? Platform::SamplerFilter::Nearest
                      : Platform::SamplerFilter::Linear;
    // GX min filters encode the min + mip filter pair.
    switch (minF) {
        case GX_NEAR:
            d.minFilter = Platform::SamplerFilter::Nearest;
            d.mipFilter = Platform::SamplerFilter::Nearest;
            break;
        case GX_LINEAR:
            d.minFilter = Platform::SamplerFilter::Linear;
            d.mipFilter = Platform::SamplerFilter::Nearest;
            break;
        case GX_NEAR_MIP_NEAR:
            d.minFilter = Platform::SamplerFilter::Nearest;
            d.mipFilter = Platform::SamplerFilter::Nearest;
            break;
        case GX_LIN_MIP_NEAR:
            d.minFilter = Platform::SamplerFilter::Linear;
            d.mipFilter = Platform::SamplerFilter::Nearest;
            break;
        case GX_NEAR_MIP_LIN:
            d.minFilter = Platform::SamplerFilter::Nearest;
            d.mipFilter = Platform::SamplerFilter::Linear;
            break;
        case GX_LIN_MIP_LIN:
        default:
            d.minFilter = Platform::SamplerFilter::Linear;
            d.mipFilter = Platform::SamplerFilter::Linear;
            break;
    }
    return d;
}

// Creates (or refreshes) the renderer texture + sampler for `d`.
bool loadTexture(TexObjData& d) {
    Platform::Renderer& r = Platform::Renderer::instance();
    if (!r.isInitialized()) {
        return false;
    }
    if (!d.loaded) {
        std::vector<u8> rgba(static_cast<size_t>(d.width) * d.height * 4);
        const size_t need = btiImageSize(d.width, d.height, d.format);
        if (!btiDecodeToRgba8(d.image, need, d.width, d.height, d.format,
                              d.palette, d.paletteBytes, d.paletteFormat,
                              rgba.data())) {
            PL_LOG_WARN("gx", "GXLoadTexObj: unsupported format 0x%x (%ux%u)",
                        static_cast<unsigned>(d.format), d.width, d.height);
            return false;
        }
        Platform::TextureDesc td;
        td.width = d.width;
        td.height = d.height;
        td.format = Platform::TextureFormat::R8G8B8A8_UNORM;
        td.initialData = rgba.data();
        td.debugName = "gx-texture";
        d.texture = r.createTexture(td);
        if (!d.texture) {
            PL_LOG_WARN("gx", "GXLoadTexObj: createTexture failed");
            return false;
        }
        d.loaded = true;
    }
    // Sampler is cheap (cached by desc); refresh on every load so LOD/wrap
    // changes made after the first load take effect.
    d.sampler = r.getOrCreateSampler(
        samplerFromGx(d.wrapS, d.wrapT, d.minFilter, d.magFilter));
    return d.sampler != nullptr;
}

} // namespace

// --- PC hooks for GXCompat.cpp ------------------------------------------------

void resolveTexGen(int coordId, float attrs[GX_VA_MAX_ATTR][4]) {
    if (coordId < 0 || coordId >= 8) {
        return;
    }
    const TexGen& g = sTexGen[coordId];
    if (!g.set) {
        return; // pass through the vertex texcoord
    }

    // Source vector (s[0..3], s[3]=1; missing components zero).
    float s[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (g.src >= GX_TG_TEX0 && g.src <= GX_TG_TEX7) {
        const int attr = GX_VA_TEX0 + (g.src - GX_TG_TEX0);
        s[0] = attrs[attr][0];
        s[1] = attrs[attr][1];
        s[2] = attrs[attr][2];
    } else if (g.src == GX_TG_POS) {
        s[0] = attrs[GX_VA_POS][0];
        s[1] = attrs[GX_VA_POS][1];
        s[2] = attrs[GX_VA_POS][2];
    } else {
        // Unsupported source (NRM/BINRM/TANGENT/COLOR...): pass through.
        return;
    }

    // Matrix: GX_IDENTITY (60) or a GX_TEXMTX0..9 register (30,33,...,57).
    float u = s[0], v = s[1];
    int mtx = -1;
    if (g.mtxId == GX_IDENTITY) {
        // identity
    } else if (g.mtxId >= GX_TEXMTX0 && g.mtxId <= GX_TEXMTX9) {
        mtx = static_cast<int>((g.mtxId - GX_TEXMTX0) / 3);
    } else {
        return; // unknown matrix id — pass through
    }
    if (mtx >= 0) {
        const f32(&m)[3][4] = sTexMtx[mtx];
        u = m[0][0] * s[0] + m[0][1] * s[1] + m[0][2] * s[2] + m[0][3];
        v = m[1][0] * s[0] + m[1][1] * s[1] + m[1][2] * s[2] + m[1][3];
        if (g.type == GX_TG_MTX3x4) {
            const f32 w = m[2][0] * s[0] + m[2][1] * s[1] + m[2][2] * s[2] + m[2][3];
            if (w != 0.0f) {
                u /= w;
                v /= w;
            }
        }
    }

    const int outAttr = GX_VA_TEX0 + coordId;
    attrs[outAttr][0] = u;
    attrs[outAttr][1] = v;
}

// Reverse of GDSetTexCoordGen's XF_TEX()/XF_DUALTEX() packing (GDGeometry.h):
// decodes the XF_TEXn register (bits: proj 1, form 2-3, tgType 4-6, row 7-11,
// emboss 12-14, light 15-18) and XF_DUALTEXn (dualmtx 0-7, normalize 8) back
// into the texgen mirror. The main matrix id (GXSetTexCoordGen2's `mtx`) is
// not stored in the XF registers — it keeps its last value.
void dlApplyXfTexGen(int coordId, std::uint32_t xfTex, std::uint32_t xfDual) {
    if (coordId < GX_TEXCOORD0 || coordId > GX_TEXCOORD7) {
        return;
    }
    TexGen& g = sTexGen[coordId];
    const int tgType = static_cast<int>((xfTex >> 4) & 0x7);
    const int row = static_cast<int>((xfTex >> 7) & 0x1F);
    const int proj = static_cast<int>((xfTex >> 1) & 0x1);
    GXTexGenType type = GX_TG_MTX2x4;
    GXTexGenSrc src = GX_TG_TEX0;
    switch (tgType) {
        case 0:
            type = proj ? GX_TG_MTX3x4 : GX_TG_MTX2x4;
            switch (row) {
                case 0: src = GX_TG_POS; break;
                case 1: src = GX_TG_NRM; break;
                case 2: src = GX_TG_COLOR0; break;  // COLOR0/1 share the row
                case 3: src = GX_TG_BINRM; break;
                case 4: src = GX_TG_TANGENT; break;
                default:
                    if (row >= 5 && row <= 12) {
                        src = static_cast<GXTexGenSrc>(GX_TG_TEX0 + (row - 5));
                    }
                    break;
            }
            break;
        case 2:
            type = GX_TG_SRTG;
            src = GX_TG_COLOR0;
            break;
        case 3:
            type = GX_TG_SRTG;
            src = GX_TG_COLOR1;
            break;
        default:
            // EMBOSS (bump) — needs indirect stages (M5.7); store as regular
            // with the source row so resolveTexGen's passthrough applies.
            type = GX_TG_MTX2x4;
            break;
    }
    g.type = type;
    g.src = src;
    g.normalize = ((xfDual >> 8) & 1) != 0;
    g.set = true;
    PL_LOG_TRACE("gx", "dlApplyXfTexGen(coord %d type %d src %d)", coordId,
                 static_cast<int>(type), static_cast<int>(src));
}

void getTexMap0(Platform::TextureHandle* outTex, Platform::SamplerHandle* outSam) {
    if (outTex) *outTex = sTexMapTex[0];
    if (outSam) *outSam = sTexMapSam[0];
}

void getTexMaps(Platform::TextureHandle* outTex, Platform::SamplerHandle* outSam) {
    if (outTex) {
        std::memcpy(outTex, sTexMapTex, sizeof(sTexMapTex));
    }
    if (outSam) {
        std::memcpy(outSam, sTexMapSam, sizeof(sTexMapSam));
    }
}

void getTexMapDims(float outDims[8][2]) {
    std::memcpy(outDims, sTexMapDims, sizeof(sTexMapDims));
}

void resetTextureState() {
    for (auto& g : sTexGen) {
        g = TexGen();
    }
    std::memset(sTexMtx, 0, sizeof(sTexMtx));
    std::memset(sTexMapTex, 0, sizeof(sTexMapTex));
    std::memset(sTexMapSam, 0, sizeof(sTexMapSam));
    std::memset(sTexMapDims, 0, sizeof(sTexMapDims));
}

void shutdownTextures() {
    Platform::Renderer& r = Platform::Renderer::instance();
    if (r.isInitialized()) {
        for (TexObjData* d : sTexObjs) {
            if (d->loaded && d->texture) {
                r.destroyTexture(d->texture);
                d->texture = nullptr;
            }
            d->sampler = nullptr;
            d->loaded = false;
        }
    }
    for (TexObjData* d : sTexObjs) {
        delete d;
    }
    sTexObjs.clear();
    std::memset(sTexMapTex, 0, sizeof(sTexMapTex));
    std::memset(sTexMapSam, 0, sizeof(sTexMapSam));
}

} // namespace Platform::CompatGx

// --- GX API implementations ---------------------------------------------------

extern "C" {

void GXInitTexObj(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXTexFmt format,
                  GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap) {
    using namespace Platform::CompatGx;
    if (!obj) {
        return;
    }
    PL_LOG_TRACE("gx", "GXInitTexObj(%ux%u fmt 0x%x)", width, height,
                 static_cast<unsigned>(format));
    TexObjData* d = texObjData(obj);
    if (d && d->loaded) {
        // Re-init of an already-loaded object: release the renderer texture.
        if (Platform::Renderer::instance().isInitialized()) {
            Platform::Renderer::instance().destroyTexture(d->texture);
        }
        d->loaded = false;
        d->texture = nullptr;
    } else if (!d) {
        d = new TexObjData();
        setTexObjData(obj, d);
        sTexObjs.push_back(d);
    }
    d->image = static_cast<const u8*>(imagePtr);
    d->width = width;
    d->height = height;
    d->format = static_cast<u8>(format);
    d->wrapS = static_cast<u8>(wrapS);
    d->wrapT = static_cast<u8>(wrapT);
    d->mipmap = (mipmap != GX_FALSE);
    d->palette = nullptr;
    d->paletteBytes = 0;
    d->paletteCount = 0;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter minFilter, GXTexFilter magFilter, f32 minLod,
                     f32 maxLod, f32 lodBias, GXBool biasClamp, GXBool edgeLod,
                     GXAnisotropy anisotropy) {
    using namespace Platform::CompatGx;
    TexObjData* d = texObjData(obj);
    if (!d) {
        return;
    }
    PL_LOG_TRACE("gx", "GXInitTexObjLOD(min %d mag %d)", static_cast<int>(minFilter),
                 static_cast<int>(magFilter));
    d->minFilter = minFilter;
    d->magFilter = magFilter;
    d->minLod = minLod;
    d->maxLod = maxLod;
    d->lodBias = lodBias;
    d->biasClamp = biasClamp;
    d->edgeLod = edgeLod;
    d->anisotropy = anisotropy;
    // TODO(PC_PORT, M5.x): mip chain + LOD bias/range on the sampler (Vulkan
    // maxLod/minLod); M5.3 uploads the base level only.
}

void GXInitTexObjCI(GXTexObj* obj, void* imagePtr, u16 width, u16 height, GXCITexFmt format,
                    GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 paletteCount) {
    using namespace Platform::CompatGx;
    if (!obj) {
        return;
    }
    PL_LOG_TRACE("gx", "GXInitTexObjCI(%ux%u fmt 0x%x)", width, height,
                 static_cast<unsigned>(format));
    TexObjData* d = texObjData(obj);
    if (d && d->loaded) {
        if (Platform::Renderer::instance().isInitialized()) {
            Platform::Renderer::instance().destroyTexture(d->texture);
        }
        d->loaded = false;
        d->texture = nullptr;
    } else if (!d) {
        d = new TexObjData();
        setTexObjData(obj, d);
        sTexObjs.push_back(d);
    }
    d->image = static_cast<const u8*>(imagePtr);
    d->width = width;
    d->height = height;
    d->format = static_cast<u8>(format);
    d->wrapS = static_cast<u8>(wrapS);
    d->wrapT = static_cast<u8>(wrapT);
    d->mipmap = (mipmap != GX_FALSE);
    d->paletteCount = paletteCount;
    // The TLUT data itself arrives via GXInitTexObjTlut; until then the
    // object decodes as zeros (TODO(PC_PORT, M5.x): TLUT management).
}

void GXInitTexObjTlut(GXTexObj* /*obj*/, u32 /*tlutName*/) {
    // TODO(PC_PORT, M5.x): TLUT upload/lookup for GX_TF_C4/C8/C14X2. The BTI
    // palette is parsed by the archive loader and passed to btiDecodeToRgba8
    // directly; this entry point is where the game's TLUT id would map to it.
    PL_LOG_WARN("gx", "GXInitTexObjTlut: TLUT management not implemented (M5.x)");
}

void GXLoadTexObj(const GXTexObj* obj, GXTexMapID id) {
    using namespace Platform::CompatGx;
    if (id < GX_TEXMAP0 || id > GX_TEXMAP7) {
        PL_LOG_WARN("gx", "GXLoadTexObj: invalid texmap id %d", static_cast<int>(id));
        return;
    }
    TexObjData* d = texObjData(obj);
    if (!d || !d->image) {
        PL_LOG_WARN("gx", "GXLoadTexObj: texture object not initialized");
        return;
    }
    if (!loadTexture(*d)) {
        return;
    }
    const int slot = static_cast<int>(id) - GX_TEXMAP0;
    sTexMapTex[slot] = d->texture;
    sTexMapSam[slot] = d->sampler;
    sTexMapDims[slot][0] = static_cast<float>(d->width);
    sTexMapDims[slot][1] = static_cast<float>(d->height);
    PL_LOG_TRACE("gx", "GXLoadTexObj -> TEXMAP%d (tex %p, %ux%u)", slot, d->texture,
                 static_cast<unsigned>(d->width), static_cast<unsigned>(d->height));
}

void GXSetTexCoordGen2(GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 mtx,
                       GXBool normalize, u32 /*postMtx*/) {
    using namespace Platform::CompatGx;
    if (coord < GX_TEXCOORD0 || coord > GX_TEXCOORD7) {
        return;
    }
    PL_LOG_TRACE("gx", "GXSetTexCoordGen2(coord %d type %d src %d mtx %u)",
                 static_cast<int>(coord), static_cast<int>(type), static_cast<int>(src),
                 mtx);
    TexGen& g = sTexGen[coord];
    g.type = type;
    g.src = src;
    g.mtxId = mtx;
    g.normalize = (normalize != GX_FALSE);
    g.set = true;
    // postMtx (GX_PTIDENTITY/GX_PTTEXMTX*) not applied in M5.3 (documented).
}

void GXLoadTexMtxImm(const f32 mtx[][4], u32 id, GXTexMtxType /*type*/) {
    using namespace Platform::CompatGx;
    if (id < GX_TEXMTX0 || id > GX_TEXMTX9 || !mtx) {
        PL_LOG_WARN("gx", "GXLoadTexMtxImm: invalid id %u", id);
        return;
    }
    const int idx = static_cast<int>((id - GX_TEXMTX0) / 3);
    std::memcpy(sTexMtx[idx], mtx, sizeof(sTexMtx[idx]));
    PL_LOG_TRACE("gx", "GXLoadTexMtxImm -> TEXMTX%d", idx);
}

} // extern "C"
