// =============================================================================
// compat/j3d — BTK reader/evaluator (see BtkAnim.h).
// =============================================================================

#include "compat/j3d/BtkAnim.h"

#include <cstring>

#include "compat/j3d/BmdModel.h"  // readNameTable

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

bool fail(std::string* error, const char* msg) {
    if (error) {
        *error = msg;
    }
    return false;
}

KeyTrack readTrack(const u8* p) {
    KeyTrack t;
    t.count = be16(p);
    t.index = be16(p + 2);
    t.tangentType = be16(p + 4);
    return t;
}

} // namespace

bool BtkAnim::load(const u8* data, size_t size, std::string* error) {
    *this = BtkAnim();
    if (!data || size < 0x20 + 0x60) {
        return fail(error, "btk: file too small");
    }
    if (std::memcmp(data, "J3D1btk1", 8) != 0) {
        return fail(error, "btk: bad magic (expected J3D1btk1)");
    }
    // Single TTK1 block right after the 0x20 file header.
    const u8* blk = data + 0x20;
    const size_t avail = size - 0x20;
    if (std::memcmp(blk, "TTK1", 4) != 0) {
        return fail(error, "btk: TTK1 block missing");
    }
    // PC_PORT: the block length is NOT a reliable field in the wild, and the
    // reference loader never reads it — petari's J3DAnmKeyLoader_v15::load
    // walks the blocks by the FILE header's block count and the block header's
    // "next" offset only, and the vendored struct even names 0x08 "unknown"
    // (J3DGraphAnimator/J3DAnimation.hpp: J3DAnmDataHeader::_8). Some writers
    // leave the block size at 0 ("no next block", which is what SMG's own
    // /ObjectData/CometNearOrbitSky arc does — 'cometnearorbitsky.btk' failed
    // to attach with the old check), others disagree with the file header.
    //
    // The buffer we were handed is ground truth: the file comes from the
    // mounted arc, so its length is exact. Clamp instead of rejecting — every
    // offset/length read below is bounds-checked against blkSize by the `has`
    // lambda, so a clamped block can never read outside the buffer, and a
    // genuinely truncated file still fails on those checks (or on the 0x60
    // structural minimum, J3DAnmTextureSRTKeyData is 0x60 bytes).
    size_t blkSize = be32(blk + 4);
    if (blkSize == 0 || blkSize > avail) {
        PL_LOG_TRACE("compat.j3d", "btk: TTK1 block size %zu clamped to the %zu byte buffer "
                                   "(file header says %zu)", blkSize, avail, static_cast<size_t>(be32(data + 8)));
        blkSize = avail;
    }
    if (blkSize < 0x60) {
        return fail(error, "btk: TTK1 block smaller than its 0x60 byte header");
    }
    const auto has = [&](size_t off, size_t len) { return off <= blkSize && len <= blkSize - off; };

    loopMode = blk[0x08];
    rotDecShift = blk[0x09];
    duration = be16(blk + 0x0A);
    const u16 trackCount = be16(blk + 0x0C);
    const u16 sCount = be16(blk + 0x0E);
    const u16 rCount = be16(blk + 0x10);
    const u16 tCount = be16(blk + 0x12);
    const u32 tableOffs = be32(blk + 0x14);
    const u32 nameOffs = be32(blk + 0x1C);
    const u32 texMtxIdxOffs = be32(blk + 0x20);
    const u32 centerOffs = be32(blk + 0x24);
    const u32 sOffs = be32(blk + 0x28);
    const u32 rOffs = be32(blk + 0x2C);
    const u32 tOffs = be32(blk + 0x30);
    maya = be32(blk + 0x5C) == 1;

    const u16 entryCount = trackCount / 3;
    if (!has(tableOffs, static_cast<size_t>(entryCount) * 0x36) ||
        !has(sOffs, static_cast<size_t>(sCount) * 4) || !has(rOffs, static_cast<size_t>(rCount) * 2) ||
        !has(tOffs, static_cast<size_t>(tCount) * 4) || !has(texMtxIdxOffs, entryCount) ||
        !has(centerOffs, static_cast<size_t>(entryCount) * 12)) {
        return fail(error, "btk: table offsets out of range");
    }
    mScaleData.resize(sCount);
    for (u16 i = 0; i < sCount; ++i) {
        const u32 v = be32(blk + sOffs + static_cast<size_t>(i) * 4);
        std::memcpy(&mScaleData[i], &v, 4);
    }
    mRotData.resize(rCount);
    for (u16 i = 0; i < rCount; ++i) {
        mRotData[i] = static_cast<s16>(be16(blk + rOffs + static_cast<size_t>(i) * 2));
    }
    mTransData.resize(tCount);
    for (u16 i = 0; i < tCount; ++i) {
        const u32 v = be32(blk + tOffs + static_cast<size_t>(i) * 4);
        std::memcpy(&mTransData[i], &v, 4);
    }
    std::vector<std::string> names;
    if (nameOffs != 0 && nameOffs < blkSize) {
        names = readNameTable(blk + nameOffs, blkSize - nameOffs);
    }
    entries.clear();
    for (u16 i = 0; i < entryCount; ++i) {
        BtkEntry e;
        e.materialName = i < names.size() ? names[i] : std::string();
        e.texMtxSlot = blk[texMtxIdxOffs + i];
        for (int k = 0; k < 3; ++k) {
            const u32 v = be32(blk + centerOffs + static_cast<size_t>(i) * 12 + k * 4);
            std::memcpy(&e.center[k], &v, 4);
        }
        // Per entry: 3 components (S, T, Q) x {scale, rotation, translation}
        // tracks of 6 bytes each = 0x36.
        const u8* t = blk + tableOffs + static_cast<size_t>(i) * 0x36;
        for (int k = 0; k < 3; ++k) {
            e.scale[k] = readTrack(t + k * 0x12 + 0x00);
            e.rotation[k] = readTrack(t + k * 0x12 + 0x06);
            e.translation[k] = readTrack(t + k * 0x12 + 0x0C);
        }
        entries.push_back(std::move(e));
    }
    return true;
}

void BtkAnim::evaluate(size_t i, f32 frame, TexSrt& out) const {
    out = TexSrt();
    if (i >= entries.size()) {
        return;
    }
    const BtkEntry& e = entries[i];
    // J3DAnmTextureSRTKey::calcTransform: scale X <- S, scale Y <- T,
    // rotation <- Q's rotation track, translation X/Y <- S/T.
    out.scaleX = evalTrackF32(e.scale[0], mScaleData.data(), mScaleData.size(), frame, 1.0f);
    out.scaleY = evalTrackF32(e.scale[1], mScaleData.data(), mScaleData.size(), frame, 1.0f);
    const f32 rot = evalTrackS16(e.rotation[2], mRotData.data(), mRotData.size(), frame);
    // `(int)value << decShift`, wrapped to s16 like the console's u16 store.
    const s32 shifted = static_cast<s32>(rot) << rotDecShift;
    out.rotation = static_cast<s16>(static_cast<u16>(shifted & 0xFFFF));
    out.transX = evalTrackF32(e.translation[0], mTransData.data(), mTransData.size(), frame, 0.0f);
    out.transY = evalTrackF32(e.translation[1], mTransData.data(), mTransData.size(), frame, 0.0f);
}

} // namespace compat::j3d
