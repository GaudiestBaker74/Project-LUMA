// =============================================================================
// compat/gx — BTI (Wii Binary Texture Image) loader + GX tiled-format decoder.
//
// Pure functions (no GX state, no renderer): unit-testable headless.
//
// The Wii stores textures in a hardware swizzle (4x4 texel blocks, except
// CMPR which uses 8x4). GXInitTexObj receives a pointer to the raw tiled
// data; the BTI header (this file) is how JUTTexture locates that data inside
// a JKRArchive. This module parses the 32-byte BTI header and decodes any GX
// texture format to plain row-major RGBA8 — the format the renderer uploads.
//
// Formats (GXTexFmt): I4=0x0, I8=0x1, IA4=0x2, IA8=0x3, RGB565=0x4,
// RGB5A3=0x5, RGBA8=0x6, CMPR=0xE (BC1/DXT1-like), C4=0x8, C8=0x9,
// C14X2=0xA (paletted; TLUT formats IA8/RGB565/RGB5A3).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace Platform::CompatGx {

// BTI header (32 bytes, big-endian). Layout matches the standard Wii format.
struct BtiHeader {
    uint8_t format = 0;         // GXTexFmt (I4..CMPR)
    uint8_t alphaEnabled = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t wrapS = 0;          // GXTexWrapMode (GX_CLAMP/REPEAT/MIRROR)
    uint8_t wrapT = 0;
    uint8_t palettesEnabled = 0;
    uint8_t paletteFormat = 0;  // GXTlutFmt (GX_TL_IA8/RGB565/RGB5A3)
    uint16_t paletteCount = 0;
    uint32_t paletteOffset = 0; // byte offset from BTI start
    float minLod = 0.0f;
    float maxLod = 0.0f;
    uint32_t imageOffset = 0;   // byte offset of the base image from BTI start
};

// Parses the BTI header (big-endian). Returns false if `size` < 32.
bool btiParseHeader(const uint8_t* data, size_t size, BtiHeader& out);

// Exact byte size of a base image in GX tiled layout (w,h must be multiples
// of the format's block size: 4x4, CMPR 8x4).
size_t btiImageSize(uint32_t w, uint32_t h, uint8_t gxFormat);

// Decodes one GX tiled image (w x h) to row-major RGBA8 (w*h*4 bytes).
// `palette` is the TLUT data (required for C4/C8/C14X2; may be null for the
// other formats) and `paletteFormat` its GXTlutFmt (GX_TL_IA8/RGB565/RGB5A3).
// `srcBytes` must be >= btiImageSize(w,h,format).
// Returns false on unknown format / undersized input.
bool btiDecodeToRgba8(const uint8_t* src, size_t srcBytes, uint32_t w, uint32_t h,
                      uint8_t gxFormat, const uint8_t* palette, size_t paletteBytes,
                      uint8_t paletteFormat, uint8_t* rgbaOut);

} // namespace Platform::CompatGx
