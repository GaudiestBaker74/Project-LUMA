// =============================================================================
// compat/gx — EFB copy: GXCopyDisp (present) + GXCopyTex (EFB -> texture) and
// the copy-state configuration APIs (M5.7c, docs/gx.md §J).
//
// Model (PC):
//   * The EFB is an offscreen render target (Platform::Renderer), sized from
//     GXSetDispCopySrc (default 640x448) and created lazily on first use. The
//     frame host renders the scene into it (beginPass(efb)), the game calls
//     GXCopyDisp to present: the renderer blits the EFB into the swapchain
//     image (scaled to the window, LINEAR = the XFB vertical filter
//     simplified) and endFrame() presents. The XFB triple buffer / JUTXfb
//     dance is replaced by the renderer's own present (the swapchain has its
//     own buffering).
//   * GXCopyTex reads the EFB region configured by GXSetTexCopySrc back to
//     CPU memory and encodes it to the GX tiled format configured by
//     GXSetTexCopyDst (the same layout btiDecodeToRgba8 expects), writing it
//     into the game's buffer (allocated with GXGetTexBufferSize). The game
//     then feeds that buffer to GXInitTexObj/GXLoadTexObj like any texture.
//     The readback is synchronous (its own one-shot command buffer + staging
//     buffer + queue wait), so it is safe anywhere in the frame; when called
//     mid-frame it sees the last *submitted* frame's EFB content, not the
//     unflushed current pass (see docs/gx.md §J — TODO(PC_PORT) for the
//     frame-split mechanism that would fix mid-frame captures).
//   * The copy filters (vertical, gamma, clamp, field rendering) are mirrored
//     but not applied: the game assumes gamma 1.0 by default, the vertical
//     filter is approximated by the LINEAR blit, and the EFB is progressive
//     (see docs/gx.md §5). GXSetCopyClear routes to the renderer clear color
//     (the EFB pass clears with it at beginPass — equivalent to the copy-clear
//     clearing the EFB for the next frame).
// =============================================================================

#include "compat/gx/GXCompat.h"

#include "platform/Log/Log.h"
#include "platform/Renderer/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// --- GX enums used below (values from the vendored GXEnum.h). ---------------
// (Referenced by name where the vendored header declares them; the copy-format
// constants GX_TF_* / GX_CTF_* come from GXEnum.h via GXCompat.h.)

namespace {

// --- copy/EFB state mirror --------------------------------------------------

u16 sDispSrcX = 0, sDispSrcY = 0, sDispSrcW = 0, sDispSrcH = 0;   // GXSetDispCopySrc
u16 sDispDstW = 0, sDispDstH = 0;                                 // GXSetDispCopyDst
u16 sTexSrcX = 0, sTexSrcY = 0, sTexSrcW = 0, sTexSrcH = 0;       // GXSetTexCopySrc
u16 sTexDstW = 0, sTexDstH = 0;                                   // GXSetTexCopyDst
GXTexFmt sTexDstFmt = GX_TF_RGBA8;
GXBool sTexDstMipmap = GX_FALSE;
GXColor sCopyClearColor = {0, 0, 0, 0};
u32 sCopyClearZ = 0xFFFFFF;
GXFBClamp sCopyClamp = GX_CLAMP_NONE;
GXBool sCopyFilterAA = GX_FALSE;
GXBool sCopyFilterVFilter = GX_FALSE;
GXGamma sDispCopyGamma = GX_GM_1_0;
GXCopyMode sDispCopyFrame2Field = GX_COPY_PROGRESSIVE;
u32 sDispCopyYScaleReg = 0;

// --- EFB render target (created lazily) -------------------------------------

Platform::RenderTargetHandle sEfbRt = nullptr;
u32 sEfbW = 0, sEfbH = 0;
Platform::TextureFormat sEfbFmt = Platform::TextureFormat::R8G8B8A8_UNORM;

constexpr u32 kDefaultEfbW = 640;
constexpr u32 kDefaultEfbH = 448;

Platform::RenderTargetHandle ensureEfb() {
    Platform::Renderer& r = Platform::Renderer::instance();
    if (!r.isInitialized()) {
        return nullptr;
    }
    u32 w = sDispSrcW ? sDispSrcW : kDefaultEfbW;
    u32 h = sDispSrcH ? sDispSrcH : kDefaultEfbH;
    // The GX pipelines (flushDraw) are built with passColorFormat() (the
    // swapchain format); the EFB must use the same format so the attachments
    // are compatible. Convert VertexFormat (color) -> TextureFormat.
    Platform::TextureFormat fmt = Platform::TextureFormat::R8G8B8A8_UNORM;
    switch (r.passColorFormat()) {
        case Platform::VertexFormat::B8G8R8A8_UNORM:
            fmt = Platform::TextureFormat::B8G8R8A8_UNORM;
            break;
        case Platform::VertexFormat::R32G32B32A32_SFLOAT:
            fmt = Platform::TextureFormat::R32G32B32A32_FLOAT;
            break;
        case Platform::VertexFormat::R8G8B8A8_UNORM:
        default:
            fmt = Platform::TextureFormat::R8G8B8A8_UNORM;
            break;
    }
    if (sEfbRt && sEfbW == w && sEfbH == h && sEfbFmt == fmt) {
        return sEfbRt;
    }
    if (sEfbRt) {
        r.destroyRenderTarget(sEfbRt);
        sEfbRt = nullptr;
    }
    Platform::RenderTargetDesc td;
    td.width = w;
    td.height = h;
    // The GX pipelines (flushDraw) are built with passColorFormat(); the EFB
    // must match so the attachments are compatible.
    td.colorFormat = fmt;
    td.hasDepth = true; // GX Z24X8, like the console EFB
    td.debugName = "gx-efb";
    sEfbRt = r.createRenderTarget(td);
    if (!sEfbRt) {
        PL_LOG_WARN("gx", "ensureEfb: createRenderTarget(%ux%u) failed", w, h);
        return nullptr;
    }
    sEfbW = w;
    sEfbH = h;
    sEfbFmt = fmt;
    PL_LOG_INFO("gx", "EFB render target created: %ux%u (%d)", w, h, static_cast<int>(fmt));
    return sEfbRt;
}

// --- GX tiled format encoding (inverse of Bti.cpp's decoder) -----------------

constexpr uint8_t kFmtI4 = 0x0;
constexpr uint8_t kFmtI8 = 0x1;
constexpr uint8_t kFmtIA4 = 0x2;
constexpr uint8_t kFmtIA8 = 0x3;
constexpr uint8_t kFmtRGB565 = 0x4;
constexpr uint8_t kFmtRGB5A3 = 0x5;
constexpr uint8_t kFmtRGBA8 = 0x6;
constexpr uint8_t kFmtCMPR = 0xE;

inline void putBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

// 8-bit channel -> 5/6-bit GX field (inverse of scale8).
inline uint16_t to5(uint8_t v) { return static_cast<uint16_t>((v * 31u + 127u) / 255u); }
inline uint16_t to6(uint8_t v) { return static_cast<uint16_t>((v * 63u + 127u) / 255u); }

// (r,g,b) -> RGB565 (big-endian), a -> packed RGB5A3. Encoders use the same
// rounding the decoder's scale8() implies, so decode(encode(c)) is close to c.
inline uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>((to5(r) << 11) | (to6(g) << 5) | to5(b));
}
inline uint16_t pack5a3(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a >= 224) {
        return static_cast<uint16_t>(0x8000 | (to5(r) << 10) | (to5(g) << 5) | to5(b));
    }
    const uint16_t a3 = static_cast<uint16_t>((a * 7u + 127u) / 255u);
    const uint16_t r4 = static_cast<uint16_t>((r * 15u + 127u) / 255u);
    const uint16_t g4 = static_cast<uint16_t>((g * 15u + 127u) / 255u);
    const uint16_t b4 = static_cast<uint16_t>((b * 15u + 127u) / 255u);
    return static_cast<uint16_t>((a3 << 12) | (r4 << 8) | (g4 << 4) | b4);
}

// Box-average a 2x2 region of an RGBA8 image (mip generation).
void boxDownsample(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                   uint8_t* dst, uint32_t dstW, uint32_t dstH) {
    for (uint32_t y = 0; y < dstH; ++y) {
        for (uint32_t x = 0; x < dstW; ++x) {
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint32_t sx = std::min(x * 2 + dx, srcW - 1);
                    const uint32_t sy = std::min(y * 2 + dy, srcH - 1);
                    const uint8_t* p = src + (sy * srcW + sx) * 4;
                    for (int c = 0; c < 4; ++c) {
                        acc[c] += p[c];
                    }
                    ++n;
                }
            }
            uint8_t* o = dst + (y * dstW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                o[c] = static_cast<uint8_t>((acc[c] + n / 2) / n);
            }
        }
    }
}

// Encodes one CMPR 4x4 subtile (defined below; forward-declared for
// encodeLevel's CMPR case).
bool encodeCmprSubtile(const uint8_t* rgba, uint32_t w, uint32_t baseX,
                       uint32_t baseY, uint8_t* out);

// Encodes a single (w x h, RGBA8) level into its GX tiled image at `out`.
// w/h must be multiples of 4 (of 8 for CMPR). Returns false on unsupported
// format.
bool encodeLevel(const uint8_t* rgba, uint32_t w, uint32_t h, uint8_t fmt,
                 uint8_t* out) {
    const uint32_t tilesW = w / 4;
    const uint32_t tilesH = h / 4;
    switch (fmt) {
        case kFmtI4: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    const uint8_t lum = static_cast<uint8_t>((p[0] + p[1] + p[2]) / 3);
                    const uint8_t v = std::min<uint8_t>(15, (lum + 8) / 17);
                    uint8_t& byte = out[((y / 4) * tilesW + (x / 4)) * 8 +
                                        (y % 4) * 2 + (x % 4) / 2];
                    if ((x % 4) % 2 == 0) {
                        byte = static_cast<uint8_t>((byte & 0x0F) | (v << 4));
                    } else {
                        byte = static_cast<uint8_t>((byte & 0xF0) | v);
                    }
                }
            }
            return true;
        }
        case kFmtI8: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    out[((y / 4) * tilesW + (x / 4)) * 16 + (y % 4) * 4 + (x % 4)] =
                        static_cast<uint8_t>((p[0] + p[1] + p[2]) / 3);
                }
            }
            return true;
        }
        case kFmtIA4: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    const uint8_t lum = static_cast<uint8_t>((p[0] + p[1] + p[2]) / 3);
                    const uint8_t i = std::min<uint8_t>(15, (lum + 8) / 17);
                    const uint8_t a = std::min<uint8_t>(15, (p[3] + 8) / 17);
                    out[((y / 4) * tilesW + (x / 4)) * 16 + (y % 4) * 4 + (x % 4)] =
                        static_cast<uint8_t>((i << 4) | a);
                }
            }
            return true;
        }
        case kFmtIA8: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    uint8_t* dst = out + ((y / 4) * tilesW + (x / 4)) * 32 +
                                   ((y % 4) * 4 + (x % 4)) * 2;
                    dst[0] = static_cast<uint8_t>((p[0] + p[1] + p[2]) / 3);
                    dst[1] = p[3];
                }
            }
            return true;
        }
        case kFmtRGB565: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    putBe16(out + ((y / 4) * tilesW + (x / 4)) * 32 +
                                ((y % 4) * 4 + (x % 4)) * 2,
                            pack565(p[0], p[1], p[2]));
                }
            }
            return true;
        }
        case kFmtRGB5A3: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    putBe16(out + ((y / 4) * tilesW + (x / 4)) * 32 +
                                ((y % 4) * 4 + (x % 4)) * 2,
                            pack5a3(p[0], p[1], p[2], p[3]));
                }
            }
            return true;
        }
        case kFmtRGBA8: {
            // Two 32-byte planes per 4x4 tile: AR (offset 0) and GB (+32).
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t* p = rgba + (y * w + x) * 4;
                    uint8_t* tile = out + ((y / 4) * tilesW + (x / 4)) * 64;
                    const size_t tOff = (y % 4) * 8 + (x % 4) * 2;
                    tile[tOff] = p[3];       // A
                    tile[tOff + 1] = p[0];   // R
                    tile[32 + tOff] = p[1];  // G
                    tile[32 + tOff + 1] = p[2]; // B
                }
            }
            return true;
        }
        case kFmtCMPR: {
            // 8x8 blocks, subtile order top-left/top-right/bottom-left/
            // bottom-right (offsets 0/8/16/24) — mirrors the decoder.
            const uint32_t tilesW8 = w / 8;
            for (uint32_t by = 0; by < h / 8; ++by) {
                for (uint32_t bx = 0; bx < tilesW8; ++bx) {
                    uint8_t* block = out + (by * tilesW8 + bx) * 32;
                    if (!encodeCmprSubtile(rgba, w, bx * 8, by * 8, block) ||
                        !encodeCmprSubtile(rgba, w, bx * 8 + 4, by * 8, block + 8) ||
                        !encodeCmprSubtile(rgba, w, bx * 8, by * 8 + 4, block + 16) ||
                        !encodeCmprSubtile(rgba, w, bx * 8 + 4, by * 8 + 4, block + 24)) {
                        return false;
                    }
                }
            }
            return true;
        }
        default:
            return false;
    }
}

// Decoder-style color helpers (must match Bti.cpp exactly so the round-trip
// encode->decode is deterministic).
inline uint8_t scaleColor(uint16_t v, uint16_t maxValue) {
    return static_cast<uint8_t>((v * 255u + maxValue / 2u) / maxValue);
}
inline void color565Rgb(uint16_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = scaleColor((v >> 11) & 0x1F, 31);
    *g = scaleColor((v >> 5) & 0x3F, 63);
    *b = scaleColor(v & 0x1F, 31);
}

// RGB565 with the highest nonzero component decremented (for uniform blocks:
// the decoder switches to 3-color+transparent when c0 <= c1, so c1 must be
// strictly darker than c0 — a raw c0-1 would corrupt the other components).
inline uint16_t pack565Darker(uint8_t r, uint8_t g, uint8_t b) {
    if (r > 0) {
        return pack565(static_cast<uint8_t>(r - 1), g, b);
    }
    if (g > 0) {
        return pack565(r, static_cast<uint8_t>(g - 1), b);
    }
    if (b > 0) {
        return pack565(r, g, static_cast<uint8_t>(b - 1));
    }
    return 0x0000; // pure black: c0 == c1 == 0 decodes to black either way
}

// Encodes one CMPR 4x4 subtile starting at (baseX, baseY). Pixels with
// alpha >= 128 are opaque; an all-opaque subtile uses 4-color mode
// (c0 > c1: the two extreme u16 colors), otherwise 3-color + transparent
// (c0 <= c1). Endpoint ordering by u16 value guarantees the decoder picks
// the intended mode with no transparent holes in opaque blocks.
bool encodeCmprSubtile(const uint8_t* rgba, uint32_t w, uint32_t baseX,
                       uint32_t baseY, uint8_t* out) {
    // Gather the subtile pixels; track the min/max RGB565 u16 values and
    // whether any pixel is transparent.
    uint8_t px[16][4];
    bool anyTransparent = false;
    uint16_t maxU16 = 0, minU16 = 0xFFFF;
    for (uint32_t ty = 0; ty < 4; ++ty) {
        for (uint32_t tx = 0; tx < 4; ++tx) {
            const uint8_t* p = rgba + ((baseY + ty) * w + baseX + tx) * 4;
            const int idx = ty * 4 + tx;
            px[idx][0] = p[0];
            px[idx][1] = p[1];
            px[idx][2] = p[2];
            px[idx][3] = p[3];
            if (p[3] < 128) {
                anyTransparent = true;
            }
            const uint16_t v = pack565(p[0], p[1], p[2]);
            maxU16 = std::max(maxU16, v);
            minU16 = std::min(minU16, v);
        }
    }

    uint16_t c0 = 0, c1 = 0;
    bool transparent = false;
    if (!anyTransparent) {
        // 4-color mode (c0 > c1 required). For a uniform block c0 == c1, so
        // darken c1; otherwise c0 = the max-u16 color, c1 = the min.
        if (maxU16 == minU16) {
            c0 = maxU16;
            c1 = pack565Darker(px[0][0], px[0][1], px[0][2]);
        } else {
            c0 = maxU16;
            c1 = minU16;
        }
    } else {
        // 3-color + transparent (c0 <= c1 required): c0 = min, c1 = max.
        c0 = minU16;
        c1 = maxU16;
        transparent = true;
    }

    putBe16(out, c0);
    putBe16(out + 2, c1);

    // Build the decode table in 8-bit space (same math as Bti.cpp).
    uint8_t col[4][3];
    color565Rgb(c0, &col[0][0], &col[0][1], &col[0][2]);
    color565Rgb(c1, &col[1][0], &col[1][1], &col[1][2]);
    if (!transparent) {
        for (int c = 0; c < 3; ++c) {
            col[2][c] = static_cast<uint8_t>((2 * col[0][c] + col[1][c]) / 3);
            col[3][c] = static_cast<uint8_t>((col[0][c] + 2 * col[1][c]) / 3);
        }
    } else {
        for (int c = 0; c < 3; ++c) {
            col[2][c] = static_cast<uint8_t>((col[0][c] + col[1][c]) / 2);
        }
    }

    // Assign each pixel to the nearest color (index 3 = transparent).
    for (uint32_t ty = 0; ty < 4; ++ty) {
        uint8_t rowIndices = 0; // one byte per row, 4 texels of 2 bits
        for (uint32_t tx = 0; tx < 4; ++tx) {
            const int idx = ty * 4 + tx;
            uint8_t best = 0;
            int bestDist = INT32_MAX;
            const int last = transparent ? 2 : 3;
            for (int k = 0; k <= last; ++k) {
                const int dr = int(px[idx][0]) - col[k][0];
                const int dg = int(px[idx][1]) - col[k][1];
                const int db = int(px[idx][2]) - col[k][2];
                const int dist = dr * dr + dg * dg + db * db;
                if (dist < bestDist) {
                    bestDist = dist;
                    best = static_cast<uint8_t>(k);
                }
            }
            if (transparent && px[idx][3] < 128) {
                best = 3;
            }
            rowIndices |= static_cast<uint8_t>((best & 0x3u) << (tx * 2));
        }
        out[4 + ty] = rowIndices; // LSB = leftmost (matches decodeCmprSubtile)
    }
    return true;
}

// Size of one encoded level (base or mip) — same tile math as btiImageSize.
size_t encodedLevelSize(uint32_t w, uint32_t h, uint8_t fmt) {
    const uint32_t tilesW = w / 4;
    const uint32_t tilesH = h / 4;
    switch (fmt) {
        case kFmtI4:
            return static_cast<size_t>(tilesW) * tilesH * 8;
        case kFmtI8:
        case kFmtIA4:
            return static_cast<size_t>(tilesW) * tilesH * 16;
        case kFmtIA8:
        case kFmtRGB565:
        case kFmtRGB5A3:
            return static_cast<size_t>(tilesW) * tilesH * 32;
        case kFmtRGBA8:
            return static_cast<size_t>(tilesW) * tilesH * 64;
        case kFmtCMPR:
            return static_cast<size_t>(w / 8) * (h / 8) * 32;
        default:
            return 0;
    }
}

} // namespace

// --- PC hooks (declared in GXCompat.h) ---------------------------------------

void* Platform::CompatGx::getEfbRenderTarget() {
    return ensureEfb();
}

void Platform::CompatGx::shutdownEfb() {
    if (sEfbRt && Platform::Renderer::instance().isInitialized()) {
        Platform::Renderer::instance().destroyRenderTarget(sEfbRt);
    }
    sEfbRt = nullptr;
    sEfbW = sEfbH = 0;
}

size_t Platform::CompatGx::encodedTexSize(uint32_t w, uint32_t h, uint8_t gxFmt,
                                          bool mipmap) {
    size_t total = encodedLevelSize(w, h, gxFmt);
    if (mipmap) {
        uint32_t mw = w, mh = h;
        // Mip levels down to 4x4 (the tile layout can't represent smaller
        // levels; see header note in GXCopyTex).
        while (mw > 4 || mh > 4) {
            mw = std::max<uint32_t>(mw / 2, 4);
            mh = std::max<uint32_t>(mh / 2, 4);
            total += encodedLevelSize(mw, mh, gxFmt);
        }
    }
    return total;
}

bool Platform::CompatGx::encodeEfbRgbaToGx(const uint8_t* rgba, uint32_t w,
                                           uint32_t h, uint8_t gxFmt,
                                           bool mipmap, uint8_t* out,
                                           size_t outBytes) {
    if (!rgba || !out || w == 0 || h == 0 || (w % 4) != 0 || (h % 4) != 0) {
        return false;
    }
    if (gxFmt == kFmtCMPR && ((w % 8) != 0 || (h % 8) != 0)) {
        return false;
    }
    const size_t need = encodedTexSize(w, h, gxFmt, mipmap);
    if (need == 0 || outBytes < need) {
        return false;
    }

    if (!encodeLevel(rgba, w, h, gxFmt, out)) {
        return false;
    }
    if (mipmap) {
        // Generate the mip chain by box-filtering, encoding each level
        // sequentially after the base level.
        std::vector<uint8_t> cur(rgba, rgba + static_cast<size_t>(w) * h * 4);
        std::vector<uint8_t> next;
        uint32_t cw = w, ch = h;
        size_t offset = encodedLevelSize(w, h, gxFmt);
        while (cw > 4 || ch > 4) {
            const uint32_t nw = std::max<uint32_t>(cw / 2, 4);
            const uint32_t nh = std::max<uint32_t>(ch / 2, 4);
            next.assign(static_cast<size_t>(nw) * nh * 4, 0);
            boxDownsample(cur.data(), cw, ch, next.data(), nw, nh);
            if (!encodeLevel(next.data(), nw, nh, gxFmt, out + offset)) {
                return false;
            }
            offset += encodedLevelSize(nw, nh, gxFmt);
            cur.swap(next);
            cw = nw;
            ch = nh;
        }
    }
    return true;
}

// --- GX copy configuration APIs ----------------------------------------------

void GXSetDispCopySrc(u16 left, u16 top, u16 width, u16 height) {
    PL_LOG_TRACE("gx", "GXSetDispCopySrc(%u, %u, %u, %u)", left, top, width, height);
    sDispSrcX = left;
    sDispSrcY = top;
    sDispSrcW = width;
    sDispSrcH = height;
    // The EFB render target follows the copy source (the game configures the
    // full frame: (0, 0, fbWidth, efbHeight)). Recreate lazily on next use if
    // the size changed.
    if (sEfbRt && (sEfbW != width || sEfbH != height)) {
        Platform::Renderer& r = Platform::Renderer::instance();
        if (r.isInitialized()) {
            r.destroyRenderTarget(sEfbRt);
        }
        sEfbRt = nullptr;
        sEfbW = sEfbH = 0;
    }
}

void GXSetDispCopyDst(u16 width, u16 height) {
    PL_LOG_TRACE("gx", "GXSetDispCopyDst(%u, %u)", width, height);
    sDispDstW = width;
    sDispDstH = height;
}

void GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height) {
    PL_LOG_TRACE("gx", "GXSetTexCopySrc(%u, %u, %u, %u)", left, top, width, height);
    sTexSrcX = left;
    sTexSrcY = top;
    sTexSrcW = width;
    sTexSrcH = height;
}

void GXSetTexCopyDst(u16 width, u16 height, GXTexFmt fmt, GXBool mipmap) {
    PL_LOG_TRACE("gx", "GXSetTexCopyDst(%u, %u, %d, %s)", width, height,
                 static_cast<int>(fmt), mipmap ? "TRUE" : "FALSE");
    sTexDstW = width;
    sTexDstH = height;
    sTexDstFmt = fmt;
    sTexDstMipmap = mipmap;
}

void GXSetDispCopyFrame2Field(GXCopyMode mode) {
    PL_LOG_TRACE("gx", "GXSetDispCopyFrame2Field(%d)", static_cast<int>(mode));
    // The PC EFB is progressive; field-rendering modes are mirrored only.
    sDispCopyFrame2Field = mode;
}

void GXSetCopyClamp(GXFBClamp clamp) {
    PL_LOG_TRACE("gx", "GXSetCopyClamp(%d)", static_cast<int>(clamp));
    // No-op on PC: the blit always covers the whole swapchain image.
    sCopyClamp = clamp;
}

f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
    // SDK formula (GXFrameBuf.c): 256 * xfbHeight / (2 * efbHeight).
    return static_cast<f32>(256 * static_cast<u32>(xfbHeight)) /
           static_cast<f32>(2 * static_cast<u32>(efbHeight));
}

u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) {
    // SDK formula: (2 * efbHeight) * yScale / 256, truncated.
    const u32 n = static_cast<u32>(2 * static_cast<u32>(efbHeight) * yScale / 256.0f);
    return static_cast<u16>(n);
}

u32 GXSetDispCopyYScale(f32 yScale) {
    PL_LOG_TRACE("gx", "GXSetDispCopyYScale(%f)", static_cast<double>(yScale));
    sDispCopyYScaleReg = static_cast<u32>(yScale * 256.0f);
    return sDispCopyYScaleReg;
}

void GXSetCopyClear(GXColor color, u32 zClear) {
    PL_LOG_TRACE("gx", "GXSetCopyClear(%u,%u,%u,%u, z=%u)", color.r, color.g,
                 color.b, color.a, static_cast<unsigned>(zClear));
    sCopyClearColor = color;
    sCopyClearZ = zClear;
    // The EFB pass clears with this color (beginPass(RT)); the copy-clear IS
    // the EFB clear on PC.
    Platform::Renderer& r = Platform::Renderer::instance();
    if (r.isInitialized()) {
        r.setClearColor(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                        color.a / 255.0f);
    }
}

void GXSetCopyFilter(GXBool aa, const u8 samplePattern[12][2], GXBool vFilter,
                     const u8 vfilter[7]) {
    PL_LOG_TRACE("gx", "GXSetCopyFilter(aa=%s, vfilter=%s)", aa ? "TRUE" : "FALSE",
                 vFilter ? "TRUE" : "FALSE");
    (void)samplePattern;
    (void)vfilter;
    // Mirrored only: AA and the vertical filter are approximated by the
    // LINEAR blit (see docs/gx.md §J).
    sCopyFilterAA = aa;
    sCopyFilterVFilter = vFilter;
}

void GXSetDispCopyGamma(GXGamma gamma) {
    PL_LOG_TRACE("gx", "GXSetDispCopyGamma(%d)", static_cast<int>(gamma));
    sDispCopyGamma = gamma;
    if (gamma != GX_GM_1_0) {
        // TODO(PC_PORT): gamma 1.7/2.2 on the copy would need a gamma pass on
        // the blit. The game assumes gamma 1.0 by default (docs/gx.md §5).
        PL_LOG_WARN("gx", "GXSetDispCopyGamma(%d) — only gamma 1.0 supported, "
                          "copy left unadjusted", static_cast<int>(gamma));
    }
}

// --- the copies ---------------------------------------------------------------

void GXCopyDisp(void* dst, GXBool clear) {
    PL_LOG_TRACE("gx", "GXCopyDisp(%p, %s)", dst, clear ? "TRUE" : "FALSE");
    // The XFB pointer is unused: the XFB triple buffer / JUTXfb is replaced by
    // the renderer's swapchain (its own buffering). `clear` is likewise
    // ignored — the EFB pass clears at beginPass (equivalent to the copy-clear
    // clearing the EFB for the next frame).
    (void)dst;
    (void)clear;
    Platform::Renderer& r = Platform::Renderer::instance();
    if (!r.isInitialized()) {
        return;
    }
    if (!ensureEfb()) {
        PL_LOG_WARN("gx", "GXCopyDisp: EFB unavailable — present skipped");
        return;
    }
    const bool blitted = r.blitPassToSwapchain();
    if (!blitted) {
        // E.g. the last pass was the swapchain itself (no EFB): nothing to
        // copy — the frame is presented as rendered.
        PL_LOG_TRACE("gx", "GXCopyDisp: no EFB pass to blit (swapchain pass)");
    }
    // M9.5.3c-diag: the user's boots show a black window while the sandbox
    // shows the EFB content, so report the present chain at INFO: first 5
    // frames, then every 600th (~10 s). blit=NO-PASS + black window would
    // mean the frame never opened/closed the EFB pass (beginFrame skipped or
    // pass ordering); blit=OK shifts the suspicion to vkQueuePresentKHR
    // (see Renderer::endFrame's "present #N" line).
    {
        static u32 sPresentDiagCount = 0;
        const u32 n = sPresentDiagCount++;
        if (n < 5 || (n % 600) == 0) {
            PL_LOG_INFO("gx", "GXCopyDisp #%u: efb=%ux%u blit=%s", n, sEfbW, sEfbH,
                        blitted ? "OK" : "NO-PASS");
        }
    }

    // One-shot diagnostic (M9.5.3c): full EFB readback at present #300 —
    // the layout pane tree dumps say everything is visible/loaded, yet the
    // user's screen stays black, so report what was ACTUALLY rasterized:
    // average color, non-black pixel count and its bounding box (a bbox
    // inside the frame = quads land on screen; empty bbox = nothing drawn).
    static u32 sDispProbeCount = 0;
    if (++sDispProbeCount == 300) {
        r.flushFrame(); // make the readback see THIS frame (docs/gx.md §J)
        const u32 pw = sEfbW;
        const u32 ph = sEfbH;
        std::vector<uint8_t> rgba(static_cast<size_t>(pw) * ph * 4);
        if (r.readRenderTarget(sEfbRt, 0, 0, pw, ph, rgba.data())) {
            u64 nonBlack = 0;
            u64 sumR = 0, sumG = 0, sumB = 0;
            u32 minX = pw, minY = ph, maxX = 0, maxY = 0;
            for (u32 y = 0; y < ph; y++) {
                for (u32 x = 0; x < pw; x++) {
                    const uint8_t* p = &rgba[(static_cast<size_t>(y) * pw + x) * 4];
                    sumR += p[0];
                    sumG += p[1];
                    sumB += p[2];
                    if (p[0] | p[1] | p[2]) {
                        nonBlack++;
                        if (x < minX) minX = x;
                        if (x > maxX) maxX = x;
                        if (y < minY) minY = y;
                        if (y > maxY) maxY = y;
                    }
                }
            }
            const u64 total = static_cast<u64>(pw) * ph;
            PL_LOG_INFO("gx", "EFB probe: %ux%u nonBlack=%llu/%llu avgRGB=(%llu,%llu,%llu) "
                              "bbox=(%u,%u)-(%u,%u)",
                        pw, ph, static_cast<unsigned long long>(nonBlack),
                        static_cast<unsigned long long>(total),
                        static_cast<unsigned long long>(sumR / total),
                        static_cast<unsigned long long>(sumG / total),
                        static_cast<unsigned long long>(sumB / total),
                        nonBlack ? minX : 0, nonBlack ? minY : 0,
                        nonBlack ? maxX : 0, nonBlack ? maxY : 0);
            auto logPx = [&](const char* name, u32 x, u32 y) {
                if (x >= pw || y >= ph) {
                    return;
                }
                const uint8_t* p = &rgba[(static_cast<size_t>(y) * pw + x) * 4];
                PL_LOG_INFO("gx", "EFB probe px %s (%u,%u) = (%u,%u,%u,%u)", name, x, y,
                            static_cast<unsigned>(p[0]), static_cast<unsigned>(p[1]),
                            static_cast<unsigned>(p[2]), static_cast<unsigned>(p[3]));
            };
            logPx("center", pw / 2, ph / 2);
            logPx("tl", 8, 8);
            logPx("tr", pw - 9, 8);
            logPx("bl", 8, ph - 9);
            logPx("br", pw - 9, ph - 9);
        } else {
            PL_LOG_WARN("gx", "EFB probe: readback failed");
        }
    }
    // M8 (compat/vi): the VI retrace callbacks fire here, around the present.
}

void GXCopyTex(void* dst, GXBool clear) {
    PL_LOG_TRACE("gx", "GXCopyTex(%p, %s)", dst, clear ? "TRUE" : "FALSE");
    (void)clear; // same reasoning as GXCopyDisp
    if (!dst) {
        PL_LOG_WARN("gx", "GXCopyTex: null dst — copy dropped");
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();
    if (!r.isInitialized() || !ensureEfb()) {
        return;
    }
    const u32 w = sTexDstW;
    const u32 h = sTexDstH;
    if (w == 0 || h == 0 || (w % 4) != 0 || (h % 4) != 0 ||
        (sTexDstFmt == GX_TF_CMPR && ((w % 8) != 0 || (h % 8) != 0))) {
        PL_LOG_WARN("gx", "GXCopyTex: invalid copy dst %ux%u fmt %d", w, h,
                    static_cast<int>(sTexDstFmt));
        return;
    }
    if (Platform::Renderer::instance().inPass()) {
        // Mid-frame capture: the current pass is not flushed to the GPU yet,
        // so the readback sees the last submitted frame's EFB. TODO(PC_PORT):
        // a mid-pass flush (split the pass) would make this see the current
        // pass (docs/gx.md §J).
        static bool sWarnedMidFrame = false;
        if (!sWarnedMidFrame) {
            PL_LOG_WARN("gx", "GXCopyTex mid-frame: readback sees the last "
                              "submitted frame (mid-pass split not implemented)");
            sWarnedMidFrame = true;
        }
    } else {
        // Between passes (the common case: a pass was just rendered and
        // GXCopyTex captures it): flush the frame command buffer so the
        // synchronous readback below sees that content.
        Platform::Renderer::instance().flushFrame();
    }

    // Read the source region (GXSetTexCopySrc), sized by the copy dst.
    const u32 sx = std::min<u32>(sTexSrcX, sEfbW);
    const u32 sy = std::min<u32>(sTexSrcY, sEfbH);
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    if (!r.readRenderTarget(sEfbRt, sx, sy, w, h, rgba.data())) {
        PL_LOG_WARN("gx", "GXCopyTex: EFB readback failed (%u,%u %ux%u)", sx, sy,
                    w, h);
        return;
    }

    const size_t need = Platform::CompatGx::encodedTexSize(w, h, sTexDstFmt,
                                                           sTexDstMipmap == GX_TRUE);
    if (!Platform::CompatGx::encodeEfbRgbaToGx(rgba.data(), w, h, sTexDstFmt,
                                               sTexDstMipmap == GX_TRUE,
                                               static_cast<uint8_t*>(dst), need)) {
        PL_LOG_WARN("gx", "GXCopyTex: unsupported format 0x%x (%ux%u)",
                    static_cast<unsigned>(sTexDstFmt), w, h);
        return;
    }
    PL_LOG_TRACE("gx", "GXCopyTex: %ux%u fmt 0x%x -> %zu bytes", w, h,
                 static_cast<unsigned>(sTexDstFmt), need);
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap,
                       u8 maxLod) {
    // SDK formula (GXTexture.c): bytes per level = w*h*bpp, summing the mip
    // chain 1..maxLod when mipmap. Matches the tiled layout sizes for the
    // sizes GX allows (multiples of 4 / 8), so buffers sized with this call
    // fit the GXCopyTex output.
    auto levelSize = [&](u32 w, u32 h) -> u32 {
        switch (format) {
            case GX_TF_I4:
            case GX_TF_C4:
                return (w * h) >> 1;
            case GX_TF_I8:
            case GX_TF_C8:
            case GX_TF_IA4:
                return w * h;
            case GX_TF_IA8:
            case GX_TF_RGB565:
            case GX_TF_RGB5A3:
            case GX_TF_C14X2:
                return w * h * 2;
            case GX_TF_RGBA8:
                return w * h * 4;
            case GX_TF_CMPR:
                return (w * h) >> 1;
            default:
                return 0;
        }
    };
    u32 size = levelSize(width, height);
    if (size == 0) {
        return 0;
    }
    if (mipmap) {
        for (u8 i = 1; i <= maxLod; ++i) {
            const u32 w = (width >> i) ? (width >> i) : 1;
            const u32 h = (height >> i) ? (height >> i) : 1;
            size += levelSize(w, h);
        }
    }
    return size;
}

void GXClearBoundingBox(void) {
    PL_LOG_TRACE("gx", "GXClearBoundingBox()");
    // The GP bounding box (GXGetBoundingBox) is not tracked on PC.
}

// --- debug accessor -----------------------------------------------------------

void GXCompatDebugCopyState(GxCopyDebugState& out) {
    out.dispSrcX = sDispSrcX;
    out.dispSrcY = sDispSrcY;
    out.dispSrcW = sDispSrcW;
    out.dispSrcH = sDispSrcH;
    out.dispDstW = sDispDstW;
    out.dispDstH = sDispDstH;
    out.texSrcX = sTexSrcX;
    out.texSrcY = sTexSrcY;
    out.texSrcW = sTexSrcW;
    out.texSrcH = sTexSrcH;
    out.texDstW = sTexDstW;
    out.texDstH = sTexDstH;
    out.texFmt = static_cast<int>(sTexDstFmt);
    out.texMipmap = static_cast<int>(sTexDstMipmap);
    out.copyClearR = sCopyClearColor.r;
    out.copyClearG = sCopyClearColor.g;
    out.copyClearB = sCopyClearColor.b;
    out.copyClearA = sCopyClearColor.a;
    out.copyClearZ = sCopyClearZ;
    out.copyClamp = static_cast<int>(sCopyClamp);
    out.copyFilterAA = static_cast<int>(sCopyFilterAA);
    out.copyFilterVFilter = static_cast<int>(sCopyFilterVFilter);
    out.dispCopyGamma = static_cast<int>(sDispCopyGamma);
    out.dispCopyFrame2Field = static_cast<int>(sDispCopyFrame2Field);
    out.dispCopyYScale = sDispCopyYScaleReg;
    out.efbW = sEfbW;
    out.efbH = sEfbH;
}
