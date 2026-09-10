#pragma once
// =============================================================================
// PC_PORT PATCH of the vendored libs/nw4r/include/nw4r/ut/Color.h (M9.5.4).
// This shadow copy wins over the vendored header through the compat include
// path (see src/compat/include/README.md).
//
// Change vs. upstream: the u32 view of the colour is BIG-ENDIAN CANONICAL
// (0xRRGGBBAA) on every host, instead of a raw reinterpret_cast of the four
// GXColor bytes.
//
// Why: nw4r treats ut::Color both as a GXColor {r,g,b,a} byte struct AND as
// one u32 (`operator u32`, `operator=(u32)`, the WHITE/0xFFFFFFFF compares).
// On the console both views agree because PowerPC is big-endian: the word
// read over the bytes r,g,b,a is 0xRRGGBBAA. On a little-endian host the raw
// word over the same bytes is 0xAABBGGRR, and every place that hands the u32
// view to GX gets the channels reversed:
//   * lyt_common.cpp DrawQuad -> GXColor1u32(vtxColors[i]) (picture / window
//     vertex colours) — the FIFO writer unpacks r = MSB, like the hardware.
//     A {255,255,255,a<255} corner colour arrived as r=a,g=255,b=255,a=255:
//     the opaque yellow/cyan boxes behind the TM/(R)/subtitle panes of the
//     title logo (v6 report).
//   * ut_CharWriter.cpp -> GXColor1u32(mVertexColor.*) (text vertex colours).
//   * GXSetChanMatColor / GXSetTevColor(reg, min/max) take a GXColor, so the
//     byte view stays correct — only the u32 view was broken.
// Making the u32 view canonical fixes every user at once; the byte members
// (r,g,b,a) keep their meaning, so the resource copies (res::Picture::vtxCols
// stored as r,g,b,a bytes on disk — deliberately NOT byte-swapped by
// compat/nw4r/LytHost.cpp) and the per-component animation writes are
// untouched.
//
// Everything else is identical to upstream.
// =============================================================================

#include <revolution/gx/GXStruct.h>

namespace nw4r {
    namespace ut {
        struct Color : public GXColor {
        public:
            static const int ALPHA_MAX = 255;

            static const u32 WHITE = 0xFFFFFFFF;

            Color() { *this = 0xFFFFFFFF; }

            Color(u32 color) { *this = color; }

            Color(const GXColor& color) { *this = color; }

            Color& operator=(u32 color) {
                // PC_PORT: canonical 0xRRGGBBAA -> byte members (host-endian safe).
                r = static_cast< u8 >(color >> 24);
                g = static_cast< u8 >(color >> 16);
                b = static_cast< u8 >(color >> 8);
                a = static_cast< u8 >(color);
                return *this;
            }

            Color& operator=(const GXColor& color) {
                // PC_PORT: member-wise copy (upstream reinterpreted the bytes
                // as a u32, which is the same thing on a big-endian console).
                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
                return *this;
            }

            ~Color() {}

            // PC_PORT: canonical 0xRRGGBBAA regardless of host endianness.
            operator u32() const { return ToU32(); }

            u32 ToU32() const {
                return (static_cast< u32 >(r) << 24) | (static_cast< u32 >(g) << 16) | (static_cast< u32 >(b) << 8) |
                       static_cast< u32 >(a);
            }

            // PC_PORT: the upstream `u32& ToU32ref()` reference accessors cannot
            // be endian-corrected; nothing in the port uses them (grep before
            // reintroducing). Removed so that any new user fails to compile
            // instead of silently reading a reversed word.
        };
    };  // namespace ut
};  // namespace nw4r
