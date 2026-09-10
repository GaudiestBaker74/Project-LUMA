#pragma once
// =============================================================================
// compat/j3d — BMD/BDL (J3D binary model) parser for the host (M9.5.4 v8).
//
// Why: the title screen backdrop is the J3D model "CometNearOrbitSky"
// (FileSelectSky actor). The JSystem J3D graph (J3DModelLoader, J3DMaterial,
// J3DShape, J3DGD display-list writers, J3DSys…) is not ported — it is built
// around the console's GD write-gather FIFO and DMA'd matrix arrays. This
// module is a compact, self-contained reader of the same file format that
// produces plain host structures; BmdRenderer.cpp replays them through the
// compat GX API (GXSet*/GXCallDisplayList), so nothing of the J3D runtime is
// needed to draw a static/BTK-animated model.
//
// Scope (what the sky needs, kept general where it is cheap):
//   INF1 (hierarchy), VTX1 (vertex arrays -> host-endian copies, colours
//   expanded to RGBA8), DRW1 (draw matrix table), JNT1 (joints), SHP1 (shapes:
//   VCD decls + packet display lists), MAT3 v26 (the full GX material state),
//   TEX1 (BTI headers + image/palette pointers). EVP1 (skinning envelopes) and
//   MDL3 (BDL precompiled material DLs) are skipped: envelope-driven matrices
//   and PNMTXIDX (multi-matrix) shapes are reported and not drawn.
//
// Layout references: JSystem/J3DGraphLoader/J3DModelLoader.hpp block structs,
// J3DMaterialFactory.hpp (J3DMaterialInitData, 0x14C bytes) and the J3DStruct
// info structs; all offsets are big-endian.
//
// The parser keeps pointers into the caller's buffer (display lists, texture
// images, palettes): the BMD bytes must outlive the BmdModel.
// =============================================================================

#include <revolution/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "compat/gx/Bti.h"
#include "compat/j3d/J3DMathCompat.h"

namespace compat::j3d {

// --- VTX1 -------------------------------------------------------------------

/// One GXSetArray-able attribute array (host endianness).
struct BmdVertexArray {
    u32 attr = 0xFF;       // GX_VA_POS .. GX_VA_TEX7 (NBT reported as GX_VA_NRM)
    u32 compCnt = 0;       // GXCompCnt (colours: GX_CLR_RGBA after conversion)
    u32 compType = 0;      // GXCompType (colours: GX_RGBA8 after conversion)
    u8 frac = 0;
    u8 stride = 0;         // bytes per element (as passed to GXSetArray)
    u32 elementCount = 0;
    std::vector<u8> data;  // host-endian components
    bool valid() const { return attr != 0xFF && stride != 0 && !data.empty(); }
};

// --- JNT1 -------------------------------------------------------------------

struct BmdJoint {
    std::string name;
    u16 flags = 0;
    u8 scaleCompensate = 0;
    JointTransform transform;
    f32 radius = 0.0f;
    f32 bboxMin[3] = {0, 0, 0};
    f32 bboxMax[3] = {0, 0, 0};
    s16 parent = -1;       // filled from INF1
};

// --- DRW1 -------------------------------------------------------------------

struct BmdDrawMatrix {
    bool weighted = false; // false: joint index, true: EVP1 envelope index
    u16 index = 0;
};

// --- SHP1 -------------------------------------------------------------------

struct BmdVtxDecl {
    u32 attr = 0;          // GXAttr (NBT reported as GX_VA_NRM, `nbt` set)
    u32 type = 0;          // GXAttrType (GX_DIRECT / GX_INDEX8 / GX_INDEX16)
    bool nbt = false;
};

struct BmdMtxGroup {
    std::vector<u16> mtxTable;  // DRW1 indices (0xFFFF = keep previous)
    const u8* dl = nullptr;     // packet display list (big-endian GX FIFO bytes)
    u32 dlSize = 0;
};

struct BmdShape {
    u8 mtxType = 0;             // 0 single matrix, 1 billboard, 2 Y-billboard, 3 multi
    std::vector<BmdVtxDecl> decls;
    std::vector<BmdMtxGroup> groups;
    f32 radius = 0.0f;
    f32 bboxMin[3] = {0, 0, 0};
    f32 bboxMax[3] = {0, 0, 0};
    bool hasMatrixIndexAttr = false;  // PNMTXIDX/TEXnMTXIDX in the decl (not drawable here)
};

// --- MAT3 -------------------------------------------------------------------

struct BmdColorChan {
    u8 enable = 0;
    u8 matSrc = 0;         // GXColorSrc
    u8 lightMask = 0;
    u8 diffuseFn = 0;
    u8 attnFn = 2;         // GXAttnFn (GX_AF_NONE)
    u8 ambSrc = 0;
};

struct BmdTexCoord {
    u8 type = 1;           // GXTexGenType (GX_TG_MTX2x4)
    u8 src = 4;            // GXTexGenSrc (GX_TG_TEX0)
    u8 mtx = 60;           // GX_IDENTITY or GX_TEXMTX0 + 3*i
};

struct BmdTexMtx {
    bool valid = false;
    u8 projection = 1;     // GXTexMtxType: 0 = 3x4, 1 = 2x4
    u8 info = 0;           // mode = info & 0x3F, bit 7 = Maya SRT
    f32 center[3] = {0.5f, 0.5f, 0.5f};
    TexSrt srt;
    f32 effectMtx[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
};

struct BmdTevOrder {
    u8 texCoord = 0xFF;
    u8 texMap = 0xFF;
    u8 colorChan = 0xFF;
};

struct BmdTevStage {
    u8 colorIn[4] = {15, 15, 15, 15};  // GX_CC_ZERO
    u8 colorOp = 0, colorBias = 0, colorScale = 0, colorClamp = 1, colorReg = 0;
    u8 alphaIn[4] = {7, 7, 7, 7};      // GX_CA_ZERO
    u8 alphaOp = 0, alphaBias = 0, alphaScale = 0, alphaClamp = 1, alphaReg = 0;
};

struct BmdFog {
    u8 type = 0;
    u8 adjEnable = 0;
    u16 center = 0;
    f32 startZ = 0.0f, endZ = 0.0f, nearZ = 0.1f, farZ = 10000.0f;
    u8 color[4] = {0, 0, 0, 0};
    u16 adjTable[10] = {};
};

struct BmdMaterial {
    std::string name;
    u8 mode = 1;                       // 1 OPA, 4 XLU
    u32 cullMode = 2;                  // GXCullMode
    u8 matColor[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
    u8 chanNum = 0;
    BmdColorChan chan[4];              // COLOR0, ALPHA0, COLOR1, ALPHA1 (BMD order)
    u8 ambColor[2][4] = {{50, 50, 50, 50}, {50, 50, 50, 50}};
    u8 texGenNum = 0;
    BmdTexCoord texCoord[8];
    BmdTexMtx texMtx[8];
    s32 texNo[8] = {-1, -1, -1, -1, -1, -1, -1, -1};  // TEX1 index or -1
    u8 tevStageNum = 0;
    BmdTevOrder tevOrder[16];
    s16 tevColor[4][4] = {};           // GXColorS10 (TEVREG0..2 loaded, [3] kept)
    u8 kColor[4][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}};
    u8 kColorSel[16];
    u8 kAlphaSel[16];
    BmdTevStage tevStage[16];
    u8 swapMode[16][2] = {};           // rasSel, texSel
    u8 swapTable[4][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}};
    BmdFog fog;
    u8 alphaComp[5] = {7, 0, 1, 7, 0}; // comp0, ref0, op, comp1, ref1 (ALWAYS/OR/ALWAYS)
    u8 blend[4] = {0, 4, 5, 3};        // type, src, dst, logic
    u8 zMode[3] = {1, 3, 1};           // enable, func (LEQUAL), update
    u8 zCompLoc = 1;
    u8 dither = 0;
    s32 joint = 0;                     // hierarchy joint (texgen model matrix)

    BmdMaterial();
};

// --- TEX1 -------------------------------------------------------------------

struct BmdTexture {
    std::string name;
    Platform::CompatGx::BtiHeader header;
    const u8* image = nullptr;
    size_t imageBytes = 0;
    const u8* palette = nullptr;
    size_t paletteBytes = 0;
    u8 minFilter = 1, magFilter = 1;   // GXTexFilter
    u8 mipmap = 0, edgeLod = 0, biasClamp = 0, maxAnisotropy = 0;
    s16 lodBias = 0;
    u8 minLod = 0, maxLod = 0;
};

// --- INF1 -------------------------------------------------------------------

/// One drawable: shape `shape` with material `material`, attached to `joint`,
/// in hierarchy (draw) order.
struct BmdDrawItem {
    u16 material = 0;
    u16 shape = 0;
    u16 joint = 0;
};

class BmdModel {
public:
    /// Parses `data` (J3D2bmd3 / J3D2bdl4). Returns false with `error` set on
    /// malformed input. `data` must stay alive while the model is used.
    bool load(const u8* data, size_t size, std::string* error = nullptr);

    u32 loadFlags = 0;          // INF1 flags (bits 0-3: matrix calc type)
    u32 vertexCount = 0;
    std::vector<BmdVertexArray> arrays;   // indexed by GX_VA_* (size 26)
    std::vector<BmdJoint> joints;
    std::vector<BmdDrawMatrix> drawMatrices;
    std::vector<BmdShape> shapes;
    std::vector<BmdMaterial> materials;
    std::vector<BmdTexture> textures;
    std::vector<BmdDrawItem> drawItems;
    std::vector<u16> jointOrder;          // joints in INF1 (parent-before-child) order
    bool hasEnvelopes = false;
    s32 rootJoint = -1;

    const BmdVertexArray* array(u32 attr) const {
        return (attr < arrays.size() && arrays[attr].valid()) ? &arrays[attr] : nullptr;
    }
    s32 findMaterial(const char* name) const;

private:
    bool readInf1(const u8* p, size_t n, std::string* error);
    bool readVtx1(const u8* p, size_t n, std::string* error);
    bool readDrw1(const u8* p, size_t n, std::string* error);
    bool readJnt1(const u8* p, size_t n, std::string* error);
    bool readShp1(const u8* p, size_t n, std::string* error);
    bool readMat3(const u8* p, size_t n, std::string* error);
    bool readTex1(const u8* p, size_t n, std::string* error);
    std::vector<u16> mHierarchy;  // raw INF1 (type, value) pairs, resolved after all chunks
};

/// Reads a J3D ResNTAB name table at `p` (u16 count, u16 pad, {u16 hash, u16 offs}).
std::vector<std::string> readNameTable(const u8* p, size_t avail);

} // namespace compat::j3d
