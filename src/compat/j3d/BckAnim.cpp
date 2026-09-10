// =============================================================================
// compat/j3d — BCK reader/evaluator (see BckAnim.h).
// =============================================================================

#include "compat/j3d/BckAnim.h"

#include <cstring>

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

bool BckAnim::load(const u8* data, size_t size, std::string* error) {
    *this = BckAnim();
    if (!data || size < 0x20 + 0x24) {
        return fail(error, "bck: file too small");
    }
    if (std::memcmp(data, "J3D1bck1", 8) != 0) {
        return fail(error, "bck: bad magic (expected J3D1bck1)");
    }
    const u32 fileSize = be32(data + 8);
    if (fileSize > size) {
        return fail(error, "bck: header size exceeds buffer");
    }
    const u8* blk = data + 0x20;
    const size_t avail = fileSize - 0x20;
    if (std::memcmp(blk, "ANK1", 4) != 0) {
        return fail(error, "bck: ANK1 block missing");
    }
    const u32 blkSize = be32(blk + 4);
    if (blkSize < 0x24 || blkSize > avail) {
        return fail(error, "bck: ANK1 block size out of range");
    }
    const auto has = [&](size_t off, size_t len) { return off <= blkSize && len <= blkSize - off; };

    loopMode = blk[0x08];
    rotDecShift = blk[0x09];
    duration = be16(blk + 0x0A);
    const u16 jointCount = be16(blk + 0x0C);
    const u16 sCount = be16(blk + 0x0E);
    const u16 rCount = be16(blk + 0x10);
    const u16 tCount = be16(blk + 0x12);
    const u32 tableOffs = be32(blk + 0x14);
    const u32 sOffs = be32(blk + 0x18);
    const u32 rOffs = be32(blk + 0x1C);
    const u32 tOffs = be32(blk + 0x20);
    if (!has(tableOffs, static_cast<size_t>(jointCount) * 0x36) ||
        !has(sOffs, static_cast<size_t>(sCount) * 4) || !has(rOffs, static_cast<size_t>(rCount) * 2) ||
        !has(tOffs, static_cast<size_t>(tCount) * 4)) {
        return fail(error, "bck: table offsets out of range");
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
    joints.clear();
    for (u16 i = 0; i < jointCount; ++i) {
        // Per joint: X, Y, Z × {scale, rotation, translation} tracks of 6
        // bytes = 0x36.
        const u8* t = blk + tableOffs + static_cast<size_t>(i) * 0x36;
        BckJointTracks j;
        for (int k = 0; k < 3; ++k) {
            j.scale[k] = readTrack(t + k * 0x12 + 0x00);
            j.rotation[k] = readTrack(t + k * 0x12 + 0x06);
            j.translation[k] = readTrack(t + k * 0x12 + 0x0C);
        }
        joints.push_back(j);
    }
    return true;
}

void BckAnim::evaluate(size_t i, f32 frame, JointTransform& out) const {
    out = JointTransform();
    if (i >= joints.size()) {
        return;
    }
    const BckJointTracks& j = joints[i];
    for (int k = 0; k < 3; ++k) {
        out.scale[k] = evalTrackF32(j.scale[k], mScaleData.data(), mScaleData.size(), frame, 1.0f);
        const f32 rot = evalTrackS16(j.rotation[k], mRotData.data(), mRotData.size(), frame);
        const s32 shifted = static_cast<s32>(rot) << rotDecShift;
        out.rotation[k] = static_cast<s16>(static_cast<u16>(shifted & 0xFFFF));
        out.translation[k] =
            evalTrackF32(j.translation[k], mTransData.data(), mTransData.size(), frame, 0.0f);
    }
}

} // namespace compat::j3d
