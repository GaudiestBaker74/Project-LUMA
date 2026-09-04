// =============================================================================
// M9.5.2: nw4r layout stack tests (headless, no GPU, no game assets).
//
// Exercises the pieces the title screen needs, against synthetic big-endian
// blobs built in memory (same pattern as jkr_archive_test.cpp):
//
//   - Platform::CompatLyt::convertBrlyt (compat/nw4r/LytHost): in-place
//     BE -> host conversion of a synthetic brlyt covering every section type
//     the swapper knows (lyt1/txl1/fnl1/mat1/pan1/pas1/pic1/txt1/wnd1/bnd1/
//     pae1/grp1/grs1/gre1), idempotency, and rejection of garbage.
//   - nw4r::lyt::Layout::Build over a converted blob: minimal (panes only)
//     and full-chain (picture + material + TPL texture fetch + textbox with
//     a brfnt font fetch through a stub ResourceAccessor). This also covers
//     the reconstructed Layout::BuildPaneObj (compat/nw4r/LytMissing.cpp)
//     and the host MEMAllocator wiring.
//   - Platform::CompatGx::tplToHost (compat/gx/TplHost): synthetic TPL
//     conversion + cache identity + garbage rejection.
//   - nw4r::ut::ResFont::SetResource (patched ut_ResFont.cpp): synthetic BE
//     brfnt conversion, metrics/width lookups, garbage rejection.
//
// The synthetic layouts mirror the real disk formats; offsets follow the
// section map in compat/nw4r/LytHost.cpp and libs/nw4r/include/nw4r/lyt/
// resources.h. All multi-byte fields are written big-endian, like the Wii.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/gx/GXCompat.h"
#include "compat/gx/TplHost.h"
#include "compat/nw4r/LytHost.h"
#include "platform/Renderer/Renderer.h"

#include <SDL3/SDL.h>

#include <nw4r/lyt/drawInfo.h>
#include <nw4r/lyt/layout.h>
#include <nw4r/lyt/pane.h>
#include <nw4r/lyt/resourceAccessor.h>
#include <nw4r/lyt/textBox.h>
#include <nw4r/ut/Font.h>
#include <nw4r/ut/ResFont.h>

#include <revolution/mem/allocator.h>
#include <revolution/mtx.h>

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>

#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Big-endian blob writers + little-endian readers (post-conversion checks).
// ---------------------------------------------------------------------------

void put8(std::vector<u8>& v, u8 x) {
    v.push_back(x);
}

void put16(std::vector<u8>& v, u16 x) {
    v.push_back(static_cast<u8>(x >> 8));
    v.push_back(static_cast<u8>(x));
}

void put32(std::vector<u8>& v, u32 x) {
    v.push_back(static_cast<u8>(x >> 24));
    v.push_back(static_cast<u8>(x >> 16));
    v.push_back(static_cast<u8>(x >> 8));
    v.push_back(static_cast<u8>(x));
}

void putF32(std::vector<u8>& v, f32 f) {
    u32 bits;
    std::memcpy(&bits, &f, sizeof(bits));
    put32(v, bits);
}

void putBytes(std::vector<u8>& v, const void* p, size_t n) {
    const u8* b = static_cast<const u8*>(p);
    v.insert(v.end(), b, b + n);
}

void putFixedStr(std::vector<u8>& v, const char* s, size_t fixedLen) {
    const size_t n = std::strlen(s);
    putBytes(v, s, n < fixedLen ? n : fixedLen);
    for (size_t i = n; i < fixedLen; ++i) {
        put8(v, 0);
    }
}

void putNulStr(std::vector<u8>& v, const char* s) {
    putBytes(v, s, std::strlen(s) + 1);
}

void padTo4(std::vector<u8>& v) {
    while (v.size() % 4 != 0) {
        put8(v, 0);
    }
}

void patch16(std::vector<u8>& v, size_t off, u16 x) {
    v[off + 0] = static_cast<u8>(x >> 8);
    v[off + 1] = static_cast<u8>(x);
}

void patch32(std::vector<u8>& v, size_t off, u32 x) {
    v[off + 0] = static_cast<u8>(x >> 24);
    v[off + 1] = static_cast<u8>(x >> 16);
    v[off + 2] = static_cast<u8>(x >> 8);
    v[off + 3] = static_cast<u8>(x);
}

u16 get16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

u32 get32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

f32 getF32(const u8* p) {
    u32 bits = get32(p);
    f32 v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

// Assembles a brlyt: header + blocks. Blocks are appended with makeBlock()
// and their file offsets recorded, so checks can address sections directly.
class BrlytBuilder {
public:
    // Begins a block: writes kind + a size placeholder, returns the block's
    // file offset. Call endBlock() when the body is complete.
    u32 beginBlock(const char* kind) {
        const u32 off = static_cast<u32>(16 + mBlocks.size());
        putBytes(mBlocks, kind, 4);
        put32(mBlocks, 0);  // size, patched in endBlock
        return off;
    }

    void endBlock(u32 blockOff) {
        const u32 size = static_cast<u32>(16 + mBlocks.size()) - blockOff;
        patch32(mBlocks, blockOff + 4 - 16, size);
        ++mBlockCount;
    }

    // Body writes go through body(), a view onto the block payload.
    std::vector<u8>& raw() { return mBlocks; }

    std::vector<u8> finish() {
        std::vector<u8> file;
        putBytes(file, "RLYT", 4);
        put16(file, 0xFEFF);  // BOM value; FE FF bytes on disk
        put16(file, 0x000A);  // version 10
        put32(file, static_cast<u32>(16 + mBlocks.size()));
        put16(file, 16);  // headerSize
        put16(file, static_cast<u16>(mBlockCount));
        putBytes(file, mBlocks.data(), mBlocks.size());
        return file;
    }

private:
    std::vector<u8> mBlocks;
    int mBlockCount = 0;
};

// --- pane body helpers (res::Pane: 76 bytes on disk) -------------------------

void putPaneBody(std::vector<u8>& v, const char* name, f32 tx, f32 ty, f32 tz, f32 sx, f32 sy,
                 f32 w, f32 h) {
    put8(v, 1);    // flag: visible
    put8(v, 0);    // basePosition
    put8(v, 0xFF); // alpha
    put8(v, 0);    // padding
    putFixedStr(v, name, 16);
    putFixedStr(v, "", 8);  // userData
    putF32(v, tx);
    putF32(v, ty);
    putF32(v, tz);
    putF32(v, 0.0f);  // rotate x
    putF32(v, 0.0f);
    putF32(v, 0.0f);
    putF32(v, sx);
    putF32(v, sy);
    putF32(v, w);
    putF32(v, h);
}

// ---------------------------------------------------------------------------
// The full synthetic layout. Section sizes/offsets per the LytHost.cpp map:
//   lyt1(20) txl1(52) fnl1(32) mat1(124) pan1(76) pas1(8) pic1(128)
//   txt1(124) wnd1(164) bnd1(76) pae1(8) grp1(28) grs1(8) grp1(44) gre1(8)
// ---------------------------------------------------------------------------

struct RichLayout {
    std::vector<u8> file;
    // Recorded block offsets.
    u32 lyt1, txl1, fnl1, mat1, pan1, pic1, txt1, wnd1, bnd1, grp1Root, grp1Named;
    u32 material;  // file offset of the material entry inside mat1
};

RichLayout makeRichBrlyt() {
    RichLayout out;
    BrlytBuilder b;

    out.lyt1 = b.beginBlock("lyt1");
    put8(b.raw(), 0);           // originType
    put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    putF32(b.raw(), 640.0f);    // layoutSize
    putF32(b.raw(), 480.0f);
    b.endBlock(out.lyt1);

    out.txl1 = b.beginBlock("txl1");
    // res::Texture entries (resources.h): 8 bytes each — u32 nameStrOffset
    // (RELATIVE to the entry array at block+12: lyt_material.cpp resolves
    // `ConvertOffsToPtr<char>(textures, nameStrOffset)`), u8 type, 3 pad.
    // Strings sit after the entries at block+28/+40 -> entry-relative 16/28.
    // (Before M9.5.3c this test wrote 4-byte entries with block-relative
    // offsets — wrong vs. the real format; it only passed because the stub
    // accessor serves ANY name.)
    put16(b.raw(), 2); put16(b.raw(), 0);
    put32(b.raw(), 16); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    put32(b.raw(), 28); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    putNulStr(b.raw(), "tex0.tpl"); padTo4(b.raw());  // @28
    putNulStr(b.raw(), "tex1.tpl"); padTo4(b.raw());  // @40
    b.endBlock(out.txl1);

    out.fnl1 = b.beginBlock("fnl1");
    // res::Font entry (8 B), same entry-array-relative rule as txl1.
    put16(b.raw(), 1); put16(b.raw(), 0);
    put32(b.raw(), 8); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    putNulStr(b.raw(), "font1.brfnt"); padTo4(b.raw());  // @20 (entries+8)
    b.endBlock(out.fnl1);

    // mat1: one material, resNum = texMap 1 | texSRT 1<<4 | texCoordGen 1<<8
    //       | tevStage 1<<18 = 0x00040111. Tail order per Material ctor:
    //       TexMap(4) TexSRT(20) TexCoordGen(4) TevStage(16).
    out.mat1 = b.beginBlock("mat1");
    put16(b.raw(), 1); put16(b.raw(), 0);
    put32(b.raw(), 16);  // material offset, block-relative
    out.material = out.mat1 + 16;
    putFixedStr(b.raw(), "mat_test", 20);
    for (int i = 0; i < 3; ++i) {  // tevCols: 3 x GXColorS10 (4 x s16)
        for (int c = 0; c < 4; ++c) {
            put16(b.raw(), static_cast<u16>(static_cast<s16>(200 + c)));
        }
    }
    for (int i = 0; i < 16; ++i) {  // tevKCols: bytes
        put8(b.raw(), static_cast<u8>(i));
    }
    put32(b.raw(), 0x00040111);  // resNum bits
    put16(b.raw(), 1);           // TexMap.texIdx -> tex1.tpl
    put8(b.raw(), 0); put8(b.raw(), 0);
    putF32(b.raw(), 3.0f); putF32(b.raw(), 4.0f);   // TexSRT.translate
    putF32(b.raw(), 5.0f);                          // TexSRT.rotate
    putF32(b.raw(), 1.5f); putF32(b.raw(), 2.5f);   // TexSRT.scale
    // TexCoordGen {texGenType, texGenSrc, texMtx, reserve} — the library
    // default (GX_TG_MTX2x4=1, GX_TG_TEX0=4, GX_IDENTITY=60). IDENTITY keeps
    // Material::SetupGX off the tex-mtx path (a garbage texMtx would index
    // useTexMtx[(mtx-GX_TEXMTX0)/3] out of bounds — real brlyts are always
    // valid here, the draw test needs to be too).
    put8(b.raw(), 1); put8(b.raw(), 4); put8(b.raw(), 60); put8(b.raw(), 0);
    // TevStage (16 B), layout per nw4r/lyt/types.h: texCoordGen, colChan,
    // texMap, swapSel, colIn{ab,cd,op,cl}, alpIn{ab,cd,op,cl}, ind{4}.
    // REPLACE preset: out.rgb = TEXC, out.a = TEXA — the red TPL lands as-is.
    const u8 tevStage[16] = {
        0,     // texCoordGen = GX_TEXCOORD0
        4,     // colChan = GX_COLOR0A0 -> kC2R -> col0
        0,     // texMap = GX_TEXMAP0 (no disable bit)
        0,     // swapSel: rasSel=0, texSel=0
        0xFF,  // colIn a,b = GX_CC_ZERO, GX_CC_ZERO
        0x8F,  // colIn c,d = GX_CC_ZERO, GX_CC_TEXC(8)
        0x00,  // colOp: ADD, bias ZERO, scale 1
        0x61,  // clamp=1, outReg=GX_TEVPREV, kSel=GX_TEV_KCSEL_K0(0x0C)<<3
        0x77,  // alpIn a,b = GX_CA_ZERO, GX_CA_ZERO
        0x47,  // alpIn c,d = GX_CA_ZERO, GX_CA_TEXA(4)
        0x00,  // alpOp: ADD, bias ZERO, scale 1
        0x81,  // clamp=1, outReg=GX_TEVPREV, kSel=GX_TEV_KASEL_K0_R(0x10)<<3
        0, 0, 0, 0,  // indirect off (ITF_8/ITB_NONE/ITM_OFF/ITW_OFF/ITBA_OFF)
    };
    for (int i = 0; i < 16; ++i) {  // TevStage
        put8(b.raw(), tevStage[i]);
    }
    b.endBlock(out.mat1);

    out.pan1 = b.beginBlock("pan1");
    putPaneBody(b.raw(), "root_pane", 0, 0, 0, 1, 1, 640, 480);
    b.endBlock(out.pan1);

    const u32 pas1 = b.beginBlock("pas1");
    b.endBlock(pas1);

    out.pic1 = b.beginBlock("pic1");
    putPaneBody(b.raw(), "picture", 10, 20, 0, 1, 1, 100, 50);
    for (int i = 0; i < 4; ++i) {
        put32(b.raw(), 0xFFFFFFFF);  // vtxCols
    }
    put16(b.raw(), 0);  // materialIdx
    put8(b.raw(), 1);   // texCoordNum
    put8(b.raw(), 0);
    const f32 tc[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    for (int i = 0; i < 8; ++i) {
        putF32(b.raw(), tc[i]);
    }
    b.endBlock(out.pic1);

    // txt1: "Hi!" as UTF-16BE at block offset 116; textStrBytes = 8.
    out.txt1 = b.beginBlock("txt1");
    putPaneBody(b.raw(), "textbox", 0, 0, 0, 1, 1, 200, 30);
    put16(b.raw(), 64);  // textBufBytes
    put16(b.raw(), 8);   // textStrBytes
    put16(b.raw(), 0);   // materialIdx
    put16(b.raw(), 0);   // fontIdx
    put8(b.raw(), 0);    // textPosition
    put8(b.raw(), 0);    // textAlignment
    put8(b.raw(), 0); put8(b.raw(), 0);
    put32(b.raw(), 116);  // textStrOffset (block-relative)
    put32(b.raw(), 0xFFFFFFFF);  // textCols[0]
    put32(b.raw(), 0x80808080);  // textCols[1]
    putF32(b.raw(), 16.0f);      // fontSize.w
    putF32(b.raw(), 16.0f);      // fontSize.h
    putF32(b.raw(), 0.5f);       // charSpace
    putF32(b.raw(), 2.0f);       // lineSpace
    const u8 hi[] = {0x00, 'H', 0x00, 'i', 0x00, '!', 0x00, 0x00};  // UTF-16BE
    putBytes(b.raw(), hi, sizeof(hi));
    b.endBlock(out.txt1);

    // wnd1: 1 frame @108 (table @104), content @112 (WindowContent 20B +
    // 1 texcoord set 32B) -> block size 164.
    out.wnd1 = b.beginBlock("wnd1");
    putPaneBody(b.raw(), "window", 0, 0, 0, 1, 1, 300, 200);
    putF32(b.raw(), 10.0f); putF32(b.raw(), 10.0f);  // inflation L R
    putF32(b.raw(), 10.0f); putF32(b.raw(), 10.0f);  // inflation T B
    put8(b.raw(), 1);  // frameNum
    put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    put32(b.raw(), 112);  // contentOffset (block-relative)
    put32(b.raw(), 104);  // frameOffsetTableOffset
    put32(b.raw(), 108);  // frame[0] offset
    put16(b.raw(), 0);    // WindowFrame.materialIdx
    put8(b.raw(), 0); put8(b.raw(), 0);
    for (int i = 0; i < 4; ++i) {
        put32(b.raw(), 0xFFFFFFFF);  // WindowContent.vtxCols
    }
    put16(b.raw(), 0);  // WindowContent.materialIdx
    put8(b.raw(), 1);   // texCoordNum
    put8(b.raw(), 0);
    for (int i = 0; i < 8; ++i) {
        putF32(b.raw(), tc[i]);
    }
    b.endBlock(out.wnd1);

    out.bnd1 = b.beginBlock("bnd1");
    putPaneBody(b.raw(), "bounding", 0, 0, 0, 1, 1, 640, 480);
    b.endBlock(out.bnd1);

    const u32 pae1 = b.beginBlock("pae1");
    b.endBlock(pae1);

    out.grp1Root = b.beginBlock("grp1");
    putFixedStr(b.raw(), "root_group", 16);
    put16(b.raw(), 0); put16(b.raw(), 0);  // paneNum
    b.endBlock(out.grp1Root);

    const u32 grs1 = b.beginBlock("grs1");
    b.endBlock(grs1);

    out.grp1Named = b.beginBlock("grp1");
    putFixedStr(b.raw(), "grp_a", 16);
    put16(b.raw(), 1); put16(b.raw(), 0);  // paneNum
    putFixedStr(b.raw(), "picture", 16);
    b.endBlock(out.grp1Named);

    const u32 gre1 = b.beginBlock("gre1");
    b.endBlock(gre1);

    out.file = b.finish();
    return out;
}

// Minimal layout: root pane -> one child, plus the root group wrapper and one
// named group. Blocks: lyt1 pan1 pas1 pan1 pae1 grp1 grs1 grp1 gre1.
std::vector<u8> makeMinimalBrlyt() {
    BrlytBuilder b;

    const u32 lyt1 = b.beginBlock("lyt1");
    put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    putF32(b.raw(), 640.0f);
    putF32(b.raw(), 480.0f);
    b.endBlock(lyt1);

    const u32 root = b.beginBlock("pan1");
    putPaneBody(b.raw(), "root_pane", 0, 0, 0, 1, 1, 640, 480);
    b.endBlock(root);

    const u32 pas1 = b.beginBlock("pas1");
    b.endBlock(pas1);

    const u32 child = b.beginBlock("pan1");
    putPaneBody(b.raw(), "child_pane", 10, 20, 0, 2, 2, 100, 50);
    b.endBlock(child);

    const u32 pae1 = b.beginBlock("pae1");
    b.endBlock(pae1);

    const u32 grpRoot = b.beginBlock("grp1");
    putFixedStr(b.raw(), "root_group", 16);
    put16(b.raw(), 0); put16(b.raw(), 0);
    b.endBlock(grpRoot);

    const u32 grs1 = b.beginBlock("grs1");
    b.endBlock(grs1);

    const u32 grpA = b.beginBlock("grp1");
    putFixedStr(b.raw(), "grp_a", 16);
    put16(b.raw(), 1); put16(b.raw(), 0);
    putFixedStr(b.raw(), "child_pane", 16);
    b.endBlock(grpA);

    const u32 gre1 = b.beginBlock("gre1");
    b.endBlock(gre1);

    return b.finish();
}

// Synthetic BE TPL: one 4x4 RGB565 texture (solid red), no CLUT. 4x4 is the
// smallest GX-legal size (the tiled decoder rejects 2x2).
//   header(12) + descriptor(8) + texture header(36) + texels(32)
std::vector<u8> makeTpl() {
    std::vector<u8> v;
    put32(v, 0x0020AF30);  // version (BE magic)
    put32(v, 1);           // numDescriptors
    put32(v, 12);          // descriptor array offset
    put32(v, 20);          // texture header offset
    put32(v, 0);           // no CLUT
    put16(v, 4);           // height
    put16(v, 4);           // width
    put32(v, 4);           // format: GX_TF_RGB565 (was 3=IA8 by mistake —
                           // the texels below are RGB565 and the decoder
                           // rejected IA8-in-TPL as unsupported)
    put32(v, 56);          // data offset
    put32(v, 0);           // wrapS: clamp
    put32(v, 0);           // wrapT: clamp
    put32(v, 1);           // minFilter: linear
    put32(v, 1);           // magFilter: linear
    putF32(v, 0.0f);       // LODBias
    put8(v, 0);            // edgeLODEnable
    put8(v, 0);            // minLOD
    put8(v, 0);            // maxLOD
    put8(v, 0);
    u8 texels[32];
    for (int i = 0; i < 16; i++) {
        texels[i * 2 + 0] = 0xF8;  // RGB565 red
        texels[i * 2 + 1] = 0x00;
    }
    putBytes(v, texels, sizeof(texels));
    return v;
}

// Synthetic BE brfnt (RFNT): FINF + TGLF + CWDH + CMAP + one 64-byte sheet.
//   header(16) FINF@16(32) TGLF@48(32) CWDH@80(32) CMAP@112(28) sheet@140
std::vector<u8> makeBrfnt() {
    std::vector<u8> v;
    putBytes(v, "RFNT", 4);
    put16(v, 0xFEFF);   // BOM value; FE FF bytes on disk
    put16(v, 0x0102);   // version
    put32(v, 204);      // fileSize
    put16(v, 16);       // headerSize
    put16(v, 4);        // dataBlocks

    // FINF: data @24
    putBytes(v, "FINF", 4);
    put32(v, 32);
    put8(v, 1);   // fontType
    put8(v, 18);  // linefeed
    put16(v, 0);  // alterCharIndex
    put8(v, 0);   // defaultWidth.left
    put8(v, 7);   // defaultWidth.glyphWidth
    put8(v, 7);   // defaultWidth.charWidth
    put8(v, 0);   // encoding: UTF-8
    put32(v, 56);   // glyph block data offset (TGLF)
    put32(v, 88);   // width data offset (CWDH)
    put32(v, 120);  // map data offset (CMAP)
    put8(v, 16);  // height
    put8(v, 8);   // width
    put8(v, 14);  // ascent
    put8(v, 0);

    // TGLF: data @56, block size 32 (8 header + 24 body)
    putBytes(v, "TGLF", 4);
    put32(v, 32);
    put8(v, 8);   // cellWidth
    put8(v, 16);  // cellHeight
    put8(v, 14);  // baselinePos
    put8(v, 8);   // maxCharWidth
    put32(v, 64); // sheetSize
    put16(v, 1);  // sheetNum
    put16(v, 0);  // sheetFormat
    put16(v, 1);  // sheetRow
    put16(v, 1);  // sheetLine
    put16(v, 8);  // sheetWidth
    put16(v, 16); // sheetHeight
    put32(v, 140);  // sheet image offset

    // CWDH: data @88; glyphs 0..2, 3-byte CharWidths entries.
    putBytes(v, "CWDH", 4);
    put32(v, 32);
    put16(v, 0);  // indexBegin
    put16(v, 2);  // indexEnd
    put32(v, 0);  // next
    const u8 widths[9] = {
        0, 7, 7,  // glyph 0
        0, 5, 5,  // glyph 1
        1, 9, 9,  // glyph 2 (negative left as u8 wrap: use +1)
    };
    putBytes(v, widths, sizeof(widths));
    for (int i = 0; i < 7; ++i) {
        put8(v, 0);
    }

    // CMAP: data @120; codes 0x20..0x22 direct-mapped to glyphs 0..2.
    putBytes(v, "CMAP", 4);
    put32(v, 28);
    put16(v, 0x20);  // ccodeBegin
    put16(v, 0x22);  // ccodeEnd
    put16(v, 0);     // mappingMethod: direct
    put16(v, 0);     // reserved
    put32(v, 0);     // next
    put16(v, 0);
    put16(v, 1);
    put16(v, 2);
    put16(v, 0);  // pad to 4

    // Sheet payload @140 (64 bytes, contents irrelevant for metrics).
    for (int i = 0; i < 64; ++i) {
        put8(v, static_cast<u8>(i));
    }
    return v;
}

// Stub accessor: serves the synthetic TPL for 'timg' and the synthetic brfnt
// for 'font' (the two resource types Layout::Build/Material/TextBox fetch).
// Records every requested name so tests can pin the txl1/fnl1 offset
// semantics (what the engine asks for must be the name the block declares).
class StubResourceAccessor : public nw4r::lyt::ResourceAccessor {
public:
    StubResourceAccessor() {
        mTpl = makeTpl();
        mFont = makeBrfnt();
    }

    void* GetResource(nw4r::lyt::ResType type, const char* name, u32* pSize) override {
        requested.emplace_back(name != nullptr ? name : "(null)");
        if (type == 'timg') {
            if (pSize != nullptr) {
                *pSize = static_cast<u32>(mTpl.size());
            }
            return mTpl.data();
        }
        if (type == 'font') {
            if (pSize != nullptr) {
                *pSize = static_cast<u32>(mFont.size());
            }
            return mFont.data();
        }
        return nullptr;
    }

    nw4r::ut::Font* GetFont(const char* name) override {
        requestedFonts.emplace_back(name != nullptr ? name : "(null)");
        return nullptr;
    }

    std::vector<std::string> requested;    // GetResource names, in order
    std::vector<std::string> requestedFonts;  // GetFont names, in order

private:
    std::vector<u8> mTpl;
    std::vector<u8> mFont;
};

// malloc-backed MEMAllocator for Layout::NewObj (the game wires
// MR::NewDeleteAllocator in GameBoot; tests stay dependency-free).
void* testAlloc(MEMAllocator*, u32 size) {
    return std::malloc(size);
}

void testFree(MEMAllocator*, void* p) {
    std::free(p);
}

MEMAllocatorFunc sTestFuncs = {testAlloc, testFree};
MEMAllocator sTestAllocator = {&sTestFuncs, nullptr, 0, 0};

}  // namespace

// ---------------------------------------------------------------------------
// brlyt swapper
// ---------------------------------------------------------------------------

TEST_CASE(lyt_swapper_converts_all_sections) {
    RichLayout lyt = makeRichBrlyt();
    REQUIRE(Platform::CompatLyt::isBrlyt(lyt.file.data(), static_cast<u32>(lyt.file.size())));

    // Pre-conversion sanity: the file is big-endian (FE FF on disk).
    REQUIRE(lyt.file[4] == 0xFE && lyt.file[5] == 0xFF);

    REQUIRE(Platform::CompatLyt::convertBrlyt(lyt.file.data(),
                                              static_cast<u32>(lyt.file.size())));

    const u8* p = lyt.file.data();

    // Header now reads host-native.
    CHECK_EQ(get16(p + 4), static_cast<u16>(0xFEFF));
    CHECK_EQ(get16(p + 6), static_cast<u16>(0x000A));
    CHECK_EQ(get32(p + 8), static_cast<u32>(lyt.file.size()));
    CHECK_EQ(get16(p + 12), static_cast<u16>(16));
    CHECK_EQ(get16(p + 14), static_cast<u16>(15));

    // lyt1: layoutSize.
    CHECK_NEAR(getF32(p + lyt.lyt1 + 12), 640.0f, 1e-6);
    CHECK_NEAR(getF32(p + lyt.lyt1 + 16), 480.0f, 1e-6);

    // txl1: count + name offsets; names untouched. Offsets are relative to
    // the ENTRY ARRAY at block+12 (the lyt_material.cpp base), so the
    // strings at block+28/+40 are stored as 16/28.
    CHECK_EQ(get16(p + lyt.txl1 + 8), static_cast<u16>(2));
    CHECK_EQ(get32(p + lyt.txl1 + 12), static_cast<u32>(16));
    CHECK_EQ(get32(p + lyt.txl1 + 20), static_cast<u32>(28));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p + lyt.txl1 + 28), "tex0.tpl") == 0);

    // fnl1 (same entry-array-relative rule: string @20 -> stored as 8)
    CHECK_EQ(get16(p + lyt.fnl1 + 8), static_cast<u16>(1));
    CHECK_EQ(get32(p + lyt.fnl1 + 12), static_cast<u32>(8));

    // mat1: count, block-relative material offset, resNum, tail fields.
    CHECK_EQ(get16(p + lyt.mat1 + 8), static_cast<u16>(1));
    CHECK_EQ(get32(p + lyt.mat1 + 12), static_cast<u32>(16));
    const u8* m = p + lyt.material;
    CHECK(std::memcmp(m, "mat_test", 8) == 0);
    // tevCols are swapped as 32-bit words, so each pair of s16s lands
    // word-reversed in memory: [20]=s16[1], [22]=s16[0].
    CHECK_EQ(static_cast<int>(get16(m + 20)), 201);
    CHECK_EQ(static_cast<int>(get16(m + 22)), 200);
    CHECK_EQ(get32(m + 60), static_cast<u32>(0x00040111));
    CHECK_EQ(get16(m + 64), static_cast<u16>(1));  // TexMap.texIdx
    CHECK_NEAR(getF32(m + 68), 3.0f, 1e-6);        // TexSRT.translate.x
    CHECK_NEAR(getF32(m + 84), 2.5f, 1e-6);        // TexSRT.scale.y
    CHECK_EQ(static_cast<int>(m[88]), 1);          // TexCoordGen bytes untouched
    CHECK_EQ(static_cast<int>(m[89]), 4);          // (the builder writes the library
    CHECK_EQ(static_cast<int>(m[90]), 60);         // default: GX_TG_MTX2x4, GX_TG_TEX0,
    CHECK_EQ(static_cast<int>(m[91]), 0);          // GX_IDENTITY — see makeRichBrlyt)
    CHECK_EQ(static_cast<int>(m[92]), 0);          // TevStage.texCoordGen untouched
    CHECK_EQ(static_cast<int>(m[93]), 4);          // TevStage.colChan = GX_COLOR0A0
    CHECK_EQ(static_cast<int>(m[97]), 0x8F);       // TevStage colIn c,d = ZERO,TEXC

    // pan1: block size + transform f32s; name untouched.
    CHECK_EQ(get32(p + lyt.pan1 + 4), static_cast<u32>(76));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p + lyt.pan1 + 12), "root_pane") == 0);
    CHECK_NEAR(getF32(p + lyt.pan1 + 68), 640.0f, 1e-6);  // size.w

    // pic1: materialIdx + texcoords.
    CHECK_EQ(get16(p + lyt.pic1 + 92), static_cast<u16>(0));
    CHECK_NEAR(getF32(p + lyt.pic1 + 96 + 8), 1.0f, 1e-6);  // tc[2] (u of v1)

    // txt1: u16 fields, string offset, metrics; string stays UTF-16BE.
    CHECK_EQ(get16(p + lyt.txt1 + 76), static_cast<u16>(64));
    CHECK_EQ(get16(p + lyt.txt1 + 78), static_cast<u16>(8));
    CHECK_EQ(get16(p + lyt.txt1 + 82), static_cast<u16>(0));
    CHECK_EQ(get32(p + lyt.txt1 + 88), static_cast<u32>(116));
    CHECK_NEAR(getF32(p + lyt.txt1 + 100), 16.0f, 1e-6);
    CHECK_NEAR(getF32(p + lyt.txt1 + 112), 2.0f, 1e-6);
    CHECK(p[lyt.txt1 + 116] == 0x00 && p[lyt.txt1 + 117] == 'H');

    // wnd1: inflation, frame table + frame materialIdx, content.
    CHECK_NEAR(getF32(p + lyt.wnd1 + 76), 10.0f, 1e-6);
    CHECK_EQ(get32(p + lyt.wnd1 + 96), static_cast<u32>(112));
    CHECK_EQ(get32(p + lyt.wnd1 + 100), static_cast<u32>(104));
    CHECK_EQ(get32(p + lyt.wnd1 + 104), static_cast<u32>(108));
    CHECK_EQ(get16(p + lyt.wnd1 + 108), static_cast<u16>(0));
    CHECK_EQ(get16(p + lyt.wnd1 + 112 + 16), static_cast<u16>(0));  // content materialIdx

    // bnd1 + groups.
    CHECK_EQ(get32(p + lyt.bnd1 + 4), static_cast<u32>(76));
    CHECK_EQ(get16(p + lyt.grp1Root + 24), static_cast<u16>(0));
    CHECK_EQ(get16(p + lyt.grp1Named + 24), static_cast<u16>(1));
    CHECK(std::strcmp(reinterpret_cast<const char*>(p + lyt.grp1Named + 28), "picture") == 0);

    // Idempotency: a second pass must not touch a single byte.
    const std::vector<u8> snapshot = lyt.file;
    REQUIRE(Platform::CompatLyt::convertBrlyt(lyt.file.data(),
                                              static_cast<u32>(lyt.file.size())));
    CHECK(lyt.file == snapshot);
}

TEST_CASE(lyt_swapper_rejects_bad_input) {
    // Not a brlyt at all.
    u8 garbage[64];
    for (int i = 0; i < 64; ++i) {
        garbage[i] = static_cast<u8>(i * 7 + 1);
    }
    CHECK(!Platform::CompatLyt::isBrlyt(garbage, sizeof(garbage)));
    CHECK(!Platform::CompatLyt::convertBrlyt(garbage, sizeof(garbage)));

    // Buffer too small for a header.
    u8 tiny[8] = {'R', 'L', 'Y', 'T', 0xFF, 0xFE, 0, 0};
    CHECK(!Platform::CompatLyt::isBrlyt(tiny, sizeof(tiny)));
    CHECK(!Platform::CompatLyt::convertBrlyt(tiny, sizeof(tiny)));

    // Bad byte-order mark.
    std::vector<u8> badOrder;
    putBytes(badOrder, "RLYT", 4);
    put16(badOrder, 0x1234);
    put16(badOrder, 0x000A);
    put32(badOrder, 16);
    put16(badOrder, 16);
    put16(badOrder, 0);
    CHECK(Platform::CompatLyt::isBrlyt(badOrder.data(), static_cast<u32>(badOrder.size())));
    CHECK(!Platform::CompatLyt::convertBrlyt(badOrder.data(),
                                             static_cast<u32>(badOrder.size())));

    // Block whose size runs past EOF must abort the conversion.
    std::vector<u8> truncated;
    putBytes(truncated, "RLYT", 4);
    put16(truncated, 0xFEFF);
    put16(truncated, 0x000A);
    put32(truncated, 28);  // fileSize
    put16(truncated, 16);
    put16(truncated, 1);  // one block
    putBytes(truncated, "lyt1", 4);
    put32(truncated, 9999);  // bogus size
    put32(truncated, 0);
    put32(truncated, 0);
    CHECK(!Platform::CompatLyt::convertBrlyt(truncated.data(),
                                             static_cast<u32>(truncated.size())));
}

// ---------------------------------------------------------------------------
// Layout::Build
// ---------------------------------------------------------------------------

TEST_CASE(lyt_build_minimal_layout) {
    std::vector<u8> file = makeMinimalBrlyt();
    REQUIRE(Platform::CompatLyt::convertBrlyt(file.data(), static_cast<u32>(file.size())));

    nw4r::lyt::Layout::mspAllocator = &sTestAllocator;
    StubResourceAccessor accessor;

    nw4r::lyt::Layout layout;
    REQUIRE(layout.Build(file.data(), &accessor));

    nw4r::lyt::Pane* root = layout.mpRootPane;
    REQUIRE(root != nullptr);
    CHECK(std::strcmp(root->mName, "root_pane") == 0);
    CHECK_NEAR(layout.mLayoutSize.width, 640.0f, 1e-6);
    CHECK_NEAR(layout.mLayoutSize.height, 480.0f, 1e-6);
    CHECK_NEAR(root->mSize.width, 640.0f, 1e-6);
    CHECK_NEAR(root->mSize.height, 480.0f, 1e-6);
    CHECK(root->IsVisible());

    // One child, with its transform converted.
    int childCount = 0;
    nw4r::lyt::Pane* child = nullptr;
    for (auto it = root->mChildList.GetBeginIter(); it != root->mChildList.GetEndIter(); ++it) {
        ++childCount;
        child = &*it;
    }
    REQUIRE(childCount == 1);
    CHECK(std::strcmp(child->mName, "child_pane") == 0);
    CHECK_NEAR(child->mTranslate.x, 10.0f, 1e-6);
    CHECK_NEAR(child->mTranslate.y, 20.0f, 1e-6);
    CHECK_NEAR(child->mScale.x, 2.0f, 1e-6);
    CHECK(child->GetParent() == root);
    CHECK(root->FindPaneByName("child_pane", true) == child);

    // Groups: root container exists (first grp1), named group inside.
    CHECK(layout.GetGroupContainer() != nullptr);

    // Garbage must fail, not crash.
    u8 garbage[64];
    std::memset(garbage, 0x5A, sizeof(garbage));
    nw4r::lyt::Layout bad;
    CHECK(!bad.Build(garbage, &accessor));
}

TEST_CASE(lyt_build_full_chain_pic_txt_wnd) {
    RichLayout lyt = makeRichBrlyt();
    REQUIRE(Platform::CompatLyt::convertBrlyt(lyt.file.data(),
                                              static_cast<u32>(lyt.file.size())));

    nw4r::lyt::Layout::mspAllocator = &sTestAllocator;
    StubResourceAccessor accessor;  // also owns the TPL/brfnt blobs Build reads

    nw4r::lyt::Layout layout;
    REQUIRE(layout.Build(lyt.file.data(), &accessor));

    nw4r::lyt::Pane* root = layout.mpRootPane;
    REQUIRE(root != nullptr);

    // pan1 root + pas1/pae1 nesting -> 4 children (pic1, txt1, wnd1, bnd1).
    int childCount = 0;
    for (auto it = root->mChildList.GetBeginIter(); it != root->mChildList.GetEndIter(); ++it) {
        ++childCount;
    }
    CHECK_EQ(childCount, 4);

    // Picture: material resolved from mat1, texture fetched via the accessor
    // ('timg' -> synthetic TPL -> TplHost conversion -> TexMap fields).
    nw4r::lyt::Pane* pic = root->FindPaneByName("picture", true);
    REQUIRE(pic != nullptr);
    REQUIRE(pic->mpMaterial != nullptr);
    CHECK(std::strcmp(pic->mpMaterial->GetName(), "mat_test") == 0);
    const nw4r::lyt::TexMap* texMap = pic->mpMaterial->GetTexMapAry();
    REQUIRE(texMap != nullptr);
    CHECK_EQ(static_cast<int>(texMap->mWidth), 4);
    CHECK_EQ(static_cast<int>(texMap->mHeight), 4);
    CHECK_EQ(static_cast<int>(texMap->GetTexelFormat()), 4);  // GX_TF_RGB565
    CHECK(texMap->mImage != nullptr);

    // Offset semantics pinned: the material's texIdx=1 must have asked for
    // the txl1's SECOND declared name ("tex1.tpl"), resolved relative to the
    // entry array (M9.5.3c — the old test blob used the wrong base).
    bool askedTex1 = false, askedFont1 = false;
    for (const std::string& n : accessor.requested) {
        askedTex1 = askedTex1 || n == "tex1.tpl";
    }
    for (const std::string& n : accessor.requestedFonts) {
        askedFont1 = askedFont1 || n == "font1.brfnt";
    }
    CHECK(askedTex1);
    CHECK(askedFont1);

    // TextBox: BE UTF-16 text widened to host wchar_t, font built from the
    // synthetic brfnt through the accessor ('font' -> ResFont::SetResource).
    nw4r::lyt::Pane* txtPane = root->FindPaneByName("textbox", true);
    REQUIRE(txtPane != nullptr);
    // RTTI check: the reconstructed TextBox::typeInfo definition must match.
    REQUIRE(txtPane->GetRuntimeTypeInfo() == &nw4r::lyt::TextBox::typeInfo);
    CHECK(txtPane->GetRuntimeTypeInfo()->IsDerivedFrom(&nw4r::lyt::Pane::typeInfo));
    nw4r::lyt::TextBox* txt = static_cast<nw4r::lyt::TextBox*>(txtPane);
    REQUIRE(txt != nullptr);
    CHECK_EQ(static_cast<int>(txt->mTextLen), 3);
    REQUIRE(txt->mTextBuf != nullptr);
    CHECK(txt->mTextBuf[0] == L'H');
    CHECK(txt->mTextBuf[1] == L'i');
    CHECK(txt->mTextBuf[2] == L'!');
    CHECK_NEAR(txt->mFontSize.width, 16.0f, 1e-6);
    CHECK_NEAR(txt->mLineSpace, 2.0f, 1e-6);
    REQUIRE(txt->mpFont != nullptr);
    CHECK_EQ(txt->mpFont->GetLineFeed(), 18);

    // Window: built with its frame; bounding pane present.
    CHECK(root->FindPaneByName("window", true) != nullptr);
    CHECK(root->FindPaneByName("bounding", true) != nullptr);
}

// ---------------------------------------------------------------------------
// M9.5.3c hardening regression: a brlyt whose material references a texIdx
// outside the txl1 used to produce a wild `char*` (entries + garbage u32) and
// SIGSEGV inside GetResource/ResTable::getRes during Layout::Build. The
// patched Material ctor must degrade to the "not found" path (empty name) and
// the build must complete.
// ---------------------------------------------------------------------------
TEST_CASE(lyt_build_survives_out_of_range_texidx) {
    BrlytBuilder b;
    const u32 lyt1 = b.beginBlock("lyt1");
    put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    putF32(b.raw(), 640.0f);
    putF32(b.raw(), 480.0f);
    b.endBlock(lyt1);

    // txl1: exactly ONE texture, real entry format.
    const u32 txl1 = b.beginBlock("txl1");
    put16(b.raw(), 1); put16(b.raw(), 0);
    put32(b.raw(), 8); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0); put8(b.raw(), 0);
    putNulStr(b.raw(), "tex0.tpl"); padTo4(b.raw());  // @20 (entries+8)
    b.endBlock(txl1);

    // mat1: one material, texMap 1 (texIdx 7 = OUT OF RANGE) + 1 tev stage.
    const u32 mat1 = b.beginBlock("mat1");
    put16(b.raw(), 1); put16(b.raw(), 0);
    put32(b.raw(), 16);
    putFixedStr(b.raw(), "mat_bad", 20);
    for (int i = 0; i < 3; ++i) {
        for (int c = 0; c < 4; ++c) {
            put16(b.raw(), 255);
        }
    }
    for (int i = 0; i < 16; ++i) {
        put8(b.raw(), 0);
    }
    put32(b.raw(), 0x00040101);  // texMap 1 | texCoordGen 1<<8 | tevStage 1<<18
    put16(b.raw(), 7);           // texIdx 7 — txl1 only has 1 entry
    put8(b.raw(), 0); put8(b.raw(), 0);
    put8(b.raw(), 1); put8(b.raw(), 4); put8(b.raw(), 60); put8(b.raw(), 0);  // TexCoordGen
    const u8 tevStage[16] = {0, 4, 0, 0, 0xFF, 0x8F, 0x00, 0x61, 0x77, 0x47, 0x00, 0x81, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        put8(b.raw(), tevStage[i]);
    }
    b.endBlock(mat1);

    const u32 rootPan = b.beginBlock("pan1");
    putPaneBody(b.raw(), "root_pane", 0, 0, 0, 1, 1, 640, 480);
    b.endBlock(rootPan);
    const u32 pas1 = b.beginBlock("pas1");
    b.endBlock(pas1);
    const u32 pic1 = b.beginBlock("pic1");
    putPaneBody(b.raw(), "picture", 0, 0, 0, 1, 1, 100, 50);
    for (int i = 0; i < 4; ++i) {
        put32(b.raw(), 0xFFFFFFFF);
    }
    put16(b.raw(), 0);  // materialIdx -> mat_bad
    put8(b.raw(), 0);   // texCoordNum
    put8(b.raw(), 0);
    b.endBlock(pic1);
    const u32 pae1 = b.beginBlock("pae1");
    b.endBlock(pae1);
    const u32 grp1 = b.beginBlock("grp1");
    putFixedStr(b.raw(), "root_group", 16);
    put16(b.raw(), 0); put16(b.raw(), 0);
    b.endBlock(grp1);

    std::vector<u8> file = b.finish();
    REQUIRE(Platform::CompatLyt::convertBrlyt(file.data(), static_cast<u32>(file.size())));

    nw4r::lyt::Layout::mspAllocator = &sTestAllocator;
    StubResourceAccessor accessor;

    nw4r::lyt::Layout layout;
    REQUIRE(layout.Build(file.data(), &accessor));  // pre-patch: SIGSEGV here
    REQUIRE(layout.mpRootPane != nullptr);

    // The guard must have substituted the empty name instead of the wild one.
    bool askedEmpty = false;
    for (const std::string& n : accessor.requested) {
        askedEmpty = askedEmpty || n.empty();
    }
    CHECK(askedEmpty);
}

TEST_CASE(lyt_draw_full_chain_headless) {
    // M9.5.3c: the Draw half of the layout engine. CalculateMtx fills the
    // global matrices; Draw runs the whole GX submit path headless: the
    // picture (Material::SetupGX + detail::SetVertexFormat + DrawQuad), the
    // textbox (CharWriter glyph quads through the synthetic brfnt), the
    // window (frame + content quads) and the bounding (empty DrawSelf). The
    // renderer is not initialized in the test binaries, so the compat GX
    // layer mirrors the state and drops the geometry — the case proves the
    // chain runs to completion and the orientation math is right.
    RichLayout lyt = makeRichBrlyt();
    REQUIRE(Platform::CompatLyt::convertBrlyt(lyt.file.data(),
                                              static_cast<u32>(lyt.file.size())));

    nw4r::lyt::Layout::mspAllocator = &sTestAllocator;
    StubResourceAccessor accessor;  // also owns the TPL/brfnt blobs Build reads

    nw4r::lyt::Layout layout;
    REQUIRE(layout.Build(lyt.file.data(), &accessor));

    nw4r::lyt::Pane* root = layout.mpRootPane;
    REQUIRE(root != nullptr);
    nw4r::lyt::Pane* pic = root->FindPaneByName("picture", true);
    REQUIRE(pic != nullptr);

    // Y-up DrawInfo, exactly what LayoutManager::initDrawInfo sets: top >
    // bottom makes IsYAxisUp() true, so Pane::LoadMtx reverses the Y basis
    // when loading (layout-space quads grow down from the pane origin while
    // the screen ortho is Y-up).
    nw4r::lyt::DrawInfo info;
    info.mViewRect = nw4r::ut::Rect(-304.0f, 228.0f, 304.0f, -228.0f);
    CHECK(info.IsYAxisUp());

    // Non-trivial alpha on the picture to exercise the glbAlpha propagation
    // (Pane::CalculateMtx -> DrawQuad's vertex-color modulation).
    pic->mAlpha = 100;

    layout.CalculateMtx(info);

    // Identity view mtx, root at the origin: the picture keeps its brlyt
    // translate (10, 20) in the global matrix, and its alpha is its own
    // (the root is fully opaque, so no influence scaling kicks in).
    CHECK_NEAR(pic->mGlbMtx.m[0][3], 10.0f, 1e-4);
    CHECK_NEAR(pic->mGlbMtx.m[1][3], 20.0f, 1e-4);
    CHECK_EQ(static_cast<int>(pic->mGlbAlpha), 100);

    // The full draw pass — the actual test: every pane type loads its matrix
    // (GXLoadPosMtxImm), sets up its material (chan ctrl / tex gens / tex
    // objects / tev stages) and submits its quads through the compat layer.
    // The full draw pass — the actual test: every pane type loads its matrix
    // (GXLoadPosMtxImm), sets up its material (chan ctrl / tex gens / tex
    // objects / tev stages) and submits its quads through the compat layer.
    layout.Draw(info);

    // Hidden panes are skipped by Pane::Draw; hide the picture (mFlag bit 0,
    // same write the RLVI animation target does) and draw once more so the
    // early-out runs too.
    pic->mFlag = static_cast<u8>(pic->mFlag & ~1u);
    CHECK(!pic->IsVisible());
    layout.Draw(info);
}

// ---------------------------------------------------------------------------
// GPU: layout pixels must land on the EFB (M9.5.3c black-screen hunt).
// Runs under Xvfb+lavapipe; SKIPs without video/Vulkan. Reproduces the user's
// boot state — a fully visible pane tree, textures uploaded, animations
// running — yet the EFB probes (0,0,0,0) everywhere. Draws the rich synthetic
// layout with the exact state LayoutManager::draw sets (Y-up DrawInfo,
// 608x456-centered ortho, full viewport) onto a NON-black clear color, reads
// the EFB back and requires changed pixels, logging their bounding box.
// ---------------------------------------------------------------------------
TEST_CASE(lyt_draw_lands_on_efb) {
    // Full-suite hygiene: game_system_boot_reaches_logo (and scene tests) leave
    // sCurrentHeap pointed at the stationed heap; the global operator new then
    // routes this test's MB-scale renderer/readback allocations into it and
    // they fail. Restore the root heap (pattern from layout_holder_test).
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }
    JKRHeap::sRootHeap->becomeCurrentHeap();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SKIP("SDL_Init failed (no video subsystem)");
        return;
    }
    SDL_Window* window = SDL_CreateWindow("galaxy-pc-test-lytdraw", 128, 128,
                                          SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
    if (!window) {
        SDL_Quit();
        SKIP("SDL_CreateWindow (hidden, Vulkan) failed");
        return;
    }
    Platform::RendererConfig cfg{};
    cfg.appName = "galaxy-pc-tests";
    // Validation OFF by default here: the global operator new routes every
    // allocation (including the validation layer's ~4 MiB fixed bookkeeping)
    // into the JKR arena, which late-in-suite is too fragmented to satisfy it
    // (JKR alloc failure -> HeapMemoryWatcher panic). Set
    // PL_GPU_TEST_VALIDATION=1 to enable it when debugging this test alone —
    // it caught the shared-descriptor-set and destroyed-image-view bugs.
    cfg.enableValidation = SDL_getenv("PL_GPU_TEST_VALIDATION") != nullptr;
    cfg.vsync = false;
    if (!Platform::Renderer::init(window, cfg)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        SKIP("Platform::Renderer init failed (no Vulkan surface/ICD)");
        return;
    }
    Platform::Renderer& r = Platform::Renderer::instance();
    REQUIRE(r.isInitialized());

    // The rich synthetic layout: picture (textured, TPL 2x2 RGB565 R/G/B/W)
    // + textbox (brfnt glyphs) + window panes.
    RichLayout lyt = makeRichBrlyt();
    REQUIRE(Platform::CompatLyt::convertBrlyt(lyt.file.data(),
                                              static_cast<u32>(lyt.file.size())));
    nw4r::lyt::Layout::mspAllocator = &sTestAllocator;
    StubResourceAccessor accessor;
    nw4r::lyt::Layout layout;
    REQUIRE(layout.Build(lyt.file.data(), &accessor));

    nw4r::lyt::DrawInfo info;
    info.mViewRect = nw4r::ut::Rect(-304.0f, 228.0f, 304.0f, -228.0f);
    CHECK(info.IsYAxisUp());
    layout.CalculateMtx(info);

    // The GX state LayoutManager::draw + setupDrawForNW4RLayout establish
    // around mLayout->Draw (SceneCompat.cpp), mirrored for the 640x456 EFB.
    GXInit(nullptr, 0);
    GXSetDispCopySrc(0, 0, 640, 456); // EFB = 640x456, like the real boot
    GXSetViewport(0.0f, 0.0f, 640.0f, 456.0f, 0.0f, 1.0f);
    GXSetScissor(0, 0, 640, 456);
    Mtx44 projMtx;
    C_MTXOrtho(projMtx, 228.0f, -228.0f, -304.0f, 304.0f, -10000.0f, 10000.0f);
    GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    // A clear color no layout output can produce, so "nothing drawn" cannot
    // masquerade as "drawn black".
    GXSetCopyClear(GXColor{40, 60, 80, 255}, 0xFFFFFF);

    REQUIRE(r.beginFrame());
    GXClear(GX_CLEAR_COLOR);
    r.beginPass(static_cast<Platform::RenderTargetHandle>(
        Platform::CompatGx::getEfbRenderTarget()));
    REQUIRE(r.inPass());

    layout.Draw(info);

    // Debug snapshot of the last submitted primitive (parsed vertex floats in
    // VCD order): catches vertex-stream misparses (stride/descriptor bugs)
    // without any GPU ambiguity.
    {
        int count = 0, stride = 0;
        const float* verts = GXCompatDebugVertices(&count, &stride);
        fprintf(stderr, "[lyt-efb] last prim: %d verts x %d floats\n", count, stride);
        for (int i = 0; verts && i < count && i < 4; i++) {
            fprintf(stderr, "[lyt-efb]   v%d:", i);
            for (int j = 0; j < stride && j < 10; j++) {
                fprintf(stderr, " %.3f", verts[i * stride + j]);
            }
            fprintf(stderr, "\n");
        }
    }

    // Control quads: red, untextured, identity posmtx, drawn through the SAME
    // immediate path with the layout's ortho still active. Ctrl A uses the
    // minimal state of the proven gx_copy_test path; Ctrl B adds the explicit
    // chan-ctrl + tev-order calls (what lyt's SetupGX does). Both must land
    // red; a white B pinpoints the TEV order/channel mirror. They sit left
    // and right of the layout content (picture lands ~x 331..436).
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    Mtx idn;
    PSMTXIdentity(idn);
    GXLoadPosMtxImm(idn, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_NONE, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    auto quad = [&](float x0, float y0, float x1, float y1, u32 rgba) {
        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition2f32(x0, y0); GXColor1u32(rgba);
        GXPosition2f32(x1, y0); GXColor1u32(rgba);
        GXPosition2f32(x1, y1); GXColor1u32(rgba);
        GXPosition2f32(x0, y1); GXColor1u32(rgba);
        GXEnd();
    };
    quad(-290.0f, -60.0f, -160.0f, 60.0f, 0xFF0000FFu); // ctrl A (left)
    // Ctrl B: the lyt-style explicit state before the same red quad.
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL,
                  GX_DF_NONE, GX_AF_NONE);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    quad(160.0f, -60.0f, 290.0f, 60.0f, 0xFF0000FFu);   // ctrl B (right)

    // Orientation marker (M9.5.3c: the user's strap screen rendered upside
    // down — GL-style Y-up clip space fed straight into Vulkan's Y-down NDC,
    // and every synthetic check so far was vertically symmetric, so the flip
    // was invisible). A GREEN quad near the TOP of layout space: with the
    // console orientation (EFB row 0 = top, like GXCopyDisp/GXCopyTex) it
    // must land in the top rows of the EFB. The check below pins it forever.
    quad(-60.0f, 190.0f, 60.0f, 225.0f, 0x00FF00FFu); // top-center, green

    // Regression (M9.5.3c): the Material::SetupGX pattern — ONE stack
    // GXTexObj reprogrammed for each texmap — plus clearEfb's failed Z24X8
    // load used to leave a DESTROYED handle in TEXMAP0 (the second texture
    // loads into slot 1, nothing re-bound slot 0), and the next flushDraw
    // crashed the driver inside vkUpdateDescriptorSets. The fix unbinds
    // matching TEXMAP slots whenever a host texture is destroyed and makes a
    // failed load unbind its slot. Pin both behaviors on the handle state,
    // then force a flushDraw with the scrubbed maps (pre-fix: driver crash
    // right here).
    {
        static u8 texA[64], texB[64];
        for (int i = 0; i < 64; ++i) {
            texA[i] = 0x11;
            texB[i] = 0x22;
        }
        GXTexObj stackObj;  // ONE address — nw4r's SetupGX pattern
        std::memset(&stackObj, 0, sizeof(stackObj));
        GXInitTexObj(&stackObj, texA, 8, 8, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&stackObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&stackObj, GX_TEXMAP0);
        void* map0First = nullptr;
        void* sam0 = nullptr;
        Platform::CompatGx::getTexMap0(&map0First, &sam0);
        REQUIRE(map0First != nullptr);

        // Same address, new content: destroys map0First's renderer texture
        // and loads the new content into TEXMAP1. Slot 0 must NOT keep the
        // destroyed handle (pre-fix: dangling -> driver crash below).
        GXInitTexObj(&stackObj, texB, 8, 8, GX_TF_I4, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&stackObj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&stackObj, GX_TEXMAP1);
        void* maps[8];
        void* sams[8];
        Platform::CompatGx::getTexMaps(maps, sams);
        CHECK(maps[0] == nullptr);           // scrubbed at destroy (pre-fix: == map0First, freed)
        CHECK(maps[1] != nullptr);           // the new content is bound
        CHECK(maps[1] != map0First);

        // clearEfb's Z24X8 object: unsupported -> failed load must unbind its
        // slot (pre-fix: left whatever was there — including freed handles).
        static u32 zdata[16] = {};
        GXTexObj zobj;
        std::memset(&zobj, 0, sizeof(zobj));
        GXInitTexObj(&zobj, zdata, 4, 4, GX_TF_Z24X8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&zobj, GX_NEAR, GX_NEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GXLoadTexObj(&zobj, GX_TEXMAP0);
        Platform::CompatGx::getTexMaps(maps, sams);
        CHECK(maps[0] == nullptr);

        // The bind itself: white fallbacks for the null maps, a quad through
        // flushDraw. Pre-fix this was the crash site.
        quad(-60.0f, 150.0f, 60.0f, 185.0f, 0xFFFFFFFFu);
    }


    r.endPass();
    r.flushFrame(); // submit so the synchronous readback sees this frame

    const u32 w = 640, h = 456;
    // Readback via malloc, NOT operator new: the global new is routed into the
    // JKR root heap, which after the boot tests have carved their child heaps
    // has only ~1 MB free in a full-suite run — the 1.1 MB buffer would panic
    // the HeapMemoryWatcher (suite-order dependent).
    const size_t rgbaBytes = static_cast<size_t>(w) * h * 4;
    std::unique_ptr<uint8_t[], void (*)(void*)> rgba(
        static_cast<uint8_t*>(std::malloc(rgbaBytes)), &std::free);
    REQUIRE(rgba != nullptr);
    std::memset(rgba.get(), 0, rgbaBytes);
    REQUIRE(r.readRenderTarget(static_cast<Platform::RenderTargetHandle>(
        Platform::CompatGx::getEfbRenderTarget()), 0, 0, w, h, rgba.get()));

    auto differs = [&](const uint8_t* p) {
        return std::abs(int(p[0]) - 40) > 8 || std::abs(int(p[1]) - 60) > 8 ||
               std::abs(int(p[2]) - 80) > 8 || std::abs(int(p[3]) - 255) > 8;
    };
    auto isRed = [&](const uint8_t* p) {
        return p[0] > 200 && p[1] < 60 && p[2] < 60;
    };
    auto px = [&](u32 x, u32 y) -> const uint8_t* {
        return &rgba.get()[(static_cast<size_t>(y) * w + x) * 4];
    };
    auto countIn = [&](u32 x0, u32 y0, u32 x1, u32 y1, auto pred) {
        size_t n = 0;
        for (u32 y = y0; y <= y1 && y < h; y++) {
            for (u32 x = x0; x <= x1 && x < w; x++) {
                if (pred(px(x, y))) n++;
            }
        }
        return n;
    };
    size_t changed = 0;
    u32 minX = w, minY = h, maxX = 0, maxY = 0;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            const uint8_t* p = px(x, y);
            if (differs(p)) {
                changed++;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    // Ctrl A expected at EFB x 15..151, y 168..288; ctrl B at x 489..625.
    const size_t redA = countIn(20, 175, 145, 280, isRed);
    const size_t redB = countIn(495, 175, 620, 280, isRed);
    // The layout's picture pane: layout x 10..110, y 20..-30 -> EFB x 331..436,
    // y 208..258 (textured red TPL modulated white, alpha 100 over the clear).
    const size_t layoutPix = countIn(340, 215, 425, 250, differs);
    const uint8_t* cA = px(80, 228);
    const uint8_t* cB = px(560, 228);
    const uint8_t* cL = px(380, 232);

    // Orientation (console: EFB row 0 = TOP — GXCopyDisp/GXCopyTex semantics).
    // The green marker quad spans layout y 190..225, x -60..60 -> with the
    // correct orientation it rasterizes into EFB rows ~3..38, x ~276..364.
    auto isGreen = [](const uint8_t* p) {
        return p[1] > 200 && p[0] < 80 && p[2] < 80;
    };
    const size_t greenTop = countIn(280, 2, 360, 60, isGreen);       // where it MUST be
    const size_t greenBottom = countIn(280, 396, 360, 454, isGreen); // where a Y-flip puts it
    fprintf(stderr, "[lyt-efb] green top=%zu bottom=%zu (orientation %s)\n",
            greenTop, greenBottom, greenTop > 1000 ? "OK" : "FLIPPED");
    CHECK(greenTop > 1000);      // the marker lands at the TOP rows
    CHECK(greenBottom < 100);    // ...and nowhere at the bottom (pre-fix: flipped)

    fprintf(stderr, "[lyt-efb] changed=%zu/%u bbox=(%u,%u)-(%u,%u)\n"
                    "[lyt-efb] redA=%zu A=(%u,%u,%u,%u) redB=%zu B=(%u,%u,%u,%u) "
                    "layoutPix=%zu L=(%u,%u,%u,%u)\n",
            changed, w * h, changed ? minX : 0, changed ? minY : 0,
            changed ? maxX : 0, changed ? maxY : 0,
            redA, cA[0], cA[1], cA[2], cA[3], redB, cB[0], cB[1], cB[2], cB[3],
            layoutPix, cL[0], cL[1], cL[2], cL[3]);
    CHECK(redA > 10000);   // ctrl A: minimal-state red quad
    CHECK(redB > 10000);   // ctrl B: + lyt-style chan/tev-order state
    CHECK(layoutPix > 1000); // the layout's picture pane itself

    GXCopyDisp(nullptr, GX_TRUE);
    r.endFrame();
    GXCompatEndFrame();

    GXCompatShutdown();
    Platform::Renderer::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Synthetic brlan (RLAN) — M9.5.3b. Bottom-up assembly in big-endian (the
// putXX helpers write BE, same as BrlytBuilder): entries -> tags -> contents
// -> pai1 -> file. Offsets are patched once every sub-blob size is known.
// ---------------------------------------------------------------------------

std::vector<u8> makeHermiteKeys(const float (*keys)[3], int num) {
    std::vector<u8> v;
    for (int i = 0; i < num; ++i) {
        putF32(v, keys[i][0]);  // frame
        putF32(v, keys[i][1]);  // value
        putF32(v, keys[i][2]);  // slope
    }
    return v;
}

std::vector<u8> makeStepKeys(const float (*frames)[1], const u16* values, int num) {
    std::vector<u8> v;
    for (int i = 0; i < num; ++i) {
        putF32(v, frames[i][0]);
        put16(v, values[i]);
        put16(v, 0);  // pad
    }
    return v;
}

std::vector<u8> makeBrlanEntry(u8 index, u8 target, u8 keyType, u16 keyNum,
                               const std::vector<u8>& keys) {
    std::vector<u8> e;
    put8(e, index);
    put8(e, target);
    put8(e, keyType);
    put8(e, 0);
    put16(e, keyNum);
    put16(e, 0);
    put32(e, 0x0C);  // keysOffset (entry-relative)
    e.insert(e.end(), keys.begin(), keys.end());
    return e;
}

std::vector<u8> makeBrlanTag(const char* kind, const std::vector<std::vector<u8>>& entries) {
    std::vector<u8> t;
    putBytes(t, reinterpret_cast<const u8*>(kind), 4);
    put8(t, static_cast<u8>(entries.size()));
    put8(t, 0); put8(t, 0); put8(t, 0);

    std::vector<size_t> offPatches;
    for (size_t i = 0; i < entries.size(); ++i) {
        offPatches.push_back(t.size());
        put32(t, 0);  // patched below
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        patch32(t, offPatches[i], static_cast<u32>(t.size()));  // tag-relative
        t.insert(t.end(), entries[i].begin(), entries[i].end());
    }
    return t;
}

std::vector<u8> makeBrlanContent(const char* name, u8 targetKind,
                                 const std::vector<std::vector<u8>>& tags) {
    std::vector<u8> c;
    putFixedStr(c, name, 0x14);
    put8(c, static_cast<u8>(tags.size()));
    put8(c, targetKind);  // 0 = pane, 1 = material
    put16(c, 0);

    std::vector<size_t> offPatches;
    for (size_t i = 0; i < tags.size(); ++i) {
        offPatches.push_back(c.size());
        put32(c, 0);
    }
    for (size_t i = 0; i < tags.size(); ++i) {
        patch32(c, offPatches[i], static_cast<u32>(c.size()));  // content-relative
        c.insert(c.end(), tags[i].begin(), tags[i].end());
    }
    return c;
}

// The test brlan: frameSize 10, no loop, no texture files, two contents:
//   pane "root_pane":  RLPA TranslateX hermite (0,0,s20)->(10,100,s0)  [@5 = 75]
//                      RLPA ScaleX     hermite (0,1)->(10,2)          [@5 = 1.5]
//                      RLVI Visibility step    1 until f5, then 0
//                      RLVC PaneAlpha  hermite (0,255)->(10,0)        [@5 = 127.5]
//   material "mat_test": RLMC TevColor0_g hermite (0,0)->(10,200)     [@5 = 100]
//                        RLTS TranslateS    hermite (0,0)->(10,1)      [@5 = 0.5]
std::vector<u8> makeTestBrlan() {
    const float translateX[2][3] = {{0.0f, 0.0f, 20.0f}, {10.0f, 100.0f, 0.0f}};
    const float scaleX[2][3] = {{0.0f, 1.0f, 0.0f}, {10.0f, 2.0f, 0.0f}};
    const float alpha[2][3] = {{0.0f, 255.0f, 0.0f}, {10.0f, 0.0f, 0.0f}};
    const float visFrames[2][1] = {{0.0f}, {5.0f}};
    const u16 visValues[2] = {1, 0};
    const float tevg[2][3] = {{0.0f, 0.0f, 0.0f}, {10.0f, 200.0f, 0.0f}};
    const float texS[2][3] = {{0.0f, 0.0f, 0.0f}, {10.0f, 1.0f, 0.0f}};

    std::vector<std::vector<u8>> rlpaEntries;
    rlpaEntries.push_back(makeBrlanEntry(0, 0x00, 2, 2, makeHermiteKeys(translateX, 2)));
    rlpaEntries.push_back(makeBrlanEntry(0, 0x06, 2, 2, makeHermiteKeys(scaleX, 2)));

    std::vector<std::vector<u8>> rlviEntries;
    rlviEntries.push_back(makeBrlanEntry(0, 0x00, 1, 2, makeStepKeys(visFrames, visValues, 2)));

    std::vector<std::vector<u8>> rlvcEntries;
    rlvcEntries.push_back(makeBrlanEntry(0, 0x10, 2, 2, makeHermiteKeys(alpha, 2)));

    std::vector<std::vector<u8>> paneTags;
    paneTags.push_back(makeBrlanTag("RLPA", rlpaEntries));
    paneTags.push_back(makeBrlanTag("RLVI", rlviEntries));
    paneTags.push_back(makeBrlanTag("RLVC", rlvcEntries));

    std::vector<std::vector<u8>> rltags;
    rltags.push_back(makeBrlanEntry(0, 0x05, 2, 2, makeHermiteKeys(tevg, 2)));   // RLMC TevColor0_g
    rltags.push_back(makeBrlanEntry(0, 0x00, 2, 2, makeHermiteKeys(texS, 2)));   // RLTS TranslateS
    std::vector<std::vector<u8>> matTags;
    matTags.push_back(makeBrlanTag("RLMC", {rltags[0]}));
    matTags.push_back(makeBrlanTag("RLTS", {rltags[1]}));

    std::vector<u8> content0 = makeBrlanContent("root_pane", 0, paneTags);
    std::vector<u8> content1 = makeBrlanContent("mat_test", 1, matTags);

    // pai1 body (block-relative offsets are body offset + 8).
    std::vector<u8> body;
    put16(body, 10);  // frameSize
    put8(body, 0);    // loop
    put8(body, 0);    // pad
    put16(body, 0);   // fileNum
    put16(body, 2);   // animContNum
    put32(body, 0x14);  // animContOffsetsOffset (block-relative) = body+0x0C

    const size_t contOffsPos = body.size();  // == 0x0C, i.e. block+0x14
    put32(body, 0);
    put32(body, 0);
    const u32 content0Off = static_cast<u32>(8 + body.size());
    body.insert(body.end(), content0.begin(), content0.end());
    const u32 content1Off = static_cast<u32>(8 + body.size());
    body.insert(body.end(), content1.begin(), content1.end());
    patch32(body, contOffsPos + 0, content0Off);
    patch32(body, contOffsPos + 4, content1Off);

    // File: header + single pai1 block.
    std::vector<u8> block;
    putBytes(block, reinterpret_cast<const u8*>("pai1"), 4);
    put32(block, static_cast<u32>(8 + body.size()));
    block.insert(block.end(), body.begin(), body.end());

    std::vector<u8> file;
    putBytes(file, reinterpret_cast<const u8*>("RLAN"), 4);
    put16(file, 0xFEFF);  // BOM: FE FF bytes on disk
    put16(file, 0x000A);  // version
    put32(file, static_cast<u32>(16 + block.size()));
    put16(file, 16);  // headerSize
    put16(file, 1);   // dataBlocks
    file.insert(file.end(), block.begin(), block.end());
    return file;
}

TEST_CASE(brlan_binds_and_animates_pane_and_material) {
    // Layout under test: the rich brlyt (root_pane + picture/material
    // mat_test + friends).
    RichLayout lyt = makeRichBrlyt();
    REQUIRE(Platform::CompatLyt::convertBrlyt(lyt.file.data(), static_cast<u32>(lyt.file.size())));

    nw4r::lyt::Layout::mspAllocator = &sTestAllocator;
    StubResourceAccessor accessor;

    nw4r::lyt::Layout layout;
    REQUIRE(layout.Build(lyt.file.data(), &accessor));
    nw4r::lyt::Pane* root = layout.mpRootPane;
    REQUIRE(root != nullptr);

    // brlan: BE blob -> host-native (idempotent), parsed by AnimResource.
    std::vector<u8> brlan = makeTestBrlan();
    CHECK(Platform::CompatLyt::isBrlan(brlan.data(), static_cast<u32>(brlan.size())));
    REQUIRE(Platform::CompatLyt::convertBrlan(brlan.data(), static_cast<u32>(brlan.size())));
    CHECK(Platform::CompatLyt::convertBrlan(brlan.data(), static_cast<u32>(brlan.size())));

    // Production path: Layout::CreateAnimTransform -> AnimResource::Set ->
    // AnimTransformBasic::SetResource (the transform lands in the layout's
    // mAnimTransList, cleaned up by ~Layout).
    nw4r::lyt::AnimTransform* pTrans = layout.CreateAnimTransform(brlan.data(), &accessor);
    REQUIRE(pTrans != nullptr);
    CHECK_EQ(static_cast<int>(pTrans->GetFrameSize()), 10);
    CHECK(!pTrans->IsLoopData());

    layout.BindAnimation(pTrans);

    // Frame 5: hermite midpoints + the step key flipped visibility off.
    pTrans->mFrame = 5.0f;
    layout.Animate(0);

    CHECK_NEAR(root->mTranslate.x, 75.0f, 0.01f);
    CHECK_NEAR(root->mScale.x, 1.5f, 0.01f);
    CHECK(!root->IsVisible());
    CHECK_EQ(static_cast<int>(root->mAlpha), 127);  // 127.5 truncated

    nw4r::lyt::Pane* pic = root->FindPaneByName("picture", true);
    REQUIRE(pic != nullptr);
    nw4r::lyt::Material* mat = pic->mpMaterial;
    REQUIRE(mat != nullptr);
    CHECK_EQ(static_cast<int>(mat->mTevCols[0].g), 100);
    REQUIRE(mat->GetTexSRTAry() != nullptr);
    CHECK_NEAR(mat->GetTexSRTAry()[0].translate.x, 0.5f, 0.01f);

    // Frame 3: visibility back on, curve re-evaluated.
    pTrans->mFrame = 3.0f;
    layout.Animate(0);
    CHECK(root->IsVisible());
    CHECK_NEAR(root->mTranslate.x, 51.0f, 0.5f);

    // Clamping past the last key.
    pTrans->mFrame = 20.0f;
    layout.Animate(0);
    CHECK_NEAR(root->mTranslate.x, 100.0f, 0.001f);
    CHECK_EQ(static_cast<int>(root->mAlpha), 0);

    // Garbage must be refused, not crash.
    u8 garbage[32];
    std::memset(garbage, 0x5A, sizeof(garbage));
    CHECK(!Platform::CompatLyt::convertBrlan(garbage, sizeof(garbage)));
    nw4r::lyt::AnimResource badRes;
    badRes.Set(garbage);
    CHECK(badRes.mpFileHeader == nullptr);
    CHECK(badRes.GetResourceBlock() == nullptr);
    CHECK_EQ(static_cast<int>(badRes.GetGroupNum()), 0);
}

// ---------------------------------------------------------------------------
// TPL host conversion
// ---------------------------------------------------------------------------

TEST_CASE(tpl_host_converts_synthetic) {
    std::vector<u8> tpl = makeTpl();

    TPLPalettePtr host = Platform::CompatGx::tplToHost(tpl.data());
    REQUIRE(host != nullptr);
    CHECK_EQ(static_cast<int>(host->numDescriptors), 1);
    REQUIRE(host->descriptorArray != nullptr);

    const TPLHeader* th = host->descriptorArray[0].textureHeader;
    REQUIRE(th != nullptr);
    CHECK_EQ(static_cast<int>(th->width), 4);
    CHECK_EQ(static_cast<int>(th->height), 4);
    CHECK_EQ(static_cast<int>(th->format), 4);  // GX_TF_RGB565
    CHECK(th->data == reinterpret_cast<char*>(tpl.data() + 56));
    CHECK_EQ(static_cast<int>(th->minFilter), 1);
    CHECK(host->descriptorArray[0].CLUTHeader == nullptr);

    // Cached: same source pointer -> same conversion.
    CHECK(Platform::CompatGx::tplToHost(tpl.data()) == host);

    // Garbage -> nullptr.
    u8 garbage[32];
    std::memset(garbage, 0x11, sizeof(garbage));
    CHECK(Platform::CompatGx::tplToHost(garbage) == nullptr);

    // TPLGet accessor over the converted palette.
    const TPLDescriptor* desc = TPLGet(host, 0);
    REQUIRE(desc != nullptr);
    CHECK(desc->textureHeader == th);
}

// ---------------------------------------------------------------------------
// brfnt -> ResFont
// ---------------------------------------------------------------------------

TEST_CASE(resfont_setresource_be_brfnt) {
    std::vector<u8> font = makeBrfnt();

    nw4r::ut::ResFont resFont;
    REQUIRE(resFont.SetResource(font.data()));

    CHECK_EQ(resFont.GetLineFeed(), 18);
    CHECK_EQ(resFont.GetWidth(), 8);
    CHECK_EQ(resFont.GetHeight(), 16);
    CHECK_EQ(static_cast<int>(resFont.GetEncoding()),
             static_cast<int>(nw4r::ut::FONT_ENCODING_UTF8));

    // CMAP 0x20..0x22 -> glyphs 0..2; CWDH widths {7,5,9}.
    CHECK_EQ(resFont.GetCharWidth(0x20), 7);
    CHECK_EQ(resFont.GetCharWidth(0x21), 5);
    CHECK_EQ(resFont.GetCharWidth(0x22), 9);
    CHECK_EQ(static_cast<int>(resFont.GetCharWidths(0x21).glyphWidth), 5);

    // Second attach to the same blob hits the conversion cache.
    nw4r::ut::ResFont second;
    REQUIRE(second.SetResource(font.data()));
    CHECK_EQ(second.GetLineFeed(), 18);

    // Garbage must be rejected, not crash.
    u8 garbage[64];
    std::memset(garbage, 0x33, sizeof(garbage));
    nw4r::ut::ResFont bad;
    CHECK(!bad.SetResource(garbage));
    CHECK(!bad.SetResource(nullptr));
}
