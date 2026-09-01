// =============================================================================
// compat/gx — BTI decoder implementation (see Bti.h).
// =============================================================================

#include "compat/gx/Bti.h"

#include <cstring>

namespace Platform::CompatGx {

namespace {

constexpr uint8_t kFmtI4 = 0x0;
constexpr uint8_t kFmtI8 = 0x1;
constexpr uint8_t kFmtIA4 = 0x2;
constexpr uint8_t kFmtIA8 = 0x3;
constexpr uint8_t kFmtRGB565 = 0x4;
constexpr uint8_t kFmtRGB5A3 = 0x5;
constexpr uint8_t kFmtRGBA8 = 0x6;
constexpr uint8_t kFmtC4 = 0x8;
constexpr uint8_t kFmtC8 = 0x9;
constexpr uint8_t kFmtC14X2 = 0xA;
constexpr uint8_t kFmtCMPR = 0xE;

constexpr uint8_t kTlutIA8 = 0x0;
constexpr uint8_t kTlutRGB565 = 0x1;
constexpr uint8_t kTlutRGB5A3 = 0x2;

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline float beF32(const uint8_t* p) {
    uint32_t v = be32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// Scales a fixed-point channel (0..maxValue) to 0..255 with rounding.
inline uint8_t scale8(uint32_t v, uint32_t maxValue) {
    return static_cast<uint8_t>((v * 255u + maxValue / 2u) / maxValue);
}

inline void putPixel(uint8_t* rgba, uint32_t x, uint32_t y, uint32_t w, uint8_t r,
                     uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* p = rgba + (static_cast<size_t>(y) * w + x) * 4;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

struct Rgb8 {
    uint8_t r, g, b;
};

// Decodes a 16-bit GX color (RGB565 or RGB5A3) to 8-bit RGB.
inline Rgb8 color565(uint16_t v) {
    Rgb8 c;
    c.r = scale8((v >> 11) & 0x1F, 31);
    c.g = scale8((v >> 5) & 0x3F, 63);
    c.b = scale8(v & 0x1F, 31);
    return c;
}
inline Rgb8 color5a3(uint16_t v) {
    Rgb8 c;
    if (v & 0x8000) { // RGB5, opaque
        c.r = scale8((v >> 10) & 0x1F, 31);
        c.g = scale8((v >> 5) & 0x1F, 31);
        c.b = scale8(v & 0x1F, 31);
    } else { // A3 + RGB4
        c.r = scale8((v >> 8) & 0xF, 15);
        c.g = scale8((v >> 4) & 0xF, 15);
        c.b = scale8(v & 0xF, 15);
    }
    return c;
}
inline uint8_t alpha5a3(uint16_t v) {
    if (v & 0x8000) {
        return 255;
    }
    return scale8((v >> 12) & 0x7, 7);
}

// --- CMPR (BC1/DXT1-like) ----------------------------------------------------
// Standard GX CMPR/DXT1 framing: an 8x8 block = four 4x4 subtiles (8 bytes
// each: 2-byte color0, 2-byte color1 in RGB565 BE, then 4 bytes of 2-bit
// indices — one byte per row, LSB = leftmost). Color table: c0>c1 -> 4-color
// ramp; else -> 3-color + transparent.
void decodeCmprSubtile(const uint8_t* sub, uint8_t* rgbaOut, uint32_t w,
                       uint32_t baseX, uint32_t baseY) {
    const uint16_t c0 = be16(sub);
    const uint16_t c1 = be16(sub + 2);
    Rgb8 col[4];
    bool transparent = false;
    if (c0 > c1) {
        col[0] = color565(c0);
        col[1] = color565(c1);
        col[2].r = static_cast<uint8_t>((2 * col[0].r + col[1].r) / 3);
        col[2].g = static_cast<uint8_t>((2 * col[0].g + col[1].g) / 3);
        col[2].b = static_cast<uint8_t>((2 * col[0].b + col[1].b) / 3);
        col[3].r = static_cast<uint8_t>((col[0].r + 2 * col[1].r) / 3);
        col[3].g = static_cast<uint8_t>((col[0].g + 2 * col[1].g) / 3);
        col[3].b = static_cast<uint8_t>((col[0].b + 2 * col[1].b) / 3);
    } else {
        col[0] = color565(c0);
        col[1] = color565(c1);
        col[2].r = static_cast<uint8_t>((col[0].r + col[1].r) / 2);
        col[2].g = static_cast<uint8_t>((col[0].g + col[1].g) / 2);
        col[2].b = static_cast<uint8_t>((col[0].b + col[1].b) / 2);
        transparent = true;
    }
    // Standard BC1 index layout: one byte per row (4 texels of 2 bits,
    // LSB = leftmost). (M5.7c: was `sub[4 + ty*4 + tx/4]` — row 3's byte fell
    // at offset 16, past the 8-byte subtile, corrupting the next block.)
    for (uint32_t ty = 0; ty < 4; ++ty) {
        const uint8_t byte = sub[4 + ty];
        for (uint32_t tx = 0; tx < 4; ++tx) {
            const uint8_t idx = static_cast<uint8_t>((byte >> (tx * 2)) & 0x3);
            const uint32_t x = baseX + tx;
            const uint32_t y = baseY + ty;
            if (transparent && idx == 3) {
                putPixel(rgbaOut, x, y, w, 0, 0, 0, 0);
            } else {
                putPixel(rgbaOut, x, y, w, col[idx].r, col[idx].g, col[idx].b, 255);
            }
        }
    }
}

// --- paletted formats --------------------------------------------------------
// Looks up palette entry `idx` (16-bit TLUT value) and writes RGBA8.
void putPalette(uint8_t* rgbaOut, uint32_t x, uint32_t y, uint32_t w,
                const uint8_t* palette, size_t paletteBytes, uint32_t idx,
                uint8_t paletteFormat) {
    if (!palette || (static_cast<size_t>(idx) + 1) * 2 > paletteBytes) {
        putPixel(rgbaOut, x, y, w, 0, 0, 0, 0);
        return;
    }
    const uint16_t v = be16(palette + static_cast<size_t>(idx) * 2);
    switch (paletteFormat) {
        case kTlutIA8: {
            const uint8_t i = static_cast<uint8_t>(v >> 8);
            const uint8_t a = static_cast<uint8_t>(v & 0xFF);
            putPixel(rgbaOut, x, y, w, i, i, i, a);
            break;
        }
        case kTlutRGB565: {
            const Rgb8 c = color565(v);
            putPixel(rgbaOut, x, y, w, c.r, c.g, c.b, 255);
            break;
        }
        case kTlutRGB5A3: {
            const Rgb8 c = color5a3(v);
            putPixel(rgbaOut, x, y, w, c.r, c.g, c.b, alpha5a3(v));
            break;
        }
        default:
            putPixel(rgbaOut, x, y, w, 0, 0, 0, 0);
            break;
    }
}

} // namespace

bool btiParseHeader(const uint8_t* data, size_t size, BtiHeader& out) {
    if (!data || size < 32) {
        return false;
    }
    out.format = data[0];
    out.alphaEnabled = data[1];
    out.width = be16(data + 2);
    out.height = be16(data + 4);
    out.wrapS = data[6];
    out.wrapT = data[7];
    out.palettesEnabled = data[8];
    out.paletteFormat = data[9];
    out.paletteCount = be16(data + 10);
    out.paletteOffset = be32(data + 12);
    out.minLod = beF32(data + 20);
    out.maxLod = beF32(data + 24);
    out.imageOffset = be32(data + 28);
    return true;
}

size_t btiImageSize(uint32_t w, uint32_t h, uint8_t gxFormat) {
    const uint32_t tilesW = w / 4;
    const uint32_t tilesH = h / 4;
    switch (gxFormat) {
        case kFmtI4:
        case kFmtC4:
            return static_cast<size_t>(tilesW) * tilesH * 8;
        case kFmtI8:
        case kFmtIA4:
        case kFmtC8:
            return static_cast<size_t>(tilesW) * tilesH * 16;
        case kFmtIA8:
        case kFmtRGB565:
        case kFmtRGB5A3:
        case kFmtC14X2:
            return static_cast<size_t>(tilesW) * tilesH * 32;
        case kFmtRGBA8:
            return static_cast<size_t>(tilesW) * tilesH * 64;
        case kFmtCMPR:
            // Standard GX/DXT1: 8x8 blocks of 32 bytes (4 subtiles of 8).
            // Requires w%8==0 and h%8==0; the size is w*h/2 (matches
            // GXGetTexBufferSize). (M5.7c: corrected from the original 8x4
            // block framing, which doubled the size and misaligned row 3.)
            return static_cast<size_t>(w / 8) * (h / 8) * 32;
        default:
            return 0;
    }
}

bool btiDecodeToRgba8(const uint8_t* src, size_t srcBytes, uint32_t w, uint32_t h,
                      uint8_t gxFormat, const uint8_t* palette, size_t paletteBytes,
                      uint8_t paletteFormat, uint8_t* rgbaOut) {
    if (!src || !rgbaOut || w == 0 || h == 0 || (w % 4) != 0 || (h % 4) != 0) {
        return false;
    }
    const size_t need = btiImageSize(w, h, gxFormat);
    if (need == 0 || srcBytes < need) {
        return false;
    }

    const uint32_t tilesW = w / 4;

    switch (gxFormat) {
        case kFmtI4:
        case kFmtC4: {
            const bool paletted = (gxFormat == kFmtC4);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t blockOff =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * 8;
                    const uint8_t byte = src[blockOff + (y % 4) * 2 + (x % 4) / 2];
                    const uint8_t v = ((x % 4) % 2 == 0) ? static_cast<uint8_t>(byte >> 4)
                                                         : static_cast<uint8_t>(byte & 0xF);
                    if (paletted) {
                        putPalette(rgbaOut, x, y, w, palette, paletteBytes, v,
                                   paletteFormat);
                    } else {
                        const uint8_t g = static_cast<uint8_t>(v * 17);
                        putPixel(rgbaOut, x, y, w, g, g, g, 255);
                    }
                }
            }
            break;
        }
        case kFmtI8:
        case kFmtC8: {
            const bool paletted = (gxFormat == kFmtC8);
            const size_t blockBytes = 16;
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t off =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * blockBytes +
                        (y % 4) * 4 + (x % 4);
                    if (paletted) {
                        putPalette(rgbaOut, x, y, w, palette, paletteBytes, src[off],
                                   paletteFormat);
                    } else {
                        const uint8_t g = src[off];
                        putPixel(rgbaOut, x, y, w, g, g, g, 255);
                    }
                }
            }
            break;
        }
        case kFmtC14X2: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t off =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * 32 +
                        ((y % 4) * 4 + (x % 4)) * 2;
                    const uint32_t idx = be16(src + off) & 0x3FFF;
                    putPalette(rgbaOut, x, y, w, palette, paletteBytes, idx,
                               paletteFormat);
                }
            }
            break;
        }
        case kFmtIA4: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t off =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * 16 +
                        (y % 4) * 4 + (x % 4);
                    const uint8_t b = src[off];
                    const uint8_t i = static_cast<uint8_t>((b >> 4) * 17);
                    const uint8_t a = static_cast<uint8_t>((b & 0xF) * 17);
                    putPixel(rgbaOut, x, y, w, i, i, i, a);
                }
            }
            break;
        }
        case kFmtIA8: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t off =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * 32 +
                        ((y % 4) * 4 + (x % 4)) * 2;
                    const uint8_t i = src[off];
                    const uint8_t a = src[off + 1];
                    putPixel(rgbaOut, x, y, w, i, i, i, a);
                }
            }
            break;
        }
        case kFmtRGB565: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t off =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * 32 +
                        ((y % 4) * 4 + (x % 4)) * 2;
                    const Rgb8 c = color565(be16(src + off));
                    putPixel(rgbaOut, x, y, w, c.r, c.g, c.b, 255);
                }
            }
            break;
        }
        case kFmtRGB5A3: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t off =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * 32 +
                        ((y % 4) * 4 + (x % 4)) * 2;
                    const uint16_t v = be16(src + off);
                    const Rgb8 c = color5a3(v);
                    putPixel(rgbaOut, x, y, w, c.r, c.g, c.b, alpha5a3(v));
                }
            }
            break;
        }
        case kFmtRGBA8: {
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t blockOff =
                        static_cast<size_t>((y / 4) * tilesW + (x / 4)) * 64;
                    const size_t tOff = (y % 4) * 8 + (x % 4) * 2;
                    const uint8_t a = src[blockOff + tOff];
                    const uint8_t r = src[blockOff + tOff + 1];
                    const uint8_t g = src[blockOff + 32 + tOff];
                    const uint8_t b = src[blockOff + 32 + tOff + 1];
                    putPixel(rgbaOut, x, y, w, r, g, b, a);
                }
            }
            break;
        }
        case kFmtCMPR: {
            // 8x8 blocks: subtile order is top-left, top-right, bottom-left,
            // bottom-right (offsets 0/8/16/24).
            const uint32_t tilesW8 = w / 8;
            for (uint32_t by = 0; by < h / 8; ++by) {
                for (uint32_t bx = 0; bx < tilesW8; ++bx) {
                    const size_t blockOff = (static_cast<size_t>(by) * tilesW8 + bx) * 32;
                    decodeCmprSubtile(src + blockOff, rgbaOut, w, bx * 8, by * 8);
                    decodeCmprSubtile(src + blockOff + 8, rgbaOut, w, bx * 8 + 4, by * 8);
                    decodeCmprSubtile(src + blockOff + 16, rgbaOut, w, bx * 8, by * 8 + 4);
                    decodeCmprSubtile(src + blockOff + 24, rgbaOut, w, bx * 8 + 4, by * 8 + 4);
                }
            }
            break;
        }
        default:
            return false;
    }
    return true;
}

} // namespace Platform::CompatGx
