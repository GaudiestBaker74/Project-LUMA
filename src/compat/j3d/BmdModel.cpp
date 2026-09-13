// =============================================================================
// compat/j3d — BMD/BDL parser (see BmdModel.h).
// =============================================================================

#include "compat/j3d/BmdModel.h"

#include <revolution/gx/GXEnum.h>

#include <cstring>

#include "platform/Log/Log.h"

namespace compat::j3d {

namespace {

inline u16 be16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}
inline u32 be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}
inline s16 beS16(const u8* p) {
    return static_cast<s16>(be16(p));
}
inline f32 beF32(const u8* p) {
    const u32 v = be32(p);
    f32 f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

bool fail(std::string* error, const char* msg) {
    if (error) {
        *error = msg;
    }
    return false;
}

/// Bounds-checked chunk reader: every offset read through it is validated
/// against the chunk size so a truncated/corrupt file fails cleanly.
struct Chunk {
    const u8* p;
    size_t n;
    bool has(size_t off, size_t len) const { return off <= n && len <= n - off; }
    u8 u8At(size_t off) const { return has(off, 1) ? p[off] : 0; }
    u16 u16At(size_t off) const { return has(off, 2) ? be16(p + off) : 0; }
    s16 s16At(size_t off) const { return has(off, 2) ? beS16(p + off) : 0; }
    u32 u32At(size_t off) const { return has(off, 4) ? be32(p + off) : 0; }
    f32 f32At(size_t off) const { return has(off, 4) ? beF32(p + off) : 0.0f; }
};

/// Byte size of one VTX1 component for a GX comp type.
u32 compTypeBytes(u32 type) {
    switch (type) {
    case GX_U8:
    case GX_S8: return 1;
    case GX_U16:
    case GX_S16: return 2;
    case GX_F32:
    default: return 4;
    }
}

/// Component count of a VTX1 attribute (positions/normals/texcoords).
u32 attrCompCount(u32 attr, u32 compCnt) {
    if (attr == GX_VA_POS) {
        return compCnt == GX_POS_XY ? 2 : 3;
    }
    if (attr == GX_VA_NRM) {
        return 3;
    }
    if (attr == GX_VA_NBT) {
        return 9;
    }
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
        return compCnt == GX_TEX_S ? 1 : 2;
    }
    return 0;
}

/// Byte size of one colour element for a GX colour comp type.
u32 colorTypeBytes(u32 type) {
    switch (type) {
    case GX_RGB565: return 2;
    case GX_RGB8: return 3;
    case GX_RGBX8: return 4;
    case GX_RGBA4: return 2;
    case GX_RGBA6: return 3;
    case GX_RGBA8:
    default: return 4;
    }
}

/// Expands one colour element to RGBA8 (host order r,g,b,a).
void colorToRgba8(const u8* src, u32 type, u8 out[4]) {
    switch (type) {
    case GX_RGB565: {
        const u16 v = be16(src);
        const u32 r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
        out[0] = static_cast<u8>((r << 3) | (r >> 2));
        out[1] = static_cast<u8>((g << 2) | (g >> 4));
        out[2] = static_cast<u8>((b << 3) | (b >> 2));
        out[3] = 255;
        break;
    }
    case GX_RGB8:
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        out[3] = 255;
        break;
    case GX_RGBX8:
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        out[3] = 255;
        break;
    case GX_RGBA4: {
        const u16 v = be16(src);
        const u32 r = (v >> 12) & 0xF, g = (v >> 8) & 0xF, b = (v >> 4) & 0xF, a = v & 0xF;
        out[0] = static_cast<u8>(r * 17);
        out[1] = static_cast<u8>(g * 17);
        out[2] = static_cast<u8>(b * 17);
        out[3] = static_cast<u8>(a * 17);
        break;
    }
    case GX_RGBA6: {
        const u32 v = (static_cast<u32>(src[0]) << 16) | (static_cast<u32>(src[1]) << 8) | src[2];
        const u32 r = (v >> 18) & 0x3F, g = (v >> 12) & 0x3F, b = (v >> 6) & 0x3F, a = v & 0x3F;
        out[0] = static_cast<u8>((r << 2) | (r >> 4));
        out[1] = static_cast<u8>((g << 2) | (g >> 4));
        out[2] = static_cast<u8>((b << 2) | (b >> 4));
        out[3] = static_cast<u8>((a << 2) | (a >> 4));
        break;
    }
    case GX_RGBA8:
    default:
        out[0] = src[0];
        out[1] = src[1];
        out[2] = src[2];
        out[3] = src[3];
        break;
    }
}

} // namespace

std::vector<std::string> readNameTable(const u8* p, size_t avail) {
    std::vector<std::string> names;
    if (!p || avail < 4) {
        return names;
    }
    const u16 count = be16(p);
    for (u16 i = 0; i < count; ++i) {
        const size_t entry = 4 + static_cast<size_t>(i) * 4;
        if (entry + 4 > avail) {
            break;
        }
        const u16 offs = be16(p + entry + 2);
        std::string s;
        for (size_t k = offs; k < avail && p[k] != 0; ++k) {
            s.push_back(static_cast<char>(p[k]));
        }
        names.push_back(std::move(s));
    }
    return names;
}

BmdMaterial::BmdMaterial() {
    // J3DMaterialFactory: unset konst selectors default to GX_TEV_KCSEL_1 (0xC)
    // and GX_TEV_KASEL_1 (0x1C).
    for (int i = 0; i < 16; ++i) {
        kColorSel[i] = 0x0C;
        kAlphaSel[i] = 0x1C;
    }
}

s32 BmdModel::findMaterial(const char* name) const {
    if (!name) {
        return -1;
    }
    for (size_t i = 0; i < materials.size(); ++i) {
        if (materials[i].name == name) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

bool BmdModel::load(const u8* data, size_t size, std::string* error) {
    *this = BmdModel();
    if (!data || size < 0x20) {
        return fail(error, "bmd: file too small");
    }
    if (std::memcmp(data, "J3D2", 4) != 0) {
        return fail(error, "bmd: bad magic (expected J3D2)");
    }
    const bool isBmd = std::memcmp(data + 4, "bmd3", 4) == 0;
    const bool isBdl = std::memcmp(data + 4, "bdl4", 4) == 0;
    if (!isBmd && !isBdl) {
        return fail(error, "bmd: not a bmd3/bdl4 file");
    }
    const u32 fileSize = be32(data + 8);
    const u32 blockCount = be32(data + 12);
    if (fileSize > size) {
        return fail(error, "bmd: header size exceeds buffer");
    }
    arrays.assign(GX_VA_MAX_ATTR, BmdVertexArray());

    size_t off = 0x20;
    bool sawInf1 = false, sawVtx1 = false, sawShp1 = false, sawMat3 = false;
    for (u32 b = 0; b < blockCount; ++b) {
        if (off + 8 > fileSize) {
            return fail(error, "bmd: block header past end of file");
        }
        const u8* block = data + off;
        const u32 blockSize = be32(block + 4);
        if (blockSize < 8 || blockSize > fileSize - off) {
            return fail(error, "bmd: block size out of range");
        }
        char tag[5] = {static_cast<char>(block[0]), static_cast<char>(block[1]),
                       static_cast<char>(block[2]), static_cast<char>(block[3]), 0};
        bool ok = true;
        if (std::strcmp(tag, "INF1") == 0) {
            ok = readInf1(block, blockSize, error);
            sawInf1 = ok;
        } else if (std::strcmp(tag, "VTX1") == 0) {
            ok = readVtx1(block, blockSize, error);
            sawVtx1 = ok;
        } else if (std::strcmp(tag, "EVP1") == 0) {
            hasEnvelopes = be16(block + 8) != 0;
        } else if (std::strcmp(tag, "DRW1") == 0) {
            ok = readDrw1(block, blockSize, error);
        } else if (std::strcmp(tag, "JNT1") == 0) {
            ok = readJnt1(block, blockSize, error);
        } else if (std::strcmp(tag, "SHP1") == 0) {
            ok = readShp1(block, blockSize, error);
            sawShp1 = ok;
        } else if (std::strcmp(tag, "MAT3") == 0) {
            ok = readMat3(block, blockSize, error);
            sawMat3 = ok;
        } else if (std::strcmp(tag, "TEX1") == 0) {
            ok = readTex1(block, blockSize, error);
        } else if (std::strcmp(tag, "MDL3") == 0) {
            // BDL precompiled material display lists: the host rebuilds the
            // material state from MAT3 instead (the DLs bake console
            // addresses).
        } else {
            PL_LOG_DEBUG("j3d", "bmd: unknown block '%s' skipped", tag);
        }
        if (!ok) {
            return false;
        }
        off += blockSize;
    }
    if (!sawInf1 || !sawVtx1 || !sawShp1 || !sawMat3) {
        return fail(error, "bmd: missing INF1/VTX1/SHP1/MAT3 block");
    }

    // Resolve the INF1 hierarchy into parent links + draw items
    // (J3DJointTree::makeHierarchy).
    std::vector<s32> stack;
    s32 current = -1;
    s32 currentMaterial = -1;
    for (size_t i = 0; i + 1 < mHierarchy.size(); i += 2) {
        const u16 type = mHierarchy[i];
        const u16 value = mHierarchy[i + 1];
        switch (type) {
        case 0x00:  // end
            i = mHierarchy.size();
            break;
        case 0x01:  // begin children
            stack.push_back(current);
            break;
        case 0x02:  // end children
            if (!stack.empty()) {
                current = stack.back();
                stack.pop_back();
            }
            break;
        case 0x10:  // joint
            if (value < joints.size()) {
                joints[value].parent = static_cast<s16>(stack.empty() ? -1 : stack.back());
                if (rootJoint < 0) {
                    rootJoint = value;
                }
                jointOrder.push_back(value);
                current = value;
            }
            break;
        case 0x11:  // material
            if (value < materials.size()) {
                currentMaterial = value;
                materials[value].joint = current < 0 ? 0 : current;
            }
            break;
        case 0x12:  // shape
            if (value < shapes.size() && currentMaterial >= 0) {
                BmdDrawItem item;
                item.material = static_cast<u16>(currentMaterial);
                item.shape = value;
                item.joint = static_cast<u16>(current < 0 ? 0 : current);
                drawItems.push_back(item);
            }
            break;
        default:
            break;
        }
    }
    if (joints.empty()) {
        // A model without JNT1 still needs one identity joint for the draw matrix.
        joints.emplace_back();
        rootJoint = 0;
        jointOrder.push_back(0);
    }
    // Joints the hierarchy never mentioned (malformed INF1): append as roots
    // so every joint has a matrix.
    for (size_t j = 0; j < joints.size(); ++j) {
        bool listed = false;
        for (u16 o : jointOrder) {
            if (o == j) {
                listed = true;
                break;
            }
        }
        if (!listed) {
            jointOrder.push_back(static_cast<u16>(j));
        }
    }
    if (drawMatrices.empty()) {
        BmdDrawMatrix dm;
        dm.weighted = false;
        dm.index = 0;
        drawMatrices.push_back(dm);
    }
    return true;
}

bool BmdModel::readInf1(const u8* p, size_t n, std::string* error) {
    Chunk c{p, n};
    loadFlags = c.u16At(0x08);
    vertexCount = c.u32At(0x10);
    const u32 hier = c.u32At(0x14);
    if (hier == 0 || hier >= n) {
        return fail(error, "INF1: bad hierarchy offset");
    }
    for (size_t off = hier; off + 4 <= n; off += 4) {
        const u16 type = c.u16At(off);
        const u16 value = c.u16At(off + 2);
        mHierarchy.push_back(type);
        mHierarchy.push_back(value);
        if (type == 0) {
            break;
        }
    }
    return true;
}

bool BmdModel::readVtx1(const u8* p, size_t n, std::string* error) {
    Chunk c{p, n};
    const u32 fmtOffs = c.u32At(0x08);
    if (fmtOffs == 0 || fmtOffs >= n) {
        return fail(error, "VTX1: bad format table offset");
    }
    // 13 array slots: POS, NRM, NBT, CLR0, CLR1, TEX0..7.
    static const u32 kSlotAttr[13] = {GX_VA_POS, GX_VA_NRM, GX_VA_NBT, GX_VA_CLR0, GX_VA_CLR1,
                                      GX_VA_TEX0, GX_VA_TEX1, GX_VA_TEX2, GX_VA_TEX3,
                                      GX_VA_TEX4, GX_VA_TEX5, GX_VA_TEX6, GX_VA_TEX7};
    u32 slotOffs[13];
    for (int i = 0; i < 13; ++i) {
        slotOffs[i] = c.u32At(0x0C + static_cast<size_t>(i) * 4);
    }
    const auto slotEnd = [&](int slot) -> u32 {
        for (int k = slot + 1; k < 13; ++k) {
            if (slotOffs[k] != 0) {
                return slotOffs[k];
            }
        }
        return static_cast<u32>(n);
    };
    for (size_t off = fmtOffs; off + 0x10 <= n; off += 0x10) {
        const u32 attr = c.u32At(off);
        if (attr == GX_VA_NULL) {
            break;
        }
        const u32 cnt = c.u32At(off + 4);
        const u32 type = c.u32At(off + 8);
        const u8 frac = c.u8At(off + 0xC);
        int slot = -1;
        for (int i = 0; i < 13; ++i) {
            if (kSlotAttr[i] == attr) {
                slot = i;
                break;
            }
        }
        if (slot < 0 || slotOffs[slot] == 0 || slotOffs[slot] >= n) {
            continue;
        }
        const u32 start = slotOffs[slot];
        const u32 end = slotEnd(slot);
        if (end < start) {
            return fail(error, "VTX1: array offsets not ascending");
        }
        const size_t bytes = end - start;
        // NBT arrays are exposed as the normals array (GXSetArray does the
        // same on the console).
        const u32 hostAttr = (attr == GX_VA_NBT) ? static_cast<u32>(GX_VA_NRM) : attr;
        if (hostAttr >= arrays.size()) {
            continue;
        }
        BmdVertexArray& a = arrays[hostAttr];
        a.attr = hostAttr;
        a.frac = frac;
        if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
            const u32 srcStride = colorTypeBytes(type);
            const size_t count = bytes / srcStride;
            a.compCnt = GX_CLR_RGBA;
            a.compType = GX_RGBA8;
            a.stride = 4;
            a.elementCount = static_cast<u32>(count);
            a.data.resize(count * 4);
            for (size_t i = 0; i < count; ++i) {
                colorToRgba8(p + start + i * srcStride, type, a.data.data() + i * 4);
            }
        } else {
            const u32 comps = attrCompCount(attr, cnt);
            const u32 compBytes = compTypeBytes(type);
            const u32 srcStride = comps * compBytes;
            if (srcStride == 0) {
                continue;
            }
            const size_t count = bytes / srcStride;
            a.compCnt = (attr == GX_VA_NBT) ? static_cast<u32>(GX_NRM_NBT) : cnt;
            a.compType = type;
            a.stride = static_cast<u8>(srcStride);
            a.elementCount = static_cast<u32>(count);
            a.data.resize(count * srcStride);
            // Byte-swap every component to host order.
            for (size_t i = 0; i < count * comps; ++i) {
                const u8* s = p + start + i * compBytes;
                u8* d = a.data.data() + i * compBytes;
                switch (compBytes) {
                case 1: d[0] = s[0]; break;
                case 2: {
                    const u16 v = be16(s);
                    std::memcpy(d, &v, 2);
                    break;
                }
                default: {
                    const u32 v = be32(s);
                    std::memcpy(d, &v, 4);
                    break;
                }
                }
            }
        }
    }
    return true;
}

bool BmdModel::readDrw1(const u8* p, size_t n, std::string* /*error*/) {
    Chunk c{p, n};
    const u16 count = c.u16At(0x08);
    const u32 typeOffs = c.u32At(0x0C);
    const u32 dataOffs = c.u32At(0x10);
    drawMatrices.clear();
    for (u16 i = 0; i < count; ++i) {
        BmdDrawMatrix dm;
        dm.weighted = c.u8At(typeOffs + i) == 1;
        dm.index = c.u16At(dataOffs + static_cast<size_t>(i) * 2);
        drawMatrices.push_back(dm);
    }
    return true;
}

bool BmdModel::readJnt1(const u8* p, size_t n, std::string* error) {
    Chunk c{p, n};
    const u16 count = c.u16At(0x08);
    const u32 tableOffs = c.u32At(0x0C);
    const u32 remapOffs = c.u32At(0x10);
    const u32 nameOffs = c.u32At(0x14);
    if (count && (tableOffs == 0 || tableOffs >= n)) {
        return fail(error, "JNT1: bad joint table offset");
    }
    std::vector<std::string> names;
    if (nameOffs != 0 && nameOffs < n) {
        names = readNameTable(p + nameOffs, n - nameOffs);
    }
    joints.clear();
    for (u16 i = 0; i < count; ++i) {
        u16 idx = i;
        if (remapOffs != 0 && remapOffs < n) {
            idx = c.u16At(remapOffs + static_cast<size_t>(i) * 2);
        }
        const size_t e = tableOffs + static_cast<size_t>(idx) * 0x40;
        if (!c.has(e, 0x40)) {
            return fail(error, "JNT1: joint entry past end of block");
        }
        BmdJoint j;
        j.name = i < names.size() ? names[i] : std::string();
        j.flags = c.u16At(e);
        j.scaleCompensate = c.u8At(e + 2);
        if (j.scaleCompensate == 0xFF) {
            j.scaleCompensate = 0;
        }
        for (int k = 0; k < 3; ++k) {
            j.transform.scale[k] = c.f32At(e + 4 + k * 4);
            j.transform.rotation[k] = c.s16At(e + 0x10 + k * 2);
            j.transform.translation[k] = c.f32At(e + 0x18 + k * 4);
            j.bboxMin[k] = c.f32At(e + 0x28 + k * 4);
            j.bboxMax[k] = c.f32At(e + 0x34 + k * 4);
        }
        j.radius = c.f32At(e + 0x24);
        joints.push_back(std::move(j));
    }
    return true;
}

bool BmdModel::readShp1(const u8* p, size_t n, std::string* error) {
    Chunk c{p, n};
    const u16 count = c.u16At(0x08);
    const u32 initOffs = c.u32At(0x0C);
    const u32 remapOffs = c.u32At(0x10);
    const u32 declOffs = c.u32At(0x18);
    const u32 mtxTableOffs = c.u32At(0x1C);
    const u32 dlOffs = c.u32At(0x20);
    const u32 mtxInitOffs = c.u32At(0x24);
    const u32 drawInitOffs = c.u32At(0x28);
    if (count && (initOffs == 0 || initOffs >= n || declOffs >= n || dlOffs >= n)) {
        return fail(error, "SHP1: bad table offsets");
    }
    shapes.clear();
    for (u16 i = 0; i < count; ++i) {
        u16 idx = i;
        if (remapOffs != 0 && remapOffs < n) {
            idx = c.u16At(remapOffs + static_cast<size_t>(i) * 2);
        }
        const size_t e = initOffs + static_cast<size_t>(idx) * 0x28;
        if (!c.has(e, 0x28)) {
            return fail(error, "SHP1: shape entry past end of block");
        }
        BmdShape s;
        s.mtxType = c.u8At(e);
        const u16 groupCount = c.u16At(e + 2);
        const u16 declIdx = c.u16At(e + 4);
        const u16 mtxInitIdx = c.u16At(e + 6);
        const u16 drawInitIdx = c.u16At(e + 8);
        s.radius = c.f32At(e + 0xC);
        for (int k = 0; k < 3; ++k) {
            s.bboxMin[k] = c.f32At(e + 0x10 + k * 4);
            s.bboxMax[k] = c.f32At(e + 0x1C + k * 4);
        }
        // Vertex declaration list (u32 attr, u32 type), GX_VA_NULL terminated.
        for (size_t d = declOffs + declIdx; d + 8 <= n; d += 8) {
            const u32 attr = c.u32At(d);
            if (attr == GX_VA_NULL) {
                break;
            }
            BmdVtxDecl decl;
            decl.type = c.u32At(d + 4);
            decl.nbt = (attr == GX_VA_NBT);
            decl.attr = decl.nbt ? static_cast<u32>(GX_VA_NRM) : attr;
            if (attr <= GX_VA_TEX7MTXIDX) {
                s.hasMatrixIndexAttr = true;
            }
            s.decls.push_back(decl);
        }
        for (u16 g = 0; g < groupCount; ++g) {
            BmdMtxGroup grp;
            const size_t di = drawInitOffs + static_cast<size_t>(drawInitIdx + g) * 8;
            const size_t mi = mtxInitOffs + static_cast<size_t>(mtxInitIdx + g) * 8;
            const u32 dlSize = c.u32At(di);
            const u32 dlStart = c.u32At(di + 4);
            if (!c.has(dlOffs + dlStart, dlSize)) {
                return fail(error, "SHP1: packet display list out of range");
            }
            grp.dl = p + dlOffs + dlStart;
            grp.dlSize = dlSize;
            const u16 useCount = c.u16At(mi + 2);
            const u32 first = c.u32At(mi + 4);
            for (u16 k = 0; k < useCount; ++k) {
                grp.mtxTable.push_back(
                    c.u16At(mtxTableOffs + (static_cast<size_t>(first) + k) * 2));
            }
            s.groups.push_back(std::move(grp));
        }
        shapes.push_back(std::move(s));
    }
    return true;
}

bool BmdModel::readMat3(const u8* p, size_t n, std::string* error) {
    Chunk c{p, n};
    const u16 count = c.u16At(0x08);
    const u32 entryOffs = c.u32At(0x0C);
    const u32 remapOffs = c.u32At(0x10);
    const u32 nameOffs = c.u32At(0x14);
    const u32 cullOffs = c.u32At(0x1C);
    const u32 matColorOffs = c.u32At(0x20);
    const u32 chanNumOffs = c.u32At(0x24);
    const u32 chanOffs = c.u32At(0x28);
    const u32 ambOffs = c.u32At(0x2C);
    const u32 texGenNumOffs = c.u32At(0x34);
    const u32 texCoordOffs = c.u32At(0x38);
    const u32 texMtxOffs = c.u32At(0x40);
    const u32 texNoOffs = c.u32At(0x48);
    const u32 tevOrderOffs = c.u32At(0x4C);
    const u32 tevColorOffs = c.u32At(0x50);
    const u32 kColorOffs = c.u32At(0x54);
    const u32 tevStageNumOffs = c.u32At(0x58);
    const u32 tevStageOffs = c.u32At(0x5C);
    const u32 swapModeOffs = c.u32At(0x60);
    const u32 swapTableOffs = c.u32At(0x64);
    const u32 fogOffs = c.u32At(0x68);
    const u32 alphaCompOffs = c.u32At(0x6C);
    const u32 blendOffs = c.u32At(0x70);
    const u32 zModeOffs = c.u32At(0x74);
    const u32 zCompOffs = c.u32At(0x78);
    const u32 ditherOffs = c.u32At(0x7C);
    if (count && (entryOffs == 0 || entryOffs >= n)) {
        return fail(error, "MAT3: bad material entry offset");
    }
    std::vector<std::string> names;
    if (nameOffs != 0 && nameOffs < n) {
        names = readNameTable(p + nameOffs, n - nameOffs);
    }
    materials.clear();
    for (u16 i = 0; i < count; ++i) {
        u16 idx = i;
        if (remapOffs != 0 && remapOffs < n) {
            idx = c.u16At(remapOffs + static_cast<size_t>(i) * 2);
        }
        const size_t e = entryOffs + static_cast<size_t>(idx) * 0x14C;
        if (!c.has(e, 0x14C)) {
            return fail(error, "MAT3: material entry past end of block");
        }
        BmdMaterial m;
        m.name = i < names.size() ? names[i] : std::string();
        m.mode = c.u8At(e + 0x00);
        const u8 cullIdx = c.u8At(e + 0x01);
        const u8 chanNumIdx = c.u8At(e + 0x02);
        const u8 texGenNumIdx = c.u8At(e + 0x03);
        const u8 tevStageNumIdx = c.u8At(e + 0x04);
        const u8 zCompIdx = c.u8At(e + 0x05);
        const u8 zModeIdx = c.u8At(e + 0x06);
        const u8 ditherIdx = c.u8At(e + 0x07);

        if (cullIdx != 0xFF && cullOffs) {
            m.cullMode = c.u32At(cullOffs + static_cast<size_t>(cullIdx) * 4);
        }
        for (int j = 0; j < 2; ++j) {
            const u16 mi = c.u16At(e + 0x08 + j * 2);
            if (mi != 0xFFFF && matColorOffs) {
                const size_t o = matColorOffs + static_cast<size_t>(mi) * 4;
                for (int k = 0; k < 4; ++k) {
                    m.matColor[j][k] = c.u8At(o + k);
                }
            }
            const u16 ai = c.u16At(e + 0x14 + j * 2);
            if (ai != 0xFFFF && ambOffs) {
                const size_t o = ambOffs + static_cast<size_t>(ai) * 4;
                for (int k = 0; k < 4; ++k) {
                    m.ambColor[j][k] = c.u8At(o + k);
                }
            }
        }
        if (chanNumIdx != 0xFF && chanNumOffs) {
            m.chanNum = c.u8At(chanNumOffs + chanNumIdx);
        }
        for (int j = 0; j < 4; ++j) {
            const u16 ci = c.u16At(e + 0x0C + j * 2);
            BmdColorChan& ch = m.chan[j];
            if (ci != 0xFFFF && chanOffs) {
                const size_t o = chanOffs + static_cast<size_t>(ci) * 8;
                ch.enable = c.u8At(o + 0);
                ch.matSrc = c.u8At(o + 1);
                ch.lightMask = c.u8At(o + 2);
                ch.diffuseFn = c.u8At(o + 3);
                ch.attnFn = c.u8At(o + 4);
                ch.ambSrc = c.u8At(o + 5);
                if (ch.ambSrc == 0xFF) {
                    ch.ambSrc = 0;
                }
            }
        }
        if (texGenNumIdx != 0xFF && texGenNumOffs) {
            m.texGenNum = c.u8At(texGenNumOffs + texGenNumIdx);
        }
        for (int j = 0; j < 8; ++j) {
            const u16 ti = c.u16At(e + 0x28 + j * 2);
            BmdTexCoord& tc = m.texCoord[j];
            if (ti != 0xFFFF && texCoordOffs) {
                const size_t o = texCoordOffs + static_cast<size_t>(ti) * 4;
                tc.type = c.u8At(o + 0);
                tc.src = c.u8At(o + 1);
                tc.mtx = c.u8At(o + 2);
            } else {
                tc.type = GX_TG_MTX2x4;
                tc.src = static_cast<u8>(GX_TG_TEX0 + j);
                tc.mtx = GX_IDENTITY;
            }
            const u16 mi = c.u16At(e + 0x48 + j * 2);
            BmdTexMtx& tm = m.texMtx[j];
            if (mi != 0xFFFF && texMtxOffs) {
                const size_t o = texMtxOffs + static_cast<size_t>(mi) * 0x64;
                if (c.has(o, 0x64)) {
                    tm.valid = true;
                    tm.projection = c.u8At(o + 0);
                    tm.info = c.u8At(o + 1);
                    tm.center[0] = c.f32At(o + 0x04);
                    tm.center[1] = c.f32At(o + 0x08);
                    tm.center[2] = c.f32At(o + 0x0C);
                    tm.srt.scaleX = c.f32At(o + 0x10);
                    tm.srt.scaleY = c.f32At(o + 0x14);
                    tm.srt.rotation = c.s16At(o + 0x18);
                    tm.srt.transX = c.f32At(o + 0x1C);
                    tm.srt.transY = c.f32At(o + 0x20);
                    for (int r = 0; r < 4; ++r) {
                        for (int k = 0; k < 4; ++k) {
                            tm.effectMtx[r][k] = c.f32At(o + 0x24 + (r * 4 + k) * 4);
                        }
                    }
                }
            }
            const u16 tni = c.u16At(e + 0x84 + j * 2);
            if (tni != 0xFFFF && texNoOffs) {
                m.texNo[j] = c.u16At(texNoOffs + static_cast<size_t>(tni) * 2);
            }
        }
        for (int j = 0; j < 4; ++j) {
            const u16 ki = c.u16At(e + 0x94 + j * 2);
            if (ki != 0xFFFF && kColorOffs) {
                const size_t o = kColorOffs + static_cast<size_t>(ki) * 4;
                for (int k = 0; k < 4; ++k) {
                    m.kColor[j][k] = c.u8At(o + k);
                }
            }
            const u16 ci = c.u16At(e + 0xDC + j * 2);
            if (ci != 0xFFFF && tevColorOffs) {
                const size_t o = tevColorOffs + static_cast<size_t>(ci) * 8;
                for (int k = 0; k < 4; ++k) {
                    m.tevColor[j][k] = c.s16At(o + k * 2);
                }
            }
        }
        if (tevStageNumIdx != 0xFF && tevStageNumOffs) {
            m.tevStageNum = c.u8At(tevStageNumOffs + tevStageNumIdx);
        }
        for (int j = 0; j < 16; ++j) {
            const u8 kc = c.u8At(e + 0x9C + j);
            const u8 ka = c.u8At(e + 0xAC + j);
            m.kColorSel[j] = (kc != 0xFF) ? kc : 0x0C;
            m.kAlphaSel[j] = (ka != 0xFF) ? ka : 0x1C;
            const u16 oi = c.u16At(e + 0xBC + j * 2);
            if (oi != 0xFFFF && tevOrderOffs) {
                const size_t o = tevOrderOffs + static_cast<size_t>(oi) * 4;
                m.tevOrder[j].texCoord = c.u8At(o + 0);
                m.tevOrder[j].texMap = c.u8At(o + 1);
                m.tevOrder[j].colorChan = c.u8At(o + 2);
            }
            const u16 si = c.u16At(e + 0xE4 + j * 2);
            if (si != 0xFFFF && tevStageOffs) {
                const size_t o = tevStageOffs + static_cast<size_t>(si) * 0x14;
                BmdTevStage& st = m.tevStage[j];
                for (int k = 0; k < 4; ++k) {
                    st.colorIn[k] = c.u8At(o + 1 + k);
                    st.alphaIn[k] = c.u8At(o + 0xA + k);
                }
                st.colorOp = c.u8At(o + 5);
                st.colorBias = c.u8At(o + 6);
                st.colorScale = c.u8At(o + 7);
                st.colorClamp = c.u8At(o + 8);
                st.colorReg = c.u8At(o + 9);
                st.alphaOp = c.u8At(o + 0xE);
                st.alphaBias = c.u8At(o + 0xF);
                st.alphaScale = c.u8At(o + 0x10);
                st.alphaClamp = c.u8At(o + 0x11);
                st.alphaReg = c.u8At(o + 0x12);
            }
            const u16 smi = c.u16At(e + 0x104 + j * 2);
            if (smi != 0xFFFF && swapModeOffs) {
                const size_t o = swapModeOffs + static_cast<size_t>(smi) * 4;
                m.swapMode[j][0] = c.u8At(o + 0);
                m.swapMode[j][1] = c.u8At(o + 1);
            }
        }
        for (int j = 0; j < 4; ++j) {
            const u16 sti = c.u16At(e + 0x124 + j * 2);
            if (sti != 0xFFFF && swapTableOffs) {
                const size_t o = swapTableOffs + static_cast<size_t>(sti) * 4;
                for (int k = 0; k < 4; ++k) {
                    m.swapTable[j][k] = c.u8At(o + k);
                }
            }
        }
        const u16 fi = c.u16At(e + 0x144);
        if (fi != 0xFFFF && fogOffs) {
            const size_t o = fogOffs + static_cast<size_t>(fi) * 0x2C;
            m.fog.type = c.u8At(o + 0);
            m.fog.adjEnable = c.u8At(o + 1);
            m.fog.center = c.u16At(o + 2);
            m.fog.startZ = c.f32At(o + 4);
            m.fog.endZ = c.f32At(o + 8);
            m.fog.nearZ = c.f32At(o + 0xC);
            m.fog.farZ = c.f32At(o + 0x10);
            for (int k = 0; k < 4; ++k) {
                m.fog.color[k] = c.u8At(o + 0x14 + k);
            }
            for (int k = 0; k < 10; ++k) {
                m.fog.adjTable[k] = c.u16At(o + 0x18 + k * 2);
            }
        }
        const u16 aci = c.u16At(e + 0x146);
        if (aci != 0xFFFF && alphaCompOffs) {
            const size_t o = alphaCompOffs + static_cast<size_t>(aci) * 8;
            for (int k = 0; k < 5; ++k) {
                m.alphaComp[k] = c.u8At(o + k);
            }
        }
        const u16 bi = c.u16At(e + 0x148);
        if (bi != 0xFFFF && blendOffs) {
            const size_t o = blendOffs + static_cast<size_t>(bi) * 4;
            for (int k = 0; k < 4; ++k) {
                m.blend[k] = c.u8At(o + k);
            }
        }
        if (zModeIdx != 0xFF && zModeOffs) {
            const size_t o = zModeOffs + static_cast<size_t>(zModeIdx) * 4;
            m.zMode[0] = c.u8At(o + 0);
            m.zMode[1] = c.u8At(o + 1);
            m.zMode[2] = c.u8At(o + 2);
        }
        if (zCompIdx != 0xFF && zCompOffs) {
            m.zCompLoc = c.u8At(zCompOffs + zCompIdx);
        }
        if (ditherIdx != 0xFF && ditherOffs) {
            m.dither = c.u8At(ditherOffs + ditherIdx);
        }
        materials.push_back(std::move(m));
    }
    return true;
}

bool BmdModel::readTex1(const u8* p, size_t n, std::string* error) {
    Chunk c{p, n};
    const u16 count = c.u16At(0x08);
    const u32 hdrOffs = c.u32At(0x0C);
    const u32 nameOffs = c.u32At(0x10);
    if (count && (hdrOffs == 0 || hdrOffs >= n)) {
        return fail(error, "TEX1: bad header table offset");
    }
    std::vector<std::string> names;
    if (nameOffs != 0 && nameOffs < n) {
        names = readNameTable(p + nameOffs, n - nameOffs);
    }
    textures.clear();
    for (u16 i = 0; i < count; ++i) {
        const size_t h = hdrOffs + static_cast<size_t>(i) * 0x20;
        if (!c.has(h, 0x20)) {
            return fail(error, "TEX1: BTI header past end of block");
        }
        BmdTexture t;
        t.name = i < names.size() ? names[i] : std::string();
        Platform::CompatGx::btiParseHeader(p + h, 0x20, t.header);
        t.mipmap = c.u8At(h + 0x10);
        t.edgeLod = c.u8At(h + 0x11);
        t.biasClamp = c.u8At(h + 0x12);
        t.maxAnisotropy = c.u8At(h + 0x13);
        t.minFilter = c.u8At(h + 0x14);
        t.magFilter = c.u8At(h + 0x15);
        t.minLod = c.u8At(h + 0x16);
        t.maxLod = c.u8At(h + 0x17);
        t.lodBias = c.s16At(h + 0x1A);
        // Offsets are relative to the BTI header start (JUTTexture layout).
        const size_t imageBytes = Platform::CompatGx::btiImageSize(t.header.width, t.header.height,
                                                                   t.header.format);
        // M9.5.8: a mip-mapped texture stores its whole pyramid after the base
        // level, each level tile-padded. Hand the loader as many levels as the
        // block actually holds so the host can upload the chain the LOD range
        // samples (the console's texture unit derives their addresses itself).
        size_t blobBytes = imageBytes;
        if (t.mipmap != 0) {
            size_t chain = 0;
            u16 w = t.header.width;
            u16 hh = t.header.height;
            while (chain < imageBytes * 8) {
                const size_t levelBytes = Platform::CompatGx::btiImageSize(w, hh, t.header.format);
                if (levelBytes == 0) {
                    break;
                }
                chain += levelBytes;
                if (w <= 1 && hh <= 1) {
                    break;
                }
                if (w > 1) w = static_cast<u16>(w >> 1);
                if (hh > 1) hh = static_cast<u16>(hh >> 1);
            }
            blobBytes = chain;
        }
        const size_t imgOff = h + t.header.imageOffset;
        if (t.header.imageOffset != 0 && c.has(imgOff, blobBytes) && imageBytes != 0) {
            t.image = p + imgOff;
            t.imageBytes = blobBytes;
        } else if (t.header.imageOffset != 0 && c.has(imgOff, imageBytes) && imageBytes != 0) {
            PL_LOG_WARN("j3d", "TEX1: texture '%s' image out of range (fmt 0x%x %ux%u)",
                        t.name.c_str(), t.header.format, t.header.width, t.header.height);
        }
        if (t.header.paletteOffset != 0 && t.header.paletteCount != 0) {
            const size_t palOff = h + t.header.paletteOffset;
            const size_t palBytes = static_cast<size_t>(t.header.paletteCount) * 2;
            if (c.has(palOff, palBytes)) {
                t.palette = p + palOff;
                t.paletteBytes = palBytes;
            }
        }
        textures.push_back(std::move(t));
    }
    return true;
}

} // namespace compat::j3d
