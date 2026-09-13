// =============================================================================
// M9.5.4 v8: compat/j3d — host BMD parser/renderer + BTK/BCK + TitleSky.
//
// Headless (no GPU): the renderer drives the compat GX state machine and the
// captured vertex stream (GXCompatDebugVertices) proves the packet display
// lists replay through the same path the game's GX code uses.
//
// The synthetic files follow the J3D binary layouts the parser reads
// (J3DModelLoader / J3DMaterialFactory / J3DAnmLoader block structs, see the
// comments in compat/j3d/BmdModel.cpp). Every offset is big-endian.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/dvd/DVDCompat.h"
#include "compat/gx/GXCompat.h"
#include "compat/j3d/BckAnim.h"
#include "compat/j3d/BmdModel.h"
#include "compat/j3d/BmdRenderer.h"
#include "compat/j3d/BtkAnim.h"
#include "compat/j3d/J3DMathCompat.h"
#include "compat/j3d/TitleSky.h"

#include "platform/Filesystem/Filesystem.h"

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>

#include <revolution/gx.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace compat::j3d;

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// Big-endian blob writer.
// ---------------------------------------------------------------------------
struct Blob {
    std::vector<u8> v;

    size_t size() const { return v.size(); }
    void u8_(u8 x) { v.push_back(x); }
    void u16_(u16 x) {
        v.push_back(static_cast<u8>(x >> 8));
        v.push_back(static_cast<u8>(x));
    }
    void u32_(u32 x) {
        v.push_back(static_cast<u8>(x >> 24));
        v.push_back(static_cast<u8>(x >> 16));
        v.push_back(static_cast<u8>(x >> 8));
        v.push_back(static_cast<u8>(x));
    }
    void f32_(f32 x) {
        u32 bits;
        std::memcpy(&bits, &x, 4);
        u32_(bits);
    }
    void s16_(s16 x) { u16_(static_cast<u16>(x)); }
    void fill(size_t n, u8 x) { v.insert(v.end(), n, x); }
    void align(size_t a, u8 x = 0) {
        while (v.size() % a != 0) v.push_back(x);
    }
    void patch32(size_t off, u32 x) {
        v[off] = static_cast<u8>(x >> 24);
        v[off + 1] = static_cast<u8>(x >> 16);
        v[off + 2] = static_cast<u8>(x >> 8);
        v[off + 3] = static_cast<u8>(x);
    }
    void bytes(const std::vector<u8>& b) { v.insert(v.end(), b.begin(), b.end()); }
    void str(const char* s) {
        while (*s) v.push_back(static_cast<u8>(*s++));
    }
};

// J3D ResNTAB: u16 count, u16 pad, {u16 hash, u16 offset}[count], strings.
std::vector<u8> nameTable(const std::vector<std::string>& names) {
    Blob b;
    b.u16_(static_cast<u16>(names.size()));
    b.u16_(0xFFFF);
    const size_t table = b.size();
    for (size_t i = 0; i < names.size(); ++i) {
        b.u16_(0);
        b.u16_(0);
    }
    for (size_t i = 0; i < names.size(); ++i) {
        u16 hash = 0;
        for (char c : names[i]) hash = static_cast<u16>(hash * 3 + static_cast<u8>(c));
        const size_t off = b.size();
        b.v[table + i * 4] = static_cast<u8>(hash >> 8);
        b.v[table + i * 4 + 1] = static_cast<u8>(hash);
        b.v[table + i * 4 + 2] = static_cast<u8>(off >> 8);
        b.v[table + i * 4 + 3] = static_cast<u8>(off);
        b.str(names[i].c_str());
        b.u8_(0);
    }
    return b.v;
}

// Opens a chunk ("INF1" etc.), returns the offset of the size field; closeChunk
// pads to 0x20 and patches the size.
size_t openChunk(Blob& b, const char* tag) {
    b.str(tag);
    const size_t sizePos = b.size();
    b.u32_(0);
    return sizePos;
}
void closeChunk(Blob& b, size_t sizePos) {
    b.align(0x20, 0);
    b.patch32(sizePos, static_cast<u32>(b.size() - (sizePos - 4)));
}

struct SynthJoint {
    JointTransform t;
    s32 parent = -1;  // index of the parent (must be < own index)
};

struct SynthModel {
    std::vector<f32> positions;   // xyz triplets
    std::vector<u8> colors;       // rgba8 quads
    std::vector<u16> indices;     // triangle list (index into both arrays)
    std::vector<SynthJoint> joints;  // at least one
    std::string materialName = "SkyMat";
    u8 materialMode = 1;          // 1 OPA / 4 XLU
    u32 cullMode = GX_CULL_NONE;
    s32 attachJoint = -1;         // joint whose subtree holds material+shape (-1 = last)
};

// Builds a J3D2bmd3 file: INF1 + VTX1 (POS f32 / CLR0 RGBA8) + DRW1 + JNT1 +
// SHP1 (one single-matrix shape, POS/CLR0 INDEX16) + MAT3 v26 (one material,
// vertex colour pass-through, lighting off, no textures).
std::vector<u8> makeSyntheticBmd(const SynthModel& m) {
    Blob b;
    b.str("J3D2bmd3");
    b.u32_(0);      // file size (patched)
    b.u32_(6);      // block count
    b.fill(16, 0xFF);

    // --- INF1 ---------------------------------------------------------------
    {
        const size_t sp = openChunk(b, "INF1");
        b.u16_(0);          // flags: matrix calc Basic
        b.u16_(0xFFFF);
        b.u32_(1);          // packet count
        b.u32_(static_cast<u32>(m.positions.size() / 3));
        b.u32_(0x18);       // hierarchy offset
        // Joint chain: j0 { j1 { ... { mat { shape } } } } — every joint is the
        // child of the previous one unless `parent` says otherwise (the
        // synthetic models keep parents in index order, one chain).
        const s32 attach = m.attachJoint < 0 ? static_cast<s32>(m.joints.size()) - 1 : m.attachJoint;
        int depth = 0;
        for (size_t j = 0; j < m.joints.size(); ++j) {
            b.u16_(0x10);
            b.u16_(static_cast<u16>(j));
            if (static_cast<s32>(j) == attach) {
                b.u16_(0x01); b.u16_(0);
                b.u16_(0x11); b.u16_(0);   // material 0
                b.u16_(0x01); b.u16_(0);
                b.u16_(0x12); b.u16_(0);   // shape 0
                b.u16_(0x02); b.u16_(0);
                b.u16_(0x02); b.u16_(0);
            }
            if (j + 1 < m.joints.size()) {
                b.u16_(0x01); b.u16_(0);
                ++depth;
            }
        }
        for (int d = 0; d < depth; ++d) {
            b.u16_(0x02); b.u16_(0);
        }
        b.u16_(0x00); b.u16_(0);
        closeChunk(b, sp);
    }

    // --- VTX1 ---------------------------------------------------------------
    {
        const size_t sp = openChunk(b, "VTX1");
        const size_t base = sp - 4;
        b.u32_(0x40);                // format table offset
        const size_t slotTable = b.size();
        for (int i = 0; i < 13; ++i) b.u32_(0);
        // Format entries (0x10 each): POS XYZ F32, CLR0 RGBA RGBA8, terminator.
        b.u32_(GX_VA_POS);  b.u32_(GX_POS_XYZ);  b.u32_(GX_F32);   b.u8_(0); b.fill(3, 0xFF);
        b.u32_(GX_VA_CLR0); b.u32_(GX_CLR_RGBA); b.u32_(GX_RGBA8); b.u8_(0); b.fill(3, 0xFF);
        b.u32_(GX_VA_NULL); b.u32_(1);           b.u32_(0);        b.u8_(0); b.fill(3, 0xFF);
        b.align(0x20, 0);
        b.patch32(slotTable + 0 * 4, static_cast<u32>(b.size() - base));   // POS slot
        for (f32 f : m.positions) b.f32_(f);
        b.align(0x20, 0);
        b.patch32(slotTable + 3 * 4, static_cast<u32>(b.size() - base));   // CLR0 slot
        b.bytes(m.colors);
        closeChunk(b, sp);
    }

    // --- DRW1 ---------------------------------------------------------------
    {
        const size_t sp = openChunk(b, "DRW1");
        const u16 n = static_cast<u16>(m.joints.size());
        b.u16_(n);
        b.u16_(0xFFFF);
        b.u32_(0x14);                        // type table
        b.u32_(0x14 + ((n + 1u) & ~1u));     // index table (u16 aligned)
        for (u16 i = 0; i < n; ++i) b.u8_(0);    // 0 = joint matrix
        if (n & 1) b.u8_(0);
        for (u16 i = 0; i < n; ++i) b.u16_(i);   // joint index
        closeChunk(b, sp);
    }

    // --- JNT1 ---------------------------------------------------------------
    {
        const size_t sp = openChunk(b, "JNT1");
        const size_t base = sp - 4;
        const u16 n = static_cast<u16>(m.joints.size());
        b.u16_(n);
        b.u16_(0xFFFF);
        const size_t offs = b.size();
        b.u32_(0); b.u32_(0); b.u32_(0);     // table, remap, names (patched)
        b.align(0x20, 0);
        b.patch32(offs, static_cast<u32>(b.size() - base));
        for (const SynthJoint& j : m.joints) {
            b.u16_(0);                         // flags
            b.u8_(0);                          // scale compensate
            b.u8_(0xFF);
            for (int k = 0; k < 3; ++k) b.f32_(j.t.scale[k]);
            for (int k = 0; k < 3; ++k) b.s16_(j.t.rotation[k]);
            b.u16_(0xFFFF);
            for (int k = 0; k < 3; ++k) b.f32_(j.t.translation[k]);
            b.f32_(1000.0f);                   // radius
            for (int k = 0; k < 3; ++k) b.f32_(-1000.0f);
            for (int k = 0; k < 3; ++k) b.f32_(1000.0f);
        }
        b.patch32(offs + 4, static_cast<u32>(b.size() - base));
        for (u16 i = 0; i < n; ++i) b.u16_(i);
        b.align(4, 0);
        b.patch32(offs + 8, static_cast<u32>(b.size() - base));
        std::vector<std::string> names;
        for (u16 i = 0; i < n; ++i) names.push_back("joint" + std::to_string(i));
        b.bytes(nameTable(names));
        closeChunk(b, sp);
    }

    // --- SHP1 ---------------------------------------------------------------
    {
        const size_t sp = openChunk(b, "SHP1");
        const size_t base = sp - 4;
        b.u16_(1);              // shape count
        b.u16_(0xFFFF);
        const size_t offs = b.size();
        for (int i = 0; i < 8; ++i) b.u32_(0);   // init, remap, unused, decl, mtxTable, dl, mtxInit, drawInit
        b.align(0x20, 0);
        // Shape init (0x28).
        b.patch32(offs + 0, static_cast<u32>(b.size() - base));
        b.u8_(0);               // mtxType single
        b.u8_(0xFF);
        b.u16_(1);              // group count
        b.u16_(0);              // decl byte offset
        b.u16_(0);              // first mtx init
        b.u16_(0);              // first draw init
        b.u16_(0xFFFF);
        b.f32_(1000.0f);
        for (int k = 0; k < 3; ++k) b.f32_(-1000.0f);
        for (int k = 0; k < 3; ++k) b.f32_(1000.0f);
        // Remap.
        b.patch32(offs + 4, static_cast<u32>(b.size() - base));
        b.u16_(0);
        b.align(4, 0);
        // Decls.
        b.patch32(offs + 12, static_cast<u32>(b.size() - base));
        b.u32_(GX_VA_POS);  b.u32_(GX_INDEX16);
        b.u32_(GX_VA_CLR0); b.u32_(GX_INDEX16);
        b.u32_(GX_VA_NULL); b.u32_(GX_NONE);
        // Matrix table.
        b.patch32(offs + 16, static_cast<u32>(b.size() - base));
        const s32 attach = m.attachJoint < 0 ? static_cast<s32>(m.joints.size()) - 1 : m.attachJoint;
        b.u16_(static_cast<u16>(attach));  // DRW1 index == joint index here
        b.align(0x20, 0);
        // Display list: one GX_TRIANGLES primitive, INDEX16 pos + INDEX16 clr0.
        b.patch32(offs + 20, static_cast<u32>(b.size() - base));
        const size_t dlStart = b.size();
        b.u8_(static_cast<u8>(GX_TRIANGLES) | static_cast<u8>(GX_VTXFMT0));
        b.u16_(static_cast<u16>(m.indices.size()));
        for (u16 idx : m.indices) {
            b.u16_(idx);
            b.u16_(idx);
        }
        b.align(0x20, 0);      // NOP padding
        const u32 dlSize = static_cast<u32>(b.size() - dlStart);
        // Mtx init (8): u16 unk, u16 count, u32 first index.
        b.patch32(offs + 24, static_cast<u32>(b.size() - base));
        b.u16_(0); b.u16_(1); b.u32_(0);
        // Draw init (8): u32 size, u32 offset (relative to the DL table).
        b.patch32(offs + 28, static_cast<u32>(b.size() - base));
        b.u32_(dlSize); b.u32_(0);
        closeChunk(b, sp);
    }

    // --- MAT3 (v26) -----------------------------------------------------------
    {
        const size_t sp = openChunk(b, "MAT3");
        const size_t base = sp - 4;
        b.u16_(1);
        b.u16_(0xFFFF);
        const size_t offs = b.size();          // 30 u32 table offsets (0x0C..0x84)
        for (int i = 0; i < 30; ++i) b.u32_(0);
        auto setOffs = [&](int slot) { b.patch32(offs + static_cast<size_t>(slot) * 4, static_cast<u32>(b.size() - base)); };
        // Material entry (0x14C): everything 0xFF, then the indices we use.
        setOffs(0);
        const size_t entry = b.size();
        b.fill(0x14C, 0xFF);
        b.v[entry + 0x00] = m.materialMode;
        b.v[entry + 0x01] = 0;     // cull
        b.v[entry + 0x02] = 0;     // chanNum
        b.v[entry + 0x03] = 0;     // texGenNum
        b.v[entry + 0x04] = 0;     // tevStageNum
        b.v[entry + 0x05] = 0;     // zCompLoc
        b.v[entry + 0x06] = 0;     // zMode
        b.v[entry + 0x07] = 0;     // dither
        auto setU16 = [&](size_t at, u16 x) {
            b.v[entry + at] = static_cast<u8>(x >> 8);
            b.v[entry + at + 1] = static_cast<u8>(x);
        };
        setU16(0x08, 0);           // matColor[0]
        setU16(0x0C, 0);           // chan COLOR0
        setU16(0x14, 0);           // amb[0]
        setU16(0x9C - 0x9C + 0xBC, 0);  // tevOrder[0]
        b.v[entry + 0x9C] = 0x0C;  // kColorSel[0] = KCSEL_1
        b.v[entry + 0xAC] = 0x1C;  // kAlphaSel[0] = KASEL_1
        setU16(0xDC, 0);           // tevColor[0]
        setU16(0xE4, 0);           // tevStage[0]
        setU16(0x146, 0);          // alphaComp
        setU16(0x148, 0);          // blend
        // Remap + names.
        setOffs(1);
        b.u16_(0);
        b.align(4, 0);
        setOffs(2);
        b.bytes(nameTable({m.materialName}));
        b.align(4, 0);
        // Cull modes (u32).
        setOffs(4);
        b.u32_(m.cullMode);
        // Material colours.
        setOffs(5);
        b.u8_(255); b.u8_(255); b.u8_(255); b.u8_(255);
        // Channel counts.
        setOffs(6);
        b.u8_(1); b.fill(3, 0xFF);
        // Channel controls (8): enable, matSrc, lightMask, diffuseFn, attnFn, ambSrc, pad.
        setOffs(7);
        b.u8_(0); b.u8_(GX_SRC_VTX); b.u8_(0); b.u8_(GX_DF_NONE); b.u8_(GX_AF_NONE); b.u8_(GX_SRC_REG); b.u16_(0xFFFF);
        // Ambient colours.
        setOffs(8);
        b.u8_(50); b.u8_(50); b.u8_(50); b.u8_(255);
        // Tex-gen counts.
        setOffs(10);
        b.u8_(0); b.fill(3, 0xFF);
        // TEV orders (4): texCoord, texMap, colorChan, pad.
        setOffs(16);
        b.u8_(0xFF); b.u8_(0xFF); b.u8_(GX_COLOR0A0); b.u8_(0xFF);
        // TEV colours (S16 x4).
        setOffs(17);
        b.s16_(255); b.s16_(255); b.s16_(255); b.s16_(255);
        // Konst colours.
        setOffs(18);
        b.u8_(255); b.u8_(255); b.u8_(255); b.u8_(255);
        // TEV stage counts.
        setOffs(19);
        b.u8_(1); b.fill(3, 0xFF);
        // TEV stages (0x14): pass the rasterised colour/alpha through.
        setOffs(20);
        b.u8_(0xFF);
        b.u8_(GX_CC_ZERO); b.u8_(GX_CC_ZERO); b.u8_(GX_CC_ZERO); b.u8_(GX_CC_RASC);
        b.u8_(GX_TEV_ADD); b.u8_(GX_TB_ZERO); b.u8_(GX_CS_SCALE_1); b.u8_(1); b.u8_(GX_TEVPREV);
        b.u8_(GX_CA_ZERO); b.u8_(GX_CA_ZERO); b.u8_(GX_CA_ZERO); b.u8_(GX_CA_RASA);
        b.u8_(GX_TEV_ADD); b.u8_(GX_TB_ZERO); b.u8_(GX_CS_SCALE_1); b.u8_(1); b.u8_(GX_TEVPREV);
        b.u8_(0xFF);
        // Alpha compare (8).
        setOffs(24);
        b.u8_(GX_ALWAYS); b.u8_(0); b.u8_(GX_AOP_OR); b.u8_(GX_ALWAYS); b.u8_(0); b.fill(3, 0xFF);
        // Blend (4).
        setOffs(25);
        b.u8_(GX_BM_NONE); b.u8_(GX_BL_SRCALPHA); b.u8_(GX_BL_INVSRCALPHA); b.u8_(GX_LO_COPY);
        // Z mode (4).
        setOffs(26);
        b.u8_(1); b.u8_(GX_LEQUAL); b.u8_(1); b.u8_(0xFF);
        // Z comp loc.
        setOffs(27);
        b.u8_(1); b.fill(3, 0xFF);
        // Dither.
        setOffs(28);
        b.u8_(0); b.fill(3, 0xFF);
        closeChunk(b, sp);
    }

    b.patch32(8, static_cast<u32>(b.size()));
    return b.v;
}

// Two triangles (a quad) with distinct vertex colours, one joint.
SynthModel quadModel() {
    SynthModel m;
    m.positions = {-1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f};
    m.colors = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.joints.emplace_back();
    return m;
}

// Key-track blob helpers: 6-byte tracks {count, index, tangentType}.
void track(Blob& b, u16 count, u16 index, u16 tangent = 0) {
    b.u16_(count);
    b.u16_(index);
    b.u16_(tangent);
}

// J3D1btk1 with one entry (material `name`, slot 0): scale (2, 1), rotation 0,
// translation X ramps 0 -> 1 over `duration` frames (two keys), Y = 0.
std::vector<u8> makeSyntheticBtk(const std::string& name, u16 duration, u8 loopMode) {
    Blob b;
    b.str("J3D1btk1");
    b.u32_(0);
    b.u32_(1);
    b.fill(16, 0xFF);
    const size_t blk = b.size();
    b.str("TTK1");
    b.u32_(0);
    b.u8_(loopMode);
    b.u8_(0);                    // rotation decimal shift
    b.u16_(duration);
    b.u16_(3);                   // track count (entries * 3)
    b.u16_(2);                   // scale values
    b.u16_(1);                   // rotation values
    b.u16_(7);                   // translation values
    const size_t offs = b.size();  // 0x14: table, remap, names, texMtxIdx, center, S, R, T
    for (int i = 0; i < 8; ++i) b.u32_(0);
    // 0x34..0x5C: post-tex-mtx tables (unused) + Maya flag at 0x5C.
    while (b.size() - blk < 0x5C) b.u32_(0);
    b.u32_(0);                   // maya = 0
    auto setOffs = [&](int slot) { b.patch32(offs + static_cast<size_t>(slot) * 4, static_cast<u32>(b.size() - blk)); };
    // Track table: S, T, Q × {scale, rotation, translation}.
    setOffs(0);
    track(b, 1, 0); track(b, 1, 0); track(b, 2, 0);   // S: scale[0]=2, rot[0]=0, trans keys 0..5
    track(b, 1, 1); track(b, 1, 0); track(b, 1, 6);   // T: scale[1]=1, rot 0, trans[6]=0
    track(b, 1, 1); track(b, 1, 0); track(b, 1, 6);   // Q
    b.align(4, 0);
    setOffs(1);                  // remap
    b.u16_(0);
    b.align(4, 0);
    setOffs(2);                  // names
    b.bytes(nameTable({name}));
    b.align(4, 0);
    setOffs(3);                  // tex mtx slot per entry
    b.u8_(0);
    b.align(4, 0);
    setOffs(4);                  // centres
    b.f32_(0.5f); b.f32_(0.5f); b.f32_(0.5f);
    setOffs(5);                  // scale values
    b.f32_(2.0f); b.f32_(1.0f);
    setOffs(6);                  // rotation values
    b.s16_(0);
    b.align(4, 0);
    setOffs(7);                  // translation values: key0 (0, 0, 0) key1 (duration, 1, 0), then 0
    b.f32_(0.0f); b.f32_(0.0f); b.f32_(0.0f);
    b.f32_(static_cast<f32>(duration)); b.f32_(1.0f); b.f32_(0.0f);
    b.f32_(0.0f);
    b.align(0x20, 0);
    b.patch32(blk + 4, static_cast<u32>(b.size() - blk));
    b.patch32(8, static_cast<u32>(b.size()));
    return b.v;
}

// J3D1bck1 with `jointCount` joints: joint 0 rotates about Y from 0 to
// 0x2000 << 1 (= 90 degrees) over `duration` frames; the other joints are
// static with a (0, 0, 10) translation (a BCK replaces the JNT1 pose).
std::vector<u8> makeSyntheticBck(u16 jointCount, u16 duration, u8 loopMode) {
    Blob b;
    b.str("J3D1bck1");
    b.u32_(0);
    b.u32_(1);
    b.fill(16, 0xFF);
    const size_t blk = b.size();
    b.str("ANK1");
    b.u32_(0);
    b.u8_(loopMode);
    b.u8_(1);                    // rotation decimal shift
    b.u16_(duration);
    b.u16_(jointCount);
    b.u16_(1);                   // scale values
    b.u16_(7);                   // rotation values
    b.u16_(2);                   // translation values (0, 10)
    const size_t offs = b.size();  // 0x14: table, S, R, T
    for (int i = 0; i < 4; ++i) b.u32_(0);
    auto setOffs = [&](int slot) { b.patch32(offs + static_cast<size_t>(slot) * 4, static_cast<u32>(b.size() - blk)); };
    setOffs(0);
    for (u16 j = 0; j < jointCount; ++j) {
        for (int k = 0; k < 3; ++k) {
            track(b, 1, 0);                                   // scale = 1
            if (j == 0 && k == 1) track(b, 2, 0); else track(b, 1, 6);  // rotation
            if (j > 0 && k == 2) track(b, 1, 1); else track(b, 1, 0);   // child: Z = 10
        }
    }
    b.align(4, 0);
    setOffs(1);
    b.f32_(1.0f);
    setOffs(2);
    b.s16_(0); b.s16_(0); b.s16_(0);
    b.s16_(static_cast<s16>(duration)); b.s16_(0x2000); b.s16_(0);
    b.s16_(0);
    b.align(4, 0);
    setOffs(3);
    b.f32_(0.0f);
    b.f32_(10.0f);
    b.align(0x20, 0);
    b.patch32(blk + 4, static_cast<u32>(b.size() - blk));
    b.patch32(8, static_cast<u32>(b.size()));
    return b.v;
}

// ---------------------------------------------------------------------------
// Synthetic RARC (root dir + flat files) for the TitleSky mount test — same
// layout as jkr_archive_test.cpp's builder.
// ---------------------------------------------------------------------------
u16 arcHash(const std::string& name) {
    u16 hash = 0;
    for (char c : name) {
        char lc = c;
        if (lc >= 'A' && lc <= 'Z') lc = static_cast<char>(lc - 'A' + 'a');
        hash = static_cast<u16>(static_cast<u16>(lc) + hash * 3);
    }
    return hash;
}

std::vector<u8> buildRarc(const std::vector<std::pair<std::string, std::vector<u8>>>& files) {
    Blob strings;
    strings.str(".");
    strings.u8_(0);
    std::vector<u32> nameOffs;
    for (const auto& f : files) {
        nameOffs.push_back(static_cast<u32>(strings.size()));
        strings.str(f.first.c_str());
        strings.u8_(0);
    }
    const u32 nrFiles = static_cast<u32>(files.size());
    const u32 dirOffset = 0x20;
    const u32 fileOffset = dirOffset + 0x10;
    const u32 stringTableOffset = fileOffset + nrFiles * 0x14;
    const u32 stringTableSize = static_cast<u32>(strings.size());
    const u32 tablesEnd = 0x20 + 0x20 + stringTableOffset + stringTableSize;
    const u32 fileDataAbs = (tablesEnd + 0x1F) & ~0x1Fu;

    std::vector<u32> dataOffs;
    u32 total = 0;
    for (const auto& f : files) {
        dataOffs.push_back(total);
        total = (total + static_cast<u32>(f.second.size()) + 0x1F) & ~0x1Fu;
    }

    Blob b;
    b.str("RARC");
    b.u32_(0);                        // file size (patched)
    b.u32_(0x20);                     // header size
    b.u32_(fileDataAbs - 0x20);       // file data offset (header-relative)
    b.u32_(total);
    b.u32_(total);
    b.u32_(0);
    b.u32_(0);
    b.u32_(1);                        // dirs
    b.u32_(dirOffset);
    b.u32_(nrFiles);
    b.u32_(fileOffset);
    b.u32_(stringTableSize);
    b.u32_(stringTableOffset);
    b.u16_(static_cast<u16>(nrFiles));
    b.u16_(0);
    b.u32_(0);
    b.u32_(0x524F4F54);               // 'ROOT'
    b.u32_(0);
    b.u16_(0);
    b.u16_(static_cast<u16>(nrFiles));
    b.u32_(0);
    for (u32 i = 0; i < nrFiles; ++i) {
        b.u16_(static_cast<u16>(i));
        b.u16_(arcHash(files[i].first));
        b.u8_(0x11);                  // file | mram
        b.u8_(static_cast<u8>((nameOffs[i] >> 16) & 0xFF));
        b.u16_(static_cast<u16>(nameOffs[i] & 0xFFFF));
        b.u32_(dataOffs[i]);
        b.u32_(static_cast<u32>(files[i].second.size()));
        b.u32_(0);
    }
    b.bytes(strings.v);
    while (b.size() < fileDataAbs) b.u8_(0);
    for (u32 i = 0; i < nrFiles; ++i) {
        while (b.size() < fileDataAbs + dataOffs[i]) b.u8_(0);
        b.bytes(files[i].second);
    }
    b.align(0x20, 0);
    b.patch32(4, static_cast<u32>(b.size()));
    return b.v;
}

JKRHeap* ensureHeap() {
    if (JKRHeap::sRootHeap == nullptr) {
        JKRExpHeap::createRoot(1, true);
    }
    JKRHeap::sRootHeap->becomeCurrentHeap();
    if (JKRHeap::sRootHeap->getMaxAllocatableSize(0x20) < 0x10000 && JKRHeap::sSystemHeap != nullptr) {
        JKRHeap::sSystemHeap->becomeCurrentHeap();
        return JKRHeap::sSystemHeap;
    }
    return JKRHeap::sRootHeap;
}

}  // namespace

// ---------------------------------------------------------------------------
// Matrix helpers.
// ---------------------------------------------------------------------------
TEST_CASE(j3d_mtx_concat_inverse_roundtrip) {
    Mtx a, b, ab, inv, id;
    mtxRotAxisRad(a, 0.3f, 1.0f, 0.2f, 0.7f);
    a[0][3] = 10.0f; a[1][3] = -4.0f; a[2][3] = 2.5f;
    mtxApplyScale(a, a, 2.0f, 0.5f, 1.5f);
    mtxRotAxisRad(b, 1.0f, 0.0f, 0.0f, -1.1f);
    b[2][3] = 100.0f;
    mtxConcat(a, b, ab);
    REQUIRE(mtxInverse(ab, inv));
    mtxConcat(ab, inv, id);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK_NEAR(id[r][c], (r == c) ? 1.0f : 0.0f, 1e-4f);
        }
    }
    // Alias-safe concat: a = a * b must equal ab.
    Mtx a2;
    mtxCopy(a, a2);
    mtxConcat(a2, b, a2);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK_NEAR(a2[r][c], ab[r][c], 1e-5f);
        }
    }
    // Singular matrix -> false + identity.
    Mtx z = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    CHECK(!mtxInverse(z, inv));
    CHECK_NEAR(inv[1][1], 1.0f, 1e-6f);
}

TEST_CASE(j3d_mtx_lookat_title_camera) {
    // FileSelectCameraController::exeTitle camera: the target must land on the
    // view axis (x = y = 0, z < 0), the up vector must stay upwards.
    const f32 eye[3] = {0.0f, 15000.0f, 15000.0f};
    const f32 target[3] = {0.0f, 15800.0f, 0.0f};
    const f32 up[3] = {0.0f, 1.0f, 0.0f};
    Mtx view;
    mtxLookAt(view, eye, up, target);
    f32 out[3];
    mtxMultVec(view, target, out);
    CHECK_NEAR(out[0], 0.0f, 1e-2f);
    CHECK_NEAR(out[1], 0.0f, 1e-1f);
    CHECK(out[2] < -15000.0f);
    // Eye maps to the origin.
    mtxMultVec(view, eye, out);
    CHECK_NEAR(out[0], 0.0f, 1e-2f);
    CHECK_NEAR(out[1], 0.0f, 1e-2f);
    CHECK_NEAR(out[2], 0.0f, 1e-2f);
    // Rows are orthonormal (right, camUp, look).
    CHECK_NEAR(view[0][0], 1.0f, 1e-5f);
    CHECK_NEAR(view[1][1] * view[1][1] + view[1][2] * view[1][2], 1.0f, 1e-5f);
    CHECK(view[1][1] > 0.99f);   // camera up ~ +Y
    // Translation removed -> rotation only (what the sky uses).
    mtxZeroTranslation(view);
    CHECK_NEAR(view[0][3], 0.0f, 1e-9f);
    CHECK_NEAR(view[2][3], 0.0f, 1e-9f);
}

TEST_CASE(j3d_mtx_proj_concat_and_normal) {
    Mtx a;
    mtxRotAxisRad(a, 0.0f, 0.0f, 1.0f, 0.5f);
    a[0][3] = 3.0f;
    Mtx44 id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    Mtx out;
    mtxProjConcat(a, id, out);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            CHECK_NEAR(out[r][c], a[r][c], 1e-6f);
        }
    }
    // With a translation on the right: dst col 3 = a * (tx, ty, tz, 1).
    Mtx44 t = {{1, 0, 0, 2}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    mtxProjConcat(a, t, out);
    CHECK_NEAR(out[0][3], a[0][0] * 2.0f + a[0][3], 1e-5f);
    CHECK_NEAR(out[1][3], a[1][0] * 2.0f + a[1][3], 1e-5f);
    // Normal matrix of a uniformly scaled rotation = rotation / s, no translation.
    Mtx s;
    mtxApplyScale(a, s, 2.0f, 2.0f, 2.0f);
    Mtx n;
    mtxNormalMtx(s, n);
    CHECK_NEAR(n[0][0], a[0][0] / 2.0f, 1e-5f);
    CHECK_NEAR(n[1][0], a[1][0] / 2.0f, 1e-5f);
    CHECK_NEAR(n[0][3], 0.0f, 1e-9f);
}

TEST_CASE(j3d_translate_rotate_and_texture_mtx) {
    // J3DGetTranslateRotateMtx: rotation Y = 90 degrees (0x4000) + translation.
    JointTransform t;
    t.rotation[1] = 0x4000;
    t.translation[0] = 1.0f; t.translation[1] = 2.0f; t.translation[2] = 3.0f;
    Mtx m;
    getTranslateRotateMtx(t, m);
    CHECK_NEAR(m[0][0], 0.0f, 1e-5f);
    CHECK_NEAR(m[0][2], 1.0f, 1e-5f);
    CHECK_NEAR(m[2][0], -1.0f, 1e-5f);
    CHECK_NEAR(m[2][2], 0.0f, 1e-5f);
    CHECK_NEAR(m[1][1], 1.0f, 1e-6f);
    CHECK_NEAR(m[0][3], 1.0f, 1e-9f);
    CHECK_NEAR(m[1][3], 2.0f, 1e-9f);
    CHECK_NEAR(m[2][3], 3.0f, 1e-9f);
    CHECK_NEAR(sinShort(0x4000), 1.0f, 1e-6f);
    CHECK_NEAR(cosShort(static_cast<s16>(0x8000)), -1.0f, 1e-6f);

    // J3DGetTextureMtx: scale (2, 1), translation (0.25, 0), centre 0.5.
    TexSrt srt;
    srt.scaleX = 2.0f;
    srt.transX = 0.25f;
    const f32 center[3] = {0.5f, 0.5f, 0.5f};
    Mtx tm;
    getTextureMtx(srt, center, tm);
    CHECK_NEAR(tm[0][0], 2.0f, 1e-6f);
    CHECK_NEAR(tm[0][1], 0.0f, 1e-6f);
    CHECK_NEAR(tm[0][2], -0.25f, 1e-6f);   // translation lives in column 2
    CHECK_NEAR(tm[0][3], 0.0f, 1e-9f);
    CHECK_NEAR(tm[1][1], 1.0f, 1e-6f);
    CHECK_NEAR(tm[1][2], 0.0f, 1e-6f);
    CHECK_NEAR(tm[2][2], 1.0f, 1e-9f);
    // The "Old" variant keeps the same values in column 3.
    getTextureMtxOld(srt, center, tm);
    CHECK_NEAR(tm[0][3], -0.25f, 1e-6f);
    CHECK_NEAR(tm[0][2], 0.0f, 1e-9f);
    // A 90-degree rotation about the centre maps (0.5, 0.5) onto itself.
    srt = TexSrt();
    srt.rotation = 0x4000;
    getTextureMtx(srt, center, tm);
    const f32 x = tm[0][0] * 0.5f + tm[0][1] * 0.5f + tm[0][2];
    const f32 y = tm[1][0] * 0.5f + tm[1][1] * 0.5f + tm[1][2];
    CHECK_NEAR(x, 0.5f, 1e-5f);
    CHECK_NEAR(y, 0.5f, 1e-5f);
}

// ---------------------------------------------------------------------------
// Keyframes + frame control.
// ---------------------------------------------------------------------------
TEST_CASE(j3d_hermite_and_key_tracks) {
    // Zero tangents: the classic smoothstep between the keys.
    CHECK_NEAR(hermite(0.0f, 0.0f, 0.0f, 0.0f, 10.0f, 1.0f, 0.0f), 0.0f, 1e-6f);
    CHECK_NEAR(hermite(5.0f, 0.0f, 0.0f, 0.0f, 10.0f, 1.0f, 0.0f), 0.5f, 1e-6f);
    CHECK_NEAR(hermite(10.0f, 0.0f, 0.0f, 0.0f, 10.0f, 1.0f, 0.0f), 1.0f, 1e-6f);
    // Matching linear tangents (slope 0.1/frame) give a straight line.
    CHECK_NEAR(hermite(2.5f, 0.0f, 0.0f, 0.1f, 10.0f, 1.0f, 0.1f), 0.25f, 1e-5f);

    // Track with three keys (t, v, tangent) and one-tangent layout.
    const f32 data[] = {/*unused*/ 9.0f, 9.0f,
                        0.0f, 0.0f, 0.0f,  10.0f, 1.0f, 0.0f,  20.0f, 3.0f, 0.0f};
    KeyTrack tr;
    tr.count = 3;
    tr.index = 2;
    tr.tangentType = 0;
    const size_t n = sizeof(data) / sizeof(data[0]);
    CHECK_NEAR(evalTrackF32(tr, data, n, -5.0f, 7.0f), 0.0f, 1e-6f);   // clamps to the first key
    CHECK_NEAR(evalTrackF32(tr, data, n, 5.0f, 7.0f), 0.5f, 1e-6f);
    CHECK_NEAR(evalTrackF32(tr, data, n, 15.0f, 7.0f), 2.0f, 1e-6f);
    CHECK_NEAR(evalTrackF32(tr, data, n, 25.0f, 7.0f), 3.0f, 1e-6f);   // clamps to the last key
    // Single-value track: the value itself; empty track: the default.
    KeyTrack one;
    one.count = 1;
    one.index = 1;
    CHECK_NEAR(evalTrackF32(one, data, n, 3.0f, 7.0f), 9.0f, 1e-9f);
    KeyTrack none;
    CHECK_NEAR(evalTrackF32(none, data, n, 3.0f, 7.0f), 7.0f, 1e-9f);
    // Out-of-range track is rejected (default) instead of read past the table.
    KeyTrack bad;
    bad.count = 4;
    bad.index = 2;
    CHECK_NEAR(evalTrackF32(bad, data, n, 3.0f, 7.0f), 7.0f, 1e-9f);
    // In/out tangent layout (stride 4).
    const s16 rot[] = {0, 0, 0, 0,  20, 0x2000, 0, 0};
    KeyTrack rt;
    rt.count = 2;
    rt.tangentType = 1;
    CHECK_NEAR(evalTrackS16(rt, rot, 8, 10.0f), 4096.0f, 1e-3f);
}

TEST_CASE(j3d_frame_ctrl_loop_modes) {
    FrameCtrl fc;
    fc.init(2, 10.0f);          // repeat
    for (int i = 0; i < 12; ++i) fc.update();
    CHECK_NEAR(fc.frame, 2.0f, 1e-5f);
    fc.init(0, 10.0f);          // once: stops just before the end
    for (int i = 0; i < 30; ++i) fc.update();
    CHECK(fc.frame < 10.0f);
    CHECK(fc.frame > 9.9f);
    CHECK_NEAR(fc.rate, 0.0f, 1e-9f);
    fc.init(1, 10.0f);          // once and reset
    for (int i = 0; i < 12; ++i) fc.update();
    CHECK_NEAR(fc.frame, 0.0f, 1e-9f);
    fc.init(4, 10.0f);          // mirror repeat: bounces at end - 1 (frame 9)
    for (int i = 0; i < 9; ++i) fc.update();
    CHECK_NEAR(fc.frame, 9.0f, 1e-5f);
    CHECK(fc.rate < 0.0f);
    for (int i = 0; i < 6; ++i) fc.update();
    CHECK_NEAR(fc.frame, 3.0f, 1e-5f);
    for (int i = 0; i < 4; ++i) fc.update();   // ... reaches 0 and bounces back up
    CHECK(fc.frame >= 0.0f);
    for (int i = 0; i < 3; ++i) fc.update();
    CHECK(fc.rate > 0.0f);
}

// ---------------------------------------------------------------------------
// BMD parser.
// ---------------------------------------------------------------------------
TEST_CASE(j3d_bmd_parse_synthetic) {
    SynthModel sm = quadModel();
    SynthJoint child;
    child.t.translation[1] = 10.0f;
    child.t.rotation[1] = 0x4000;
    sm.joints.push_back(child);
    const std::vector<u8> bytes = makeSyntheticBmd(sm);

    BmdModel model;
    std::string err;
    REQUIRE(model.load(bytes.data(), bytes.size(), &err));
    CHECK_EQ(model.joints.size(), static_cast<size_t>(2));
    CHECK_EQ(model.shapes.size(), static_cast<size_t>(1));
    CHECK_EQ(model.materials.size(), static_cast<size_t>(1));
    CHECK_EQ(model.drawItems.size(), static_cast<size_t>(1));
    CHECK_EQ(model.textures.size(), static_cast<size_t>(0));
    CHECK_EQ(model.rootJoint, 0);
    REQUIRE(model.joints.size() == 2);
    CHECK_EQ(model.joints[0].parent, static_cast<s16>(-1));
    CHECK_EQ(model.joints[1].parent, static_cast<s16>(0));
    CHECK(model.joints[1].name == "joint1");
    CHECK_NEAR(model.joints[1].transform.translation[1], 10.0f, 1e-9f);
    CHECK_EQ(model.joints[1].transform.rotation[1], static_cast<s16>(0x4000));
    CHECK_EQ(model.jointOrder.size(), static_cast<size_t>(2));

    // Vertex arrays: host-endian floats + RGBA8 colours.
    const BmdVertexArray* pos = model.array(GX_VA_POS);
    REQUIRE(pos != nullptr);
    CHECK_EQ(static_cast<int>(pos->stride), 12);
    CHECK(pos->elementCount >= 4);
    f32 x1;
    std::memcpy(&x1, pos->data.data() + 12, 4);
    CHECK_NEAR(x1, 1.0f, 1e-9f);
    const BmdVertexArray* clr = model.array(GX_VA_CLR0);
    REQUIRE(clr != nullptr);
    CHECK_EQ(static_cast<int>(clr->stride), 4);
    CHECK_EQ(static_cast<int>(clr->data[4 * 1 + 1]), 255);   // vertex 1 is green

    // Shape: one group, INDEX16 pos + clr0, single matrix -> joint 1.
    const BmdShape& sh = model.shapes[0];
    CHECK_EQ(static_cast<int>(sh.mtxType), 0);
    CHECK(!sh.hasMatrixIndexAttr);
    REQUIRE(sh.decls.size() == 2);
    CHECK_EQ(sh.decls[0].attr, static_cast<u32>(GX_VA_POS));
    CHECK_EQ(sh.decls[0].type, static_cast<u32>(GX_INDEX16));
    CHECK_EQ(sh.decls[1].attr, static_cast<u32>(GX_VA_CLR0));
    REQUIRE(sh.groups.size() == 1);
    CHECK(sh.groups[0].dl != nullptr);
    CHECK_EQ(sh.groups[0].dlSize, 32u);
    REQUIRE(sh.groups[0].mtxTable.size() == 1);
    CHECK_EQ(static_cast<int>(sh.groups[0].mtxTable[0]), 1);
    REQUIRE(model.drawMatrices.size() == 2);
    CHECK(!model.drawMatrices[1].weighted);
    CHECK_EQ(static_cast<int>(model.drawMatrices[1].index), 1);

    // Material: names, mode, cull, channel, TEV pass-through, defaults for
    // the unset tables.
    const BmdMaterial& mat = model.materials[0];
    CHECK(mat.name == "SkyMat");
    CHECK_EQ(model.findMaterial("SkyMat"), 0);
    CHECK_EQ(model.findMaterial("nope"), -1);
    CHECK_EQ(static_cast<int>(mat.mode), 1);
    CHECK_EQ(mat.cullMode, static_cast<u32>(GX_CULL_NONE));
    CHECK_EQ(static_cast<int>(mat.chanNum), 1);
    CHECK_EQ(static_cast<int>(mat.chan[0].enable), 0);
    CHECK_EQ(static_cast<int>(mat.chan[0].matSrc), static_cast<int>(GX_SRC_VTX));
    CHECK_EQ(static_cast<int>(mat.texGenNum), 0);
    CHECK_EQ(static_cast<int>(mat.tevStageNum), 1);
    CHECK_EQ(static_cast<int>(mat.tevOrder[0].colorChan), static_cast<int>(GX_COLOR0A0));
    CHECK_EQ(static_cast<int>(mat.tevOrder[0].texMap), 0xFF);
    CHECK_EQ(static_cast<int>(mat.tevStage[0].colorIn[3]), static_cast<int>(GX_CC_RASC));
    CHECK_EQ(static_cast<int>(mat.tevStage[0].alphaIn[3]), static_cast<int>(GX_CA_RASA));
    CHECK_EQ(static_cast<int>(mat.kColorSel[0]), 0x0C);
    CHECK_EQ(static_cast<int>(mat.kColorSel[5]), 0x0C);   // unset -> KCSEL_1
    CHECK_EQ(static_cast<int>(mat.kAlphaSel[5]), 0x1C);   // unset -> KASEL_1
    CHECK_EQ(static_cast<int>(mat.zMode[1]), static_cast<int>(GX_LEQUAL));
    CHECK_EQ(static_cast<int>(mat.blend[0]), static_cast<int>(GX_BM_NONE));
    CHECK_EQ(static_cast<int>(mat.alphaComp[0]), static_cast<int>(GX_ALWAYS));
    CHECK_EQ(static_cast<int>(mat.fog.type), 0);
    CHECK(!mat.texMtx[0].valid);
    CHECK_EQ(mat.texNo[0], -1);
    CHECK_EQ(mat.joint, 1);
    CHECK_EQ(static_cast<int>(model.drawItems[0].joint), 1);
    CHECK_EQ(static_cast<int>(model.drawItems[0].material), 0);
    CHECK_EQ(static_cast<int>(model.drawItems[0].shape), 0);
}

TEST_CASE(j3d_bmd_rejects_malformed) {
    BmdModel model;
    std::string err;
    CHECK(!model.load(nullptr, 0, &err));
    const std::vector<u8> garbage(0x100, 0xAB);
    CHECK(!model.load(garbage.data(), garbage.size(), &err));
    CHECK(!err.empty());
    // Valid magic but a block that runs past the end.
    std::vector<u8> bytes = makeSyntheticBmd(quadModel());
    std::vector<u8> truncated(bytes.begin(), bytes.begin() + 0x60);
    truncated[8] = 0; truncated[9] = 0; truncated[10] = 0; truncated[11] = 0x60;
    CHECK(!model.load(truncated.data(), truncated.size(), &err));
    // A bmd with only INF1 is rejected (VTX1/SHP1/MAT3 required).
    std::vector<u8> inf1Only(bytes.begin(), bytes.begin() + 0x20 + 0x40);
    inf1Only[12] = 0; inf1Only[13] = 0; inf1Only[14] = 0; inf1Only[15] = 1;
    inf1Only[8] = 0; inf1Only[9] = 0; inf1Only[10] = 0; inf1Only[11] = 0x60;
    CHECK(!model.load(inf1Only.data(), inf1Only.size(), &err));
    // A bad file must not poison a later good load.
    REQUIRE(model.load(bytes.data(), bytes.size(), &err));
    CHECK_EQ(model.shapes.size(), static_cast<size_t>(1));
}

// ---------------------------------------------------------------------------
// Renderer: joints + headless draw through the compat GX display-list path.
// ---------------------------------------------------------------------------
TEST_CASE(j3d_renderer_draw_captures_packet_vertices) {
    GXInit(nullptr, 0);
    SynthModel sm = quadModel();
    SynthJoint child;
    child.t.translation[1] = 10.0f;
    child.t.rotation[1] = 0x4000;
    sm.joints.push_back(child);

    BmdRenderer r;
    std::string err;
    REQUIRE(r.init(makeSyntheticBmd(sm), &err));
    CHECK(r.loaded());

    Mtx view;
    mtxIdentity(view);
    r.draw(view);
    CHECK_EQ(r.lastDrawnShapes(), 1u);

    // Joint 1 = joint 0 (identity) * T(0,10,0) * RotY(90).
    const Mtx* j1 = r.jointMtx(1);
    REQUIRE(j1 != nullptr);
    CHECK_NEAR((*j1)[1][3], 10.0f, 1e-5f);
    CHECK_NEAR((*j1)[0][2], 1.0f, 1e-5f);
    CHECK_NEAR((*j1)[2][0], -1.0f, 1e-5f);
    CHECK(r.jointMtx(2) == nullptr);

    // The packet DL replayed: 6 vertices, pos(3) + clr0(4) floats each, in
    // VCD order, fetched through the arrays registered by the renderer.
    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK_EQ(count, 6);
    CHECK_EQ(stride, 7);
    // v1 = position 1 (1,-1,0), green.
    CHECK_NEAR(data[7 + 0], 1.0f, 1e-6f);
    CHECK_NEAR(data[7 + 1], -1.0f, 1e-6f);
    CHECK_NEAR(data[7 + 4], 1.0f, 1e-4f);
    CHECK_NEAR(data[7 + 3], 0.0f, 1e-4f);
    // v5 = position 3 (-1,1,0), yellow.
    CHECK_NEAR(data[35 + 0], -1.0f, 1e-6f);
    CHECK_NEAR(data[35 + 1], 1.0f, 1e-6f);
    CHECK_NEAR(data[35 + 3], 1.0f, 1e-4f);
    CHECK_NEAR(data[35 + 4], 1.0f, 1e-4f);
    CHECK_NEAR(data[35 + 5], 0.0f, 1e-4f);

    // Material state reached the PE mirror (cull none, z on, no blend).
    GxPeDebugState pe;
    GXCompatDebugPeState(pe);
    CHECK_EQ(pe.zTest, 1);
    CHECK_EQ(pe.zWrite, 1);
    CHECK_EQ(pe.blendMode, static_cast<int>(GX_BM_NONE));
    CHECK_EQ(pe.cullMode, static_cast<int>(GX_CULL_NONE));
    CHECK_EQ(r.cullModeOverride(), -1);

    // MAT3 cull mode is honoured by default and replaced by the override.
    {
        SynthModel back = quadModel();
        back.cullMode = GX_CULL_BACK;
        BmdRenderer rb;
        REQUIRE(rb.init(makeSyntheticBmd(back), &err));
        rb.draw(view);
        GXCompatDebugPeState(pe);
        CHECK_EQ(pe.cullMode, static_cast<int>(GX_CULL_BACK));
        rb.setCullModeOverride(GX_CULL_NONE);
        rb.draw(view);
        GXCompatDebugPeState(pe);
        CHECK_EQ(pe.cullMode, static_cast<int>(GX_CULL_NONE));
        rb.setCullModeOverride(-1);
        rb.draw(view);
        GXCompatDebugPeState(pe);
        CHECK_EQ(pe.cullMode, static_cast<int>(GX_CULL_BACK));
    }

    // Base scale + base matrix apply under the root (LiveActor scale 0.8 and
    // the sky's rotation).
    r.setBaseScale(0.8f, 0.8f, 0.8f);
    Mtx base;
    mtxRotAxisRad(base, 0.0f, 1.0f, 0.0f, kPi / 2.0f);
    r.setBaseMtx(base);
    r.draw(view);
    j1 = r.jointMtx(1);
    REQUIRE(j1 != nullptr);
    CHECK_NEAR((*j1)[1][3], 8.0f, 1e-4f);
    // RotY(90) (base) * RotY(90) (joint) = RotY(180) scaled by 0.8.
    CHECK_NEAR((*j1)[0][0], -0.8f, 1e-4f);
    CHECK_NEAR((*j1)[2][2], -0.8f, 1e-4f);
    CHECK_NEAR((*j1)[0][2], 0.0f, 1e-4f);
}

TEST_CASE(j3d_renderer_skips_multi_matrix_shapes) {
    GXInit(nullptr, 0);
    // Flip the shape's mtxType to 3 (multi-matrix) inside a parsed copy: the
    // renderer must skip it (the host DL path has no PNMTXIDX) and report 0.
    std::vector<u8> bytes = makeSyntheticBmd(quadModel());
    BmdModel probe;
    REQUIRE(probe.load(bytes.data(), bytes.size()));
    // Locate the SHP1 init entry: the first 0x28-byte entry after "SHP1"+0x2C, 0x20-aligned.
    size_t shp = 0;
    for (size_t i = 0x20; i + 4 <= bytes.size(); i += 4) {
        if (std::memcmp(bytes.data() + i, "SHP1", 4) == 0) { shp = i; break; }
    }
    REQUIRE(shp != 0);
    const u32 initOffs = (static_cast<u32>(bytes[shp + 0x0C]) << 24) | (static_cast<u32>(bytes[shp + 0x0D]) << 16) |
                         (static_cast<u32>(bytes[shp + 0x0E]) << 8) | bytes[shp + 0x0F];
    bytes[shp + initOffs] = 3;
    BmdRenderer r;
    REQUIRE(r.init(bytes));
    CHECK_EQ(static_cast<int>(r.model().shapes[0].mtxType), 3);
    Mtx view;
    mtxIdentity(view);
    r.draw(view);
    CHECK_EQ(r.lastDrawnShapes(), 0u);
}

// ---------------------------------------------------------------------------
// BTK / BCK.
// ---------------------------------------------------------------------------
// PC_PORT: the TTK1 block-size field is not trustworthy in the wild — the
// reference loader never reads it (J3DAnmKeyLoader_v15::load walks the blocks
// by the file header's block count), and SMG's CometNearOrbitSky btk carries a
// value that does not describe the buffer the arc reader hands over, which used
// to make the title-sky texture animation fail to attach ("TTK1 block size out
// of range"). A zero ("no next block") or oversized value must clamp to the
// buffer; the table bounds below the clamp keep a truncated file honest.
TEST_CASE(j3d_btk_tolerates_wild_block_size) {
    const std::vector<u8> bytes = makeSyntheticBtk("SkyMat", 10, 2);
    std::string err;
    TexSrt srt;

    // Block size 0 = "no next block". Everything else in the file is intact.
    std::vector<u8> zero(bytes);
    zero[0x24] = 0; zero[0x25] = 0; zero[0x26] = 0; zero[0x27] = 0;
    BtkAnim btkZero;
    REQUIRE(btkZero.load(zero.data(), zero.size(), &err));
    REQUIRE(btkZero.entries.size() == 1);
    CHECK(btkZero.entries[0].materialName == "SkyMat");
    CHECK_EQ(static_cast<int>(btkZero.duration), 10);
    btkZero.evaluate(0, 5.0f, srt);
    CHECK_NEAR(srt.transX, 0.5f, 1e-6f);

    // Oversized value: clamp to the buffer instead of rejecting the file.
    std::vector<u8> big(bytes);
    const u32 tooBig = static_cast<u32>(bytes.size()) + 0x100u;
    big[0x24] = static_cast<u8>(tooBig >> 24);
    big[0x25] = static_cast<u8>(tooBig >> 16);
    big[0x26] = static_cast<u8>(tooBig >> 8);
    big[0x27] = static_cast<u8>(tooBig);
    BtkAnim btkBig;
    REQUIRE(btkBig.load(big.data(), big.size(), &err));
    REQUIRE(btkBig.entries.size() == 1);
    btkBig.evaluate(0, 10.0f, srt);
    CHECK_NEAR(srt.transX, 1.0f, 1e-6f);

    // The clamp is not a licence to read past the buffer: no block size makes
    // a file that ends inside its tables parse.
    CHECK(!btkZero.load(zero.data(), 0x20 + 0x80, &err));
}
TEST_CASE(j3d_btk_parse_and_evaluate) {
    const std::vector<u8> bytes = makeSyntheticBtk("SkyMat", 10, 2);
    BtkAnim btk;
    std::string err;
    REQUIRE(btk.load(bytes.data(), bytes.size(), &err));
    CHECK_EQ(static_cast<int>(btk.loopMode), 2);
    CHECK_EQ(static_cast<int>(btk.duration), 10);
    CHECK(!btk.maya);
    REQUIRE(btk.entries.size() == 1);
    CHECK(btk.entries[0].materialName == "SkyMat");
    CHECK_EQ(static_cast<int>(btk.entries[0].texMtxSlot), 0);
    CHECK_NEAR(btk.entries[0].center[0], 0.5f, 1e-9f);
    TexSrt srt;
    btk.evaluate(0, 0.0f, srt);
    CHECK_NEAR(srt.scaleX, 2.0f, 1e-9f);
    CHECK_NEAR(srt.scaleY, 1.0f, 1e-9f);
    CHECK_EQ(static_cast<int>(srt.rotation), 0);
    CHECK_NEAR(srt.transX, 0.0f, 1e-9f);
    btk.evaluate(0, 5.0f, srt);
    CHECK_NEAR(srt.transX, 0.5f, 1e-6f);
    CHECK_NEAR(srt.transY, 0.0f, 1e-9f);
    btk.evaluate(0, 10.0f, srt);
    CHECK_NEAR(srt.transX, 1.0f, 1e-6f);
    // Out-of-range entry -> identity SRT; garbage -> rejected.
    btk.evaluate(3, 5.0f, srt);
    CHECK_NEAR(srt.scaleX, 1.0f, 1e-9f);
    std::vector<u8> bad(bytes);
    bad[0x20] = 'X';
    CHECK(!btk.load(bad.data(), bad.size(), &err));
    CHECK(!btk.load(bytes.data(), 0x30, &err));

    // Attached to the model: binds by material name and advances with the
    // repeat loop; a non-matching name still attaches (warns, no binding).
    GXInit(nullptr, 0);
    BmdRenderer r;
    REQUIRE(r.init(makeSyntheticBmd(quadModel())));
    REQUIRE(r.attachBtk(bytes, &err));
    for (int i = 0; i < 12; ++i) r.update();
    CHECK_NEAR(r.btkFrame(), 2.0f, 1e-5f);
    Mtx view;
    mtxIdentity(view);
    r.draw(view);
    CHECK_EQ(r.lastDrawnShapes(), 1u);
    CHECK(r.attachBtk(makeSyntheticBtk("Other", 10, 2), &err));
}

TEST_CASE(j3d_bck_parse_and_drive_joints) {
    const std::vector<u8> bytes = makeSyntheticBck(2, 20, 2);
    BckAnim bck;
    std::string err;
    REQUIRE(bck.load(bytes.data(), bytes.size(), &err));
    CHECK_EQ(static_cast<int>(bck.duration), 20);
    CHECK_EQ(static_cast<int>(bck.rotDecShift), 1);
    REQUIRE(bck.joints.size() == 2);
    JointTransform t;
    bck.evaluate(0, 0.0f, t);
    CHECK_EQ(static_cast<int>(t.rotation[1]), 0);
    CHECK_NEAR(t.scale[0], 1.0f, 1e-9f);
    bck.evaluate(0, 20.0f, t);
    CHECK_EQ(static_cast<int>(t.rotation[1]), 0x4000);     // 0x2000 << 1
    bck.evaluate(0, 10.0f, t);
    CHECK_EQ(static_cast<int>(t.rotation[1]), 0x2000);     // Hermite midpoint, shifted
    bck.evaluate(1, 10.0f, t);
    CHECK_EQ(static_cast<int>(t.rotation[1]), 0);
    std::vector<u8> bad(bytes);
    bad[0x24] = 'Z';
    CHECK(!bck.load(bad.data(), bad.size(), &err));

    // Driving the renderer: near frame 20 joint 0 is rotated ~90 degrees
    // about Y and joint 1 (child, BCK translation (0,0,10)) swings with it.
    // The JNT1 rest pose of the child (translation 5 on Y) is replaced by the
    // BCK, exactly like J3DAnmTransformKey does on the console.
    GXInit(nullptr, 0);
    SynthModel sm = quadModel();
    SynthJoint child;
    child.t.translation[1] = 5.0f;
    sm.joints.push_back(child);
    BmdRenderer r;
    REQUIRE(r.init(makeSyntheticBmd(sm)));
    REQUIRE(r.attachBck(bytes, &err));
    Mtx view;
    mtxIdentity(view);
    r.draw(view);
    const Mtx* j1 = r.jointMtx(1);
    REQUIRE(j1 != nullptr);
    CHECK_NEAR((*j1)[2][3], 10.0f, 1e-5f);
    CHECK_NEAR((*j1)[1][3], 0.0f, 1e-5f);   // JNT1 pose overridden by the BCK
    for (int i = 0; i < 20; ++i) r.update();
    CHECK_NEAR(r.bckFrame(), 0.0f, 1e-5f);   // repeat: wrapped back to 0
    for (int i = 0; i < 20; ++i) r.update();
    // frame 20 wraps to 0 under repeat, so step to 19 explicitly.
    for (int i = 0; i < 19; ++i) r.update();
    CHECK_NEAR(r.bckFrame(), 19.0f, 1e-5f);
    r.draw(view);
    const Mtx* j0 = r.jointMtx(0);
    REQUIRE(j0 != nullptr);
    // Rotation close to 90 degrees: the child's +Z offset swings to +X.
    j1 = r.jointMtx(1);
    CHECK((*j1)[0][3] > 9.0f);
    CHECK(std::fabs((*j1)[2][3]) < 2.0f);
}

// ---------------------------------------------------------------------------
// TitleSky: FileSelectSky maths + the full mount/parse/draw path.
// ---------------------------------------------------------------------------
TEST_CASE(j3d_title_sky_angles_and_base_mtx) {
    // The console's exeWait feeds a float into JMACosShort's s16 argument:
    // the tilt is (almost) zero for every step.
    CHECK_NEAR(TitleSky::calcAngleX(0), 0.0f, 1e-9f);
    for (u32 step : {1u, 100u, 1500u, 3000u}) {
        CHECK(std::fabs(TitleSky::calcAngleX(step)) < 1e-4f);
    }
    // 1000 s later the truncated argument is ~500 units: still < 0.01 rad.
    CHECK(TitleSky::calcAngleX(60000) >= 0.0f);
    CHECK(TitleSky::calcAngleX(60000) < 1e-2f);
    // base = inverse(rotY(angleY) * rotX(angleX)): for angleX = 0 the inverse of
    // a Y rotation by +90 degrees is the rotation by -90 degrees.
    Mtx base;
    TitleSky::calcBaseMtx(0.0f, kPi / 2.0f, base);
    CHECK_NEAR(base[0][0], 0.0f, 1e-6f);
    CHECK_NEAR(base[0][2], -1.0f, 1e-6f);
    CHECK_NEAR(base[2][0], 1.0f, 1e-6f);
    CHECK_NEAR(base[1][1], 1.0f, 1e-6f);
    CHECK_NEAR(base[0][3], 0.0f, 1e-9f);
    // A missing archive is a clean failure (fallback backdrop path).
    TitleSky sky;
    CHECK(!sky.init("/ObjectData/DoesNotExist.arc"));
    CHECK(!sky.loaded());
    sky.update();
    sky.draw();   // no-op
}

TEST_CASE(j3d_title_sky_mounts_and_draws_synthetic_archive) {
    JKRHeap* heap = ensureHeap();
    REQUIRE(heap != nullptr);
    GXInit(nullptr, 0);

    namespace fs = std::filesystem;
    const std::string root = (fs::temp_directory_path() / "galaxy-pc-j3dsky").string();
    fs::remove_all(root);
    fs::create_directories(root + "/ObjectData");
    SynthModel sm = quadModel();
    sm.materialName = "CometNearOrbitSky_v";
    const std::vector<u8> arc = buildRarc({
        {"CometNearOrbitSky.bdl", makeSyntheticBmd(sm)},
        {"CometNearOrbitSky.btk", makeSyntheticBtk("CometNearOrbitSky_v", 30, 2)},
        {"CometNearOrbitSky.bck", makeSyntheticBck(1, 60, 2)},
    });
    {
        std::ofstream f(root + "/ObjectData/CometNearOrbitSky.arc", std::ios::binary);
        f.write(reinterpret_cast<const char*>(arc.data()), static_cast<std::streamsize>(arc.size()));
    }
    Platform::Filesystem::setRootDir(root);
    compat::initDVD();

    TitleSky sky;
    REQUIRE(sky.init("/ObjectData/CometNearOrbitSky.arc"));
    CHECK(sky.loaded());
    REQUIRE(sky.renderer() != nullptr);
    CHECK_EQ(sky.renderer()->model().materials.size(), static_cast<size_t>(1));

    // One frame: angleY advances by cAngleIncY, both animations step.
    sky.update();
    CHECK_NEAR(sky.angleY(), 0.001f, 1e-7f);
    CHECK_NEAR(sky.renderer()->btkFrame(), 1.0f, 1e-6f);
    CHECK_NEAR(sky.renderer()->bckFrame(), 1.0f, 1e-6f);
    sky.draw();
    CHECK_EQ(sky.renderer()->lastDrawnShapes(), 1u);
    int count = 0, stride = 0;
    const float* data = GXCompatDebugVertices(&count, &stride);
    REQUIRE(data != nullptr);
    CHECK_EQ(count, 6);
    CHECK_EQ(stride, 7);
    // The sky is drawn at LiveActor scale 0.8 under the (near-identity) base.
    const Mtx* j0 = sky.renderer()->jointMtx(0);
    REQUIRE(j0 != nullptr);
    CHECK_NEAR((*j0)[0][0], 0.8f, 1e-3f);
    CHECK_NEAR((*j0)[1][1], 0.8f, 1e-3f);
    CHECK_NEAR((*j0)[0][3], 0.0f, 1e-6f);
    // TitleSky overrides the MAT3 cull mode with GX_CULL_NONE (the dome is
    // opaque and drawn from the inside; the host front-face convention is not
    // verified against GX, so both faces are drawn).
    GxPeDebugState pe;
    GXCompatDebugPeState(pe);
    CHECK_EQ(pe.zTest, 1);
    CHECK_EQ(pe.cullMode, static_cast<int>(GX_CULL_NONE));
    CHECK_EQ(sky.renderer()->cullModeOverride(), static_cast<int>(GX_CULL_NONE));

    fs::remove_all(root);
}
