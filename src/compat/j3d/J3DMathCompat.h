#pragma once
// =============================================================================
// compat/j3d — matrix / keyframe helpers for the host BMD renderer (M9.5.4 v8).
//
// Convention: GX/J3D `Mtx` is f32[3][4] in COLUMN-VECTOR form, out = M * v with
// the translation in column 3 (out[r] = m[r][0]*x + m[r][1]*y + m[r][2]*z +
// m[r][3]). PSMTXConcat(a, b) = a * b. Everything here follows that convention
// so the results can be handed to GXLoadPosMtxImm/GXLoadTexMtxImm untouched.
//
// The functions mirror the JSystem / RVL SDK routines the J3D pipeline calls
// (J3DGetTranslateRotateMtx, J3DGetTextureMtx*, J3DMtxProjConcat,
// JMAMTXApplyScale, C_MTXLookAt, PSMTXInverse, JMAHermiteInterpolation,
// J3DGetKeyFrameInterpolation) — the originals are Paired-Single asm in the
// vendored tree and are not compiled on the host.
// =============================================================================

#include <revolution/types.h>
#include <revolution/mtx.h>

#include <cstddef>

namespace compat::j3d {

void mtxIdentity(Mtx m);
void mtxCopy(const Mtx src, Mtx dst);
/// dst = a * b (alias-safe).
void mtxConcat(const Mtx a, const Mtx b, Mtx dst);
/// Affine inverse. Returns false (dst = identity) when singular.
bool mtxInverse(const Mtx src, Mtx dst);
/// Rotation about an axis (normalised here), zero translation — TPos3f::makeRotate.
void mtxRotAxisRad(Mtx m, f32 ax, f32 ay, f32 az, f32 rad);
/// dst = src * Scale(sx, sy, sz) — JMAMTXApplyScale (columns scaled, translation kept).
void mtxApplyScale(const Mtx src, Mtx dst, f32 sx, f32 sy, f32 sz);
/// C_MTXLookAt: camera at `eye` looking at `target` (GX camera looks down -Z).
void mtxLookAt(Mtx m, const f32 eye[3], const f32 up[3], const f32 target[3]);
void mtxMultVec(const Mtx m, const f32 in[3], f32 out[3]);
/// 3x4 -> 4x4 with the implicit (0,0,0,1) row.
void mtxToMtx44(const Mtx src, Mtx44 dst);
/// J3DMtxProjConcat: dst(3x4) = a(3x4) * b(4x4) — a's implicit fourth row is
/// NOT used (only three output rows, each a's row dotted with b's columns).
void mtxProjConcat(const Mtx a, const Mtx44 b, Mtx dst);
/// Inverse-transpose of the 3x3 part (normal matrix), translation zeroed.
void mtxNormalMtx(const Mtx m, Mtx dst);
/// Zeroes the translation column (J3DTexGenBlockPatched::calc envmap input).
void mtxZeroTranslation(Mtx m);

/// JMASSin / JMASCos: 16-bit angle units (65536 = 2*pi).
f32 sinShort(s16 v);
f32 cosShort(s16 v);

/// J3DTransformInfo (JNT1 entry / BCK output).
struct JointTransform {
    f32 scale[3] = {1.0f, 1.0f, 1.0f};
    s16 rotation[3] = {0, 0, 0};
    f32 translation[3] = {0.0f, 0.0f, 0.0f};
};
/// J3DGetTranslateRotateMtx (rotation order Z * Y * X, rotations in s16 units).
void getTranslateRotateMtx(const JointTransform& t, Mtx dst);

/// J3DTextureSRTInfo.
struct TexSrt {
    f32 scaleX = 1.0f;
    f32 scaleY = 1.0f;
    s16 rotation = 0;
    f32 transX = 0.0f;
    f32 transY = 0.0f;
};
/// J3DGetTextureMtx: translation in column 2 (multiplies the AB11 "1").
void getTextureMtx(const TexSrt& srt, const f32 center[3], Mtx dst);
/// J3DGetTextureMtxOld: translation in column 3.
void getTextureMtxOld(const TexSrt& srt, const f32 center[3], Mtx dst);
void getTextureMtxMaya(const TexSrt& srt, Mtx dst);
void getTextureMtxMayaOld(const TexSrt& srt, Mtx dst);

// --- keyframe animation ------------------------------------------------------

/// J3DAnmKeyTableBase: count, first index into the data table, tangent type
/// (0 = one tangent per key {t, v, tan}; 1 = in/out tangents {t, v, in, out}).
struct KeyTrack {
    u16 count = 0;
    u16 index = 0;
    u16 tangentType = 0;
};

/// JMAHermiteInterpolation: cubic Hermite between (t0, v0, tanOut0) and
/// (t1, v1, tanIn1); tangents are per-frame slopes (scaled by the segment).
f32 hermite(f32 frame, f32 t0, f32 v0, f32 tanOut0, f32 t1, f32 v1, f32 tanIn1);

/// J3DGetKeyFrameInterpolation over a track of f32 keys (`data` is the whole
/// table; the track starts at `track.index`). `count` 0 -> `defaultValue`,
/// 1 -> the single value, otherwise the interpolated value (clamped to the
/// first/last key outside the range).
f32 evalTrackF32(const KeyTrack& track, const f32* data, size_t dataCount, f32 frame,
                 f32 defaultValue);
/// Same over s16 keys (rotation tables). The result is in the table's units
/// (the caller applies the decimal shift and wraps to s16).
f32 evalTrackS16(const KeyTrack& track, const s16* data, size_t dataCount, f32 frame);

/// J3DFrameCtrl subset: frame advance with the J3D loop modes
/// (0 once, 1 once-and-reset, 2 repeat, 3 mirror-once, 4 mirror-repeat).
struct FrameCtrl {
    f32 frame = 0.0f;
    f32 rate = 1.0f;
    f32 start = 0.0f;
    f32 end = 1.0f;
    u8 loopMode = 2;
    void init(u8 mode, f32 endFrame);
    void update();
};

} // namespace compat::j3d
