// =============================================================================
// M5.3: BTI header parse + GX tiled-format decoders (pure, headless).
//
// Synthesizes tiled texture data for every supported GX format and verifies
// the RGBA8 output pixel by pixel, including the hardware swizzle (8x8 tiles
// for I4/C4/CMPR, 8x4 for I8/IA4/C8, 4x4 for the 16/32-bit formats, RGBA8
// two-plane, CMPR 4x4 subtiles with MSB-first indices) and the paletted
// formats. The expected values follow the GX hardware (Dolphin's
// TextureDecoder) — NOT the encoder in GXCopy.cpp — so an encoder/decoder pair
// that agrees on a wrong layout cannot pass these (PC_PORT M9.5.4).
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

// PC_PORT M9.5.4: the 4/8-bit formats tile as 8x8 (I4/C4) and 8x4 (I8/IA4/C8),
// 32 bytes per tile (RVL_SDK __GXGetTexTileShift). The first version of these
// tests (and of the decoder) assumed 4x4 tiles for every format; the byte
// totals coincide, so nothing noticed until real disc textures rendered as
// stripes/noise on the title screen (PicBloom/PicLogoShine I8, PicNintendo
// IA4, PicTitleLogoJpJa I4).
TEST_CASE(bti_image_size_table) {
    CHECK(btiImageSize(8, 8, 0x0) == 32);  // I4: one 8x8 tile
    CHECK(btiImageSize(8, 4, 0x1) == 32);  // I8: one 8x4 tile
    CHECK(btiImageSize(8, 4, 0x2) == 32);  // IA4: one 8x4 tile
    CHECK(btiImageSize(4, 4, 0x3) == 32);  // IA8
    CHECK(btiImageSize(4, 4, 0x4) == 32);  // RGB565
    CHECK(btiImageSize(4, 4, 0x5) == 32);  // RGB5A3
    CHECK(btiImageSize(4, 4, 0x6) == 64);  // RGBA8
    CHECK(btiImageSize(8, 8, 0xE) == 32);  // CMPR (8x8 blocks, w*h/2)
    CHECK(btiImageSize(8, 8, 0x8) == 32);  // C4: one 8x8 tile
    CHECK(btiImageSize(8, 4, 0x9) == 32);  // C8: one 8x4 tile
    CHECK(btiImageSize(4, 4, 0xA) == 32);  // C14X2
    CHECK(btiImageSize(4, 4, 0x7F) == 0);  // unknown
    // Whole-tile padding, like GXGetTexBufferSize: a 4x4 I4 image still
    // occupies one 32-byte 8x8 tile; the title's 328x32 I4 strip = 41x4 tiles.
    CHECK(btiImageSize(4, 4, 0x0) == 32);
    CHECK(btiImageSize(328, 32, 0x0) == 41u * 4u * 32u);
    CHECK(btiImageSize(24, 16, 0x0) == 3u * 2u * 32u);   // PicTM
    CHECK(btiImageSize(288, 176, 0x1) == 36u * 44u * 32u); // PicBloomA (I8)
    CHECK(btiImageSize(168, 24, 0x2) == 21u * 6u * 32u);   // PicNintendo (IA4)
}

TEST_CASE(bti_decode_i8) {
    // 16x8 I8 = 2x2 tiles of 8x4 texels (32 B each, row-major inside a tile,
    // tiles left-to-right then top-to-bottom). Byte value = tile*64 + index.
    std::vector<uint8_t> src(4 * 32);
    for (int t = 0; t < 4; ++t) {
        for (int i = 0; i < 32; ++i) src[t * 32 + i] = static_cast<uint8_t>(t * 64 + i);
    }
    std::vector<uint8_t> out(16 * 8 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 16, 8, 0x1, nullptr, 0, 0, out.data()));
    // GX replicates the intensity into all four channels (A = I).
    CHECK(pxEq(out, 16, 0, 0, 0, 0, 0, 0));            // tile 0, texel (0,0)
    CHECK(pxEq(out, 16, 7, 0, 7, 7, 7, 7));            // tile 0, texel (7,0)
    CHECK(pxEq(out, 16, 0, 1, 8, 8, 8, 8));            // tile 0, row 1 starts at byte 8
    CHECK(pxEq(out, 16, 7, 3, 31, 31, 31, 31));        // tile 0, last texel
    CHECK(pxEq(out, 16, 8, 0, 64, 64, 64, 64));        // tile 1 (right)
    CHECK(pxEq(out, 16, 0, 4, 128, 128, 128, 128));    // tile 2 (below)
    CHECK(pxEq(out, 16, 15, 7, 192 + 31, 192 + 31, 192 + 31, 192 + 31)); // tile 3 end
}

TEST_CASE(bti_decode_i4) {
    // 16x16 I4 = 2x2 tiles of 8x8 texels (32 B each: 8 rows x 4 bytes, high
    // nibble = even x).
    std::vector<uint8_t> src(4 * 32, 0);
    src[0] = 0x01;              // tile 0: texel(0,0)=0, texel(1,0)=1
    src[4] = 0x20;              // tile 0: row 1 -> texel(0,1)=2
    src[31] = 0xF0;             // tile 0: row 7, byte 3 -> texel(6,7)=15, texel(7,7)=0
    src[32] = 0x30;             // tile 1 (x 8..15, y 0..7): texel(8,0)=3
    src[64] = 0x40;             // tile 2 (x 0..7, y 8..15): texel(0,8)=4
    src[96 + 31] = 0x0F;        // tile 3: texel(15,15)=15
    std::vector<uint8_t> out(16 * 16 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 16, 16, 0x0, nullptr, 0, 0, out.data()));
    CHECK(pxEq(out, 16, 0, 0, 0, 0, 0, 0));            // A = I, like GX
    CHECK(pxEq(out, 16, 1, 0, 17, 17, 17, 17));
    CHECK(pxEq(out, 16, 0, 1, 34, 34, 34, 34));
    CHECK(pxEq(out, 16, 6, 7, 255, 255, 255, 255));
    CHECK(pxEq(out, 16, 7, 7, 0, 0, 0, 0));
    CHECK(pxEq(out, 16, 8, 0, 51, 51, 51, 51));
    CHECK(pxEq(out, 16, 0, 8, 68, 68, 68, 68));
    CHECK(pxEq(out, 16, 15, 15, 255, 255, 255, 255));
    // A 4x4 I4 image still lives in a padded 8x8 tile (row stride 4 bytes).
    std::vector<uint8_t> small(32, 0);
    small[0] = 0x0F;  // texel(1,0)=15
    small[12] = 0xF0; // row 3 -> texel(0,3)=15
    std::vector<uint8_t> out4(4 * 4 * 4);
    CHECK(btiDecodeToRgba8(small.data(), small.size(), 4, 4, 0x0, nullptr, 0, 0, out4.data()));
    CHECK(pxEq(out4, 4, 1, 0, 255, 255, 255, 255));
    CHECK(pxEq(out4, 4, 0, 3, 255, 255, 255, 255));
    CHECK(pxEq(out4, 4, 0, 0, 0, 0, 0, 0));
}

TEST_CASE(bti_decode_ia4_ia8) {
    // IA4: 8x4 tiles; GX byte = ALPHA in the high nibble, intensity in the
    // low nibble (Dolphin DecodeBytes_IA4).
    {
        std::vector<uint8_t> src(2 * 32, 0);   // 16x4 = two tiles side by side
        src[0] = 0xF0;      // texel(0,0): a=15 -> 255, i=0 -> 0
        src[8] = 0x88;      // texel(0,1): row 1 starts at byte 8
        src[31] = 0x1F;     // texel(7,3): a=1 -> 17, i=15 -> 255
        src[32] = 0xA5;     // texel(8,0): second tile
        std::vector<uint8_t> out(16 * 4 * 4);
        CHECK(btiDecodeToRgba8(src.data(), src.size(), 16, 4, 0x2, nullptr, 0, 0, out.data()));
        CHECK(pxEq(out, 16, 0, 0, 0, 0, 0, 255));
        CHECK(pxEq(out, 16, 0, 1, 136, 136, 136, 136));
        CHECK(pxEq(out, 16, 7, 3, 255, 255, 255, 17));
        CHECK(pxEq(out, 16, 8, 0, 85, 85, 85, 170));
    }
    // IA8: 4x4 tiles; (alpha, intensity) byte pair per texel — alpha FIRST
    // (Dolphin DecodePixel_IA8: i = val >> 8 of the little-endian load).
    {
        std::vector<uint8_t> src(32);
        src[0] = 200; src[1] = 100; // texel(0,0): a=200, i=100
        src[30] = 10; src[31] = 250; // texel(3,3): a=10, i=250
        std::vector<uint8_t> out(4 * 4 * 4);
        CHECK(btiDecodeToRgba8(src.data(), src.size(), 4, 4, 0x3, nullptr, 0, 0, out.data()));
        CHECK(pxEq(out, 4, 0, 0, 100, 100, 100, 200));
        CHECK(pxEq(out, 4, 3, 3, 250, 250, 250, 10));
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
    // Indices: one byte per row, 4 texels of 2 bits, MSB pair = LEFTMOST
    // texel (GX packs them big-endian, unlike PC DXT1 — Dolphin
    // DecodeDXTBlock reads `(val >> 6) & 3` then shifts left).
    // Row 0 of the top subtiles: idx0, idx1, idx2, idx3 (0b00011011).
    src[4] = 0b00'01'10'11;
    src[12] = 0b00'01'10'11;
    // Row 3 of the top-left subtile: all idx0 (red) — exercises the byte
    // that the old 8x4 framing read out of bounds (M5.7c fix).
    src[7] = 0x00;
    // Bottom subtiles: uniform idx0, except row 1 of the bottom-left one,
    // where ONLY the leftmost texel is idx1 (0b01000000): with LSB-first
    // reading it would land on texel 3 instead.
    src[20] = 0x00;
    src[21] = 0b01'00'00'00;
    src[28] = 0x00;

    std::vector<uint8_t> out(8 * 8 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 8, 8, 0xE, nullptr, 0, 0, out.data()));
    // Top-left: idx0 = red, idx1 = blue, idx2 = (5r+3b)/8, idx3 = (3r+5b)/8
    // (GX blends 5/8-3/8, not 2/3-1/3: 255*5/8 = 159, 255*3/8 = 95).
    CHECK(pxEq(out, 8, 0, 0, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 1, 0, 0, 0, 255, 255));
    CHECK(pxEq(out, 8, 2, 0, 159, 0, 95, 255));
    CHECK(pxEq(out, 8, 3, 0, 95, 0, 159, 255));
    // Row 3 (the previously out-of-bounds byte): all idx0 -> red.
    CHECK(pxEq(out, 8, 0, 3, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 3, 3, 255, 0, 0, 255));
    // Top-right: idx0 = green, idx1 = white, idx2 = (g+w)/2, idx3 = the same
    // average color but with alpha 0 (GX keeps the color; PC DXT1 would give
    // transparent black — matters for bilinear fringes).
    CHECK(pxEq(out, 8, 4, 0, 0, 255, 0, 255));
    CHECK(pxEq(out, 8, 5, 0, 255, 255, 255, 255));
    // 8-bit-space interpolation with truncation: (0+255)/2 = 127, (255+255)/2
    // = 255. (Equivalencia visual; the exact rounding differs from Dolphin's
    // 5/6-bit-space math — documented in gx.md §5.)
    CHECK(pxEq(out, 8, 6, 0, 127, 255, 127, 255));
    CHECK(pxEq(out, 8, 7, 0, 127, 255, 127, 0));
    // Bottom-left: uniform red; bottom-right: uniform blue (rows 4-7).
    CHECK(pxEq(out, 8, 0, 7, 255, 0, 0, 255));
    // Row 5 (subtile row 1): texel 0 = idx1 (0xF000 -> dark red), texel 3 = idx0.
    CHECK(pxEq(out, 8, 0, 5, 247, 0, 0, 255));
    CHECK(pxEq(out, 8, 3, 5, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 1, 5, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 7, 7, 0, 0, 255, 255));
    CHECK(pxEq(out, 8, 7, 4, 0, 0, 255, 255));
}

TEST_CASE(bti_decode_c8_paletted) {
    // C8: byte indices in 8x4 tiles; RGB565 TLUT. 8x4 image = one tile.
    std::vector<uint8_t> src(32);
    src[0] = 0;
    src[1] = 1;
    src[31] = 1;  // texel(7,3)
    std::vector<uint8_t> pal(4);
    be16Put(pal.data(), 0xF800);       // entry 0: red
    be16Put(pal.data() + 2, 0x001F);   // entry 1: blue
    std::vector<uint8_t> out(8 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 8, 4, 0x9, pal.data(), pal.size(), 0x1,
                           out.data()));
    CHECK(pxEq(out, 8, 0, 0, 255, 0, 0, 255));
    CHECK(pxEq(out, 8, 1, 0, 0, 0, 255, 255));
    CHECK(pxEq(out, 8, 7, 3, 0, 0, 255, 255));

    // C4: nibble indices in 8x8 tiles (high nibble = even x).
    std::vector<uint8_t> src4(32, 0);
    src4[0] = 0x10;   // texel(0,0)=1, texel(1,0)=0
    src4[28] = 0x01;  // row 7 -> texel(1,7)=1
    std::vector<uint8_t> out4(8 * 8 * 4);
    CHECK(btiDecodeToRgba8(src4.data(), src4.size(), 8, 8, 0x8, pal.data(), pal.size(), 0x1,
                           out4.data()));
    CHECK(pxEq(out4, 8, 0, 0, 0, 0, 255, 255));
    CHECK(pxEq(out4, 8, 1, 0, 255, 0, 0, 255));
    CHECK(pxEq(out4, 8, 1, 7, 0, 0, 255, 255));

    // IA8 TLUT (GX_TL_IA8 = 0): entry = (alpha, intensity) bytes, alpha
    // FIRST, like GX_TF_IA8 (PC_PORT M9.5.4 — was read swapped).
    std::vector<uint8_t> palIa(4);
    palIa[0] = 0x40; palIa[1] = 0xC0;   // entry 0: a=64, i=192
    palIa[2] = 0xFF; palIa[3] = 0x10;   // entry 1: a=255, i=16
    std::vector<uint8_t> outIa(8 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 8, 4, 0x9, palIa.data(), palIa.size(), 0x0,
                           outIa.data()));
    CHECK(pxEq(outIa, 8, 0, 0, 192, 192, 192, 64));
    CHECK(pxEq(outIa, 8, 1, 0, 16, 16, 16, 255));

    // Missing palette -> zeros (defensive).
    std::vector<uint8_t> out2(8 * 4 * 4);
    CHECK(btiDecodeToRgba8(src.data(), src.size(), 8, 4, 0x9, nullptr, 0, 0x1, out2.data()));
    CHECK(pxEq(out2, 8, 0, 0, 0, 0, 0, 0));
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
