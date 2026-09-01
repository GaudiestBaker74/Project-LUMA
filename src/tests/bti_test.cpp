// =============================================================================
// M5.3: BTI header parse + GX tiled-format decoders (pure, headless).
//
// Synthesizes tiled texture data for every supported GX format and verifies
// the RGBA8 output pixel by pixel, including the hardware swizzle (4x4
// blocks, RGBA8 two-plane, CMPR 8x4 subtiles) and the paletted formats.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/gx/Bti.h"

#include <cstring>
#include <vector>

using namespace Platform::CompatGx;

namespace {

void be16Put(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
void be32Put(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

const uint8_t* pxAt(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t x, uint32_t y) {
    return rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
}
bool pxEq(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t x, uint32_t y,
          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t* p = pxAt(rgba, w, x, y);
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

} // namespace

TEST_CASE(bti_parse_header) {
    uint8_t hdr[32] = {};
    hdr[0] = 0x04; // RGB565
    hdr[1] = 0;    // no alpha
    be16Put(hdr + 2, 64);
    be16Put(hdr + 4, 32);
    hdr[6] = 0; // GX_CLAMP
    hdr[7] = 1; // GX_REPEAT
    hdr[8] = 1;
    hdr[9] = 1; // GX_TL_RGB565
    be16Put(hdr + 10, 16);
    be32Put(hdr + 12, 0x100);
    // minLod/maxLod floats (BE) at +20/+24
    be32Put(hdr + 20, 0x3F800000); // 1.0f
    be32Put(hdr + 24, 0x40000000); // 2.0f
    be32Put(hdr + 28, 0x200);

    BtiHeader h{};
    CHECK(btiParseHeader(hdr, sizeof(hdr), h));
    CHECK(h.format == 0x04);
    CHECK(h.width == 64);
    CHECK(h.height == 32);
    CHECK(h.wrapS == 0);
    CHECK(h.wrapT == 1);
    CHECK(h.palettesEnabled == 1);
    CHECK(h.paletteFormat == 1);
    CHECK(h.paletteCount == 16);
    CHECK(h.paletteOffset == 0x100u);
    CHECK(h.minLod == 1.0f);
    CHECK(h.maxLod == 2.0f);
    CHECK(h.imageOffset == 0x200u);

    // Too small / null.
    CHECK(!btiParseHeader(hdr, 31, h));
    CHECK(!btiParseHeader(nullptr, 32, h));
}

TEST_CASE(bti_image_size_table) {
    CHECK(btiImageSize(4, 4, 0x0) == 8);   // I4
    CHECK(btiImageSize(4, 4, 0x1) == 16);  // I8
    CHECK(btiImageSize(4, 4, 0x2) == 16);  // IA4
    CHECK(btiImageSize(4, 4, 0x3) == 32);  // IA8
    CHECK(btiImageSize(4, 4, 0x4) == 32);  // RGB565
    CHECK(btiImageSize(4, 4, 0x5) == 32);  // RGB5A3
    CHECK(btiImageSize(4, 4, 0x6) == 64);  // RGBA8
    CHECK(btiImageSize(8, 8, 0xE) == 32);  // CMPR (8x8 blocks, w*h/2)
    CHECK(btiImageSize(4, 4, 0x8) == 8);   // C4
    CHECK(btiImageSize(4, 4, 0x9) == 16);  // C8
    CHECK(btiImageSize(4, 4, 0xA) == 32);  // C14X2
    CHECK(btiImageSize(4, 4, 0x7F) == 0);  // unknown
}

TEST_CASE(bti_decode_i8) {
    // 4x4 I8: byte = intensity (row-major within the single block).
    std::vector<uint8_t> src(16);
    for (int i = 0; i < 16; ++i) src[i] = static_cast<uint8_t>(i * 16);
    std::vector<uint8_t> out(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x1, nullptr, 0, 0, out.data()));
    CHECK(pxEq(out, 4, 0, 0, 0, 0, 0, 255));
    CHECK(pxEq(out, 4, 3, 0, 48, 48, 48, 255));
    CHECK(pxEq(out, 4, 0, 3, 192, 192, 192, 255));
    CHECK(pxEq(out, 4, 3, 3, 240, 240, 240, 255));
}

TEST_CASE(bti_decode_i4) {
    // 4x4 I4: 8 bytes, high nibble first.
    std::vector<uint8_t> src(8);
    src[0] = 0x01; // texel(0,0)=0 -> 0; texel(1,0)=1 -> 17
    src[7] = 0xF0; // texel(2,3)=15 -> 255; texel(3,3)=0
    std::vector<uint8_t> out(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x0, nullptr, 0, 0, out.data()));
    CHECK(pxEq(out, 4, 0, 0, 0, 0, 0, 255));
    CHECK(pxEq(out, 4, 1, 0, 17, 17, 17, 255));
    CHECK(pxEq(out, 4, 2, 3, 255, 255, 255, 255));
    CHECK(pxEq(out, 4, 3, 3, 0, 0, 0, 255));
}

TEST_CASE(bti_decode_ia4_ia8) {
    // IA4: high nibble = intensity, low = alpha (both 4-bit).
    {
        std::vector<uint8_t> src(16);
        src[0] = 0xF0; // i=15 -> 255, a=0 -> 0
        src[15] = 0x88; // i=8 -> 136, a=8 -> 136
        std::vector<uint8_t> out(4 * 4 * 4);
        CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x2, nullptr, 0, 0, out.data()));
        CHECK(pxEq(out, 4, 0, 0, 255, 255, 255, 0));
        CHECK(pxEq(out, 4, 3, 3, 136, 136, 136, 136));
    }
    // IA8: (intensity, alpha) byte pair per texel.
    {
        std::vector<uint8_t> src(32);
        src[0] = 200; src[1] = 100; // texel(0,0)
        src[30] = 10; src[31] = 250; // texel(3,3)
        std::vector<uint8_t> out(4 * 4 * 4);
        CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x3, nullptr, 0, 0, out.data()));
        CHECK(pxEq(out, 4, 0, 0, 200, 200, 200, 100));
        CHECK(pxEq(out, 4, 3, 3, 10, 10, 10, 250));
    }
}

TEST_CASE(bti_decode_rgb565) {
    std::vector<uint8_t> src(32);
    be16Put(src.data(), 0xF800); // red
    be16Put(src.data() + 2, 0x07E0); // green
    be16Put(src.data() + 30, 0x001F); // blue
    std::vector<uint8_t> out(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x4, nullptr, 0, 0, out.data()));
    CHECK(pxEq(out, 4, 0, 0, 255, 0, 0, 255));
    CHECK(pxEq(out, 4, 1, 0, 0, 255, 0, 255));
    CHECK(pxEq(out, 4, 3, 3, 0, 0, 255, 255));
}

TEST_CASE(bti_decode_rgb5a3) {
    std::vector<uint8_t> src(32);
    be16Put(src.data(), 0x8000 | (31 << 10) | (0 << 5) | 0); // RGB5 opaque red
    be16Put(src.data() + 2, 0x0000 | (7 << 12) | (15 << 8) | (0 << 4) | 0); // A3(7)->255 alpha, red 15
    std::vector<uint8_t> out(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x5, nullptr, 0, 0, out.data()));
    CHECK(pxEq(out, 4, 0, 0, 255, 0, 0, 255));
    CHECK(pxEq(out, 4, 1, 0, 255, 0, 0, 255));
}

TEST_CASE(bti_decode_rgba8_two_planes) {
    // RGBA8: block = AR plane (32B) + GB plane (32B), each texel 2 bytes.
    std::vector<uint8_t> src(64);
    // texel(0,0): A=255, R=100 (plane 0, offset 0), G=200, B=50 (plane 1).
    src[0] = 255; src[1] = 100;
    src[32 + 0] = 200; src[32 + 1] = 50;
    // texel(3,3): offsets y*8+x*2 = 3*8+3*2 = 30 and 62.
    src[30] = 128; src[31] = 10;
    src[32 + 30] = 20; src[32 + 31] = 30;
    std::vector<uint8_t> out(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x6, nullptr, 0, 0, out.data()));
    CHECK(pxEq(out, 4, 0, 0, 100, 200, 50, 255));
    CHECK(pxEq(out, 4, 3, 3, 10, 20, 30, 128));
}

TEST_CASE(bti_decode_cmpr) {
    // 8x8 CMPR block (32 bytes) = four 4x4 subtiles at offsets 0/8/16/24:
    //   [0] top-left:  c0 = red (0xF800), c1 = blue (0x001F)  -> 4-color ramp
    //   [8] top-right: c0 = green (0x07E0), c1 = white (0xFFFF) -> 3-color+transp
    //   [16] bottom-left:  uniform red (0xF800 / 0xF000)
    //   [24] bottom-right: uniform blue (0x001F / 0x001E)
    std::vector<uint8_t> src(32, 0);
    be16Put(src.data(), 0xF800);
    be16Put(src.data() + 2, 0x001F);
    be16Put(src.data() + 8, 0x07E0);
    be16Put(src.data() + 10, 0xFFFF);
    be16Put(src.data() + 16, 0xF800);
    be16Put(src.data() + 18, 0xF000);
    be16Put(src.data() + 24, 0x001F);
    be16Put(src.data() + 26, 0x001E);
    // Indices: one byte per row, 4 texels of 2 bits, LSB = leftmost.
    // Row 0 of the top subtiles: idx0, idx1, idx2, idx3 (0b11100100).
    src[4] = 0b11'10'01'00;
    src[12] = 0b11'10'01'00;
    // Row 3 of the top-left subtile: all idx0 (red) — exercises the byte
    // that the old 8x4 framing read out of bounds (M5.7c fix).
    src[7] = 0x00;
    // Bottom subtiles: uniform idx0.
    src[20] = 0x00;
    src[28] = 0x00;

    std::vector<uint8_t> out(8 * 8 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 8, 8, 0xE, nullptr, 0, 0, out.data()));
    // Top-left: idx0 = red, idx1 = blue, idx2 = (2r+b)/3, idx3 = (r+2b)/3.
    CHECK(pxEq(out, 8, 0, 0, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 1, 0, 0, 0, 255, 255));
    CHECK(pxEq(out, 8, 2, 0, 170, 0, 85, 255));
    CHECK(pxEq(out, 8, 3, 0, 85, 0, 170, 255));
    // Row 3 (the previously out-of-bounds byte): all idx0 -> red.
    CHECK(pxEq(out, 8, 0, 3, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 3, 3, 255, 0, 0, 255));
    // Top-right: idx0 = green, idx1 = white, idx2 = (g+w)/2, idx3 = transparent.
    CHECK(pxEq(out, 8, 4, 0, 0, 255, 0, 255));
    CHECK(pxEq(out, 8, 5, 0, 255, 255, 255, 255));
    // 8-bit-space interpolation with truncation: (0+255)/2 = 127, (255+255)/2
    // = 255. (Equivalencia visual; the exact rounding differs from Dolphin's
    // 5/6-bit-space math — documented in gx.md §5.)
    CHECK(pxEq(out, 8, 6, 0, 127, 255, 127, 255));
    CHECK(pxEq(out, 8, 7, 0, 0, 0, 0, 0));
    // Bottom-left: uniform red; bottom-right: uniform blue (rows 4-7).
    CHECK(pxEq(out, 8, 0, 7, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 7, 7, 0, 0, 255, 255));
    CHECK(pxEq(out, 8, 7, 4, 0, 0, 255, 255));
}

TEST_CASE(bti_decode_c8_paletted) {
    // C8: byte indices; RGB565 TLUT.
    std::vector<uint8_t> src(16);
    src[0] = 0;
    src[1] = 1;
    src[15] = 1;
    std::vector<uint8_t> pal(4);
    be16Put(pal.data(), 0xF800);       // entry 0: red
    be16Put(pal.data() + 2, 0x001F);   // entry 1: blue
    std::vector<uint8_t> out(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x9, pal.data(), pal.size(), 0x1,
                           out.data()));
    CHECK(pxEq(out, 4, 0, 0, 255, 0, 0, 255));
    CHECK(pxEq(out, 4, 1, 0, 0, 0, 255, 255));
    CHECK(pxEq(out, 4, 3, 3, 0, 0, 255, 255));

    // Missing palette -> zeros (defensive).
    std::vector<uint8_t> out2(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x9, nullptr, 0, 0x1, out2.data()));
    CHECK(pxEq(out2, 4, 0, 0, 0, 0, 0, 0));
}

TEST_CASE(bti_decode_c14x2) {
    std::vector<uint8_t> src(32);
    be16Put(src.data(), 0x0001); // index 1
    std::vector<uint8_t> pal(4);
    be16Put(pal.data(), 0xFFFF); // entry 0 white
    be16Put(pal.data() + 2, 0x001F); // entry 1 blue
    std::vector<uint8_t> out(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0xA, pal.data(), pal.size(), 0x1,
                           out.data()));
    CHECK(pxEq(out, 4, 0, 0, 0, 0, 255, 255));
}

TEST_CASE(bti_decode_rejects_bad_input) {
    std::vector<uint8_t> out(4 * 4 * 4);
    // Unknown format.
    CHECK(!btiDecodeToRgba8(out.data(), 16, 4, 4, 0x7F, nullptr, 0, 0, out.data()));
    // Truncated data.
    std::vector<uint8_t> tiny(4);
    CHECK(!btiDecodeToRgba8(tiny.data(), tiny.size(), 4, 4, 0x1, nullptr, 0, 0, out.data()));
    // Non-multiple-of-4 dimensions.
    CHECK(!btiDecodeToRgba8(tiny.data(), 16, 5, 4, 0x1, nullptr, 0, 0, out.data()));
    // Null out.
    CHECK(!btiDecodeToRgba8(tiny.data(), 16, 4, 4, 0x1, nullptr, 0, 0, nullptr));
}
