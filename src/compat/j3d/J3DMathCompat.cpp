// =============================================================================
// compat/j3d — matrix / keyframe helpers (see J3DMathCompat.h).
// =============================================================================

#include "compat/j3d/J3DMathCompat.h"

#include <cmath>
#include <cstring>

namespace compat::j3d {

namespace {
constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kShortToRad = kPi / 32768.0f;
} // namespace

void mtxIdentity(Mtx m) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
}

void mtxCopy(const Mtx src, Mtx dst) {
    if (src != dst) {
        std::memcpy(dst, src, sizeof(Mtx));
    }
}

void mtxConcat(const Mtx a, const Mtx b, Mtx dst) {
    Mtx tmp;
    for (int i = 0; i < 3; ++i) {
        const f32 a0 = a[i][0];
        const f32 a1 = a[i][1];
        const f32 a2 = a[i][2];
        tmp[i][0] = a0 * b[0][0] + a1 * b[1][0] + a2 * b[2][0];
        tmp[i][1] = a0 * b[0][1] + a1 * b[1][1] + a2 * b[2][1];
        tmp[i][2] = a0 * b[0][2] + a1 * b[1][2] + a2 * b[2][2];
        tmp[i][3] = a0 * b[0][3] + a1 * b[1][3] + a2 * b[2][3] + a[i][3];
    }
    mtxCopy(tmp, dst);
}

bool mtxInverse(const Mtx src, Mtx dst) {
    const f32 det = src[0][0] * (src[1][1] * src[2][2] - src[1][2] * src[2][1]) -
                    src[0][1] * (src[1][0] * src[2][2] - src[1][2] * src[2][0]) +
                    src[0][2] * (src[1][0] * src[2][1] - src[1][1] * src[2][0]);
    if (det == 0.0f) {
        mtxIdentity(dst);
        return false;
    }
    const f32 inv = 1.0f / det;
    Mtx tmp;
    tmp[0][0] = (src[1][1] * src[2][2] - src[1][2] * src[2][1]) * inv;
    tmp[0][1] = -(src[0][1] * src[2][2] - src[0][2] * src[2][1]) * inv;
    tmp[0][2] = (src[0][1] * src[1][2] - src[0][2] * src[1][1]) * inv;
    tmp[1][0] = -(src[1][0] * src[2][2] - src[1][2] * src[2][0]) * inv;
    tmp[1][1] = (src[0][0] * src[2][2] - src[0][2] * src[2][0]) * inv;
    tmp[1][2] = -(src[0][0] * src[1][2] - src[0][2] * src[1][0]) * inv;
    tmp[2][0] = (src[1][0] * src[2][1] - src[1][1] * src[2][0]) * inv;
    tmp[2][1] = -(src[0][0] * src[2][1] - src[0][1] * src[2][0]) * inv;
    tmp[2][2] = (src[0][0] * src[1][1] - src[0][1] * src[1][0]) * inv;
    // -R^-1 * t
    tmp[0][3] = -(tmp[0][0] * src[0][3] + tmp[0][1] * src[1][3] + tmp[0][2] * src[2][3]);
    tmp[1][3] = -(tmp[1][0] * src[0][3] + tmp[1][1] * src[1][3] + tmp[1][2] * src[2][3]);
    tmp[2][3] = -(tmp[2][0] * src[0][3] + tmp[2][1] * src[1][3] + tmp[2][2] * src[2][3]);
    mtxCopy(tmp, dst);
    return true;
}

void mtxRotAxisRad(Mtx m, f32 ax, f32 ay, f32 az, f32 rad) {
    // TRotation3::setRotate(axis, angle) (JGeometry TMatrix.hpp) — normalises
    // the axis first.
    const f32 len = std::sqrt(ax * ax + ay * ay + az * az);
    f32 x = ax, y = ay, z = az;
    if (len > 0.0f) {
        x /= len;
        y /= len;
        z /= len;
    }
    const f32 s = std::sin(rad);
    const f32 c = std::cos(rad);
    const f32 negc = 1.0f - c;
    m[0][0] = c + negc * x * x;
    m[0][1] = negc * x * y - s * z;
    m[0][2] = negc * x * z + s * y;
    m[1][0] = negc * x * y + s * z;
    m[1][1] = c + negc * y * y;
    m[1][2] = negc * y * z - s * x;
    m[2][0] = negc * x * z - s * y;
    m[2][1] = negc * y * z + s * x;
    m[2][2] = c + negc * z * z;
    m[0][3] = m[1][3] = m[2][3] = 0.0f;
}

void mtxApplyScale(const Mtx src, Mtx dst, f32 sx, f32 sy, f32 sz) {
    for (int r = 0; r < 3; ++r) {
        const f32 c0 = src[r][0] * sx;
        const f32 c1 = src[r][1] * sy;
        const f32 c2 = src[r][2] * sz;
        const f32 c3 = src[r][3];
        dst[r][0] = c0;
        dst[r][1] = c1;
        dst[r][2] = c2;
        dst[r][3] = c3;
    }
}

void mtxLookAt(Mtx m, const f32 eye[3], const f32 up[3], const f32 target[3]) {
    // C_MTXLookAt (mtx44/mtx.c): look = normalize(eye - target) (camera Z axis
    // points backwards), right = normalize(up x look), camUp = look x right.
    f32 look[3] = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    f32 len = std::sqrt(look[0] * look[0] + look[1] * look[1] + look[2] * look[2]);
    if (len > 0.0f) {
        look[0] /= len;
        look[1] /= len;
        look[2] /= len;
    }
    f32 right[3] = {up[1] * look[2] - up[2] * look[1], up[2] * look[0] - up[0] * look[2],
                    up[0] * look[1] - up[1] * look[0]};
    len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (len > 0.0f) {
        right[0] /= len;
        right[1] /= len;
        right[2] /= len;
    }
    const f32 camUp[3] = {look[1] * right[2] - look[2] * right[1],
                          look[2] * right[0] - look[0] * right[2],
                          look[0] * right[1] - look[1] * right[0]};
    m[0][0] = right[0];
    m[0][1] = right[1];
    m[0][2] = right[2];
    m[0][3] = -(eye[0] * right[0] + eye[1] * right[1] + eye[2] * right[2]);
    m[1][0] = camUp[0];
    m[1][1] = camUp[1];
    m[1][2] = camUp[2];
    m[1][3] = -(eye[0] * camUp[0] + eye[1] * camUp[1] + eye[2] * camUp[2]);
    m[2][0] = look[0];
    m[2][1] = look[1];
    m[2][2] = look[2];
    m[2][3] = -(eye[0] * look[0] + eye[1] * look[1] + eye[2] * look[2]);
}

void mtxMultVec(const Mtx m, const f32 in[3], f32 out[3]) {
    const f32 x = in[0], y = in[1], z = in[2];
    out[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    out[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    out[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
}

void mtxToMtx44(const Mtx src, Mtx44 dst) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            dst[r][c] = src[r][c];
        }
    }
    dst[3][0] = dst[3][1] = dst[3][2] = 0.0f;
    dst[3][3] = 1.0f;
}

void mtxProjConcat(const Mtx a, const Mtx44 b, Mtx dst) {
    Mtx tmp;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            tmp[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] +
                        a[r][3] * b[3][c];
        }
    }
    mtxCopy(tmp, dst);
}

void mtxNormalMtx(const Mtx m, Mtx dst) {
    Mtx inv;
    if (!mtxInverse(m, inv)) {
        mtxIdentity(dst);
        return;
    }
    Mtx tmp;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            tmp[r][c] = inv[c][r];
        }
        tmp[r][3] = 0.0f;
    }
    mtxCopy(tmp, dst);
}

void mtxZeroTranslation(Mtx m) {
    m[0][3] = 0.0f;
    m[1][3] = 0.0f;
    m[2][3] = 0.0f;
}

f32 sinShort(s16 v) {
    return std::sin(static_cast<f32>(v) * kShortToRad);
}

f32 cosShort(s16 v) {
    return std::cos(static_cast<f32>(v) * kShortToRad);
}

void getTranslateRotateMtx(const JointTransform& t, Mtx dst) {
    // Verbatim J3DTransform.cpp: R = Rz * Ry * Rx, then the translation.
    const f32 sx = sinShort(t.rotation[0]), cx = cosShort(t.rotation[0]);
    const f32 sy = sinShort(t.rotation[1]), cy = cosShort(t.rotation[1]);
    const f32 sz = sinShort(t.rotation[2]), cz = cosShort(t.rotation[2]);

    dst[2][0] = -sy;
    dst[0][0] = cz * cy;
    dst[1][0] = sz * cy;
    dst[2][1] = cy * sx;
    dst[2][2] = cy * cx;

    f32 cxsz = cx * sz;
    f32 sxcz = sx * cz;
    dst[0][1] = sxcz * sy - cxsz;
    dst[1][2] = cxsz * sy - sxcz;

    cxsz = sx * sz;
    sxcz = cx * cz;
    dst[0][2] = sxcz * sy + cxsz;
    dst[1][1] = cxsz * sy + sxcz;

    dst[0][3] = t.translation[0];
    dst[1][3] = t.translation[1];
    dst[2][3] = t.translation[2];
}

void getTextureMtx(const TexSrt& srt, const f32 center[3], Mtx dst) {
    const f32 sr = sinShort(srt.rotation), cr = cosShort(srt.rotation);
    const f32 cx = srt.scaleX * cr;
    const f32 sx = srt.scaleX * sr;
    const f32 sy = srt.scaleY * sr;
    const f32 cy = srt.scaleY * cr;

    dst[0][0] = cx;
    dst[0][1] = -sx;
    dst[0][2] = (-cx * center[0] + sx * center[1]) + center[0] + srt.transX;

    dst[1][0] = sy;
    dst[1][1] = cy;
    dst[1][2] = (-sy * center[0] - cy * center[1]) + center[1] + srt.transY;

    dst[0][3] = dst[1][3] = dst[2][0] = dst[2][1] = dst[2][3] = 0.0f;
    dst[2][2] = 1.0f;
}

void getTextureMtxOld(const TexSrt& srt, const f32 center[3], Mtx dst) {
    const f32 sr = sinShort(srt.rotation), cr = cosShort(srt.rotation);
    const f32 cx = srt.scaleX * cr;
    const f32 sx = srt.scaleX * sr;
    const f32 sy = srt.scaleY * sr;
    const f32 cy = srt.scaleY * cr;

    dst[0][0] = cx;
    dst[0][1] = -sx;
    dst[0][3] = (-cx * center[0] + sx * center[1]) + center[0] + srt.transX;

    dst[1][0] = sy;
    dst[1][1] = cy;
    dst[1][3] = (-sy * center[0] - cy * center[1]) + center[1] + srt.transY;

    dst[0][2] = dst[1][2] = dst[2][0] = dst[2][1] = dst[2][3] = 0.0f;
    dst[2][2] = 1.0f;
}

void getTextureMtxMaya(const TexSrt& srt, Mtx dst) {
    const f32 sr = sinShort(srt.rotation), cr = cosShort(srt.rotation);
    const f32 tx = srt.transX - 0.5f;
    const f32 ty = srt.transY - 0.5f;

    dst[0][0] = srt.scaleX * cr;
    dst[0][1] = srt.scaleY * sr;
    dst[0][2] = tx * cr - sr * (ty + srt.scaleY) + 0.5f;

    dst[1][0] = -srt.scaleX * sr;
    dst[1][1] = srt.scaleY * cr;
    dst[1][2] = -tx * sr - cr * (ty + srt.scaleY) + 0.5f;

    dst[0][3] = dst[1][3] = dst[2][0] = dst[2][1] = dst[2][3] = 0.0f;
    dst[2][2] = 1.0f;
}

void getTextureMtxMayaOld(const TexSrt& srt, Mtx dst) {
    const f32 sr = sinShort(srt.rotation), cr = cosShort(srt.rotation);
    const f32 tx = srt.transX - 0.5f;
    const f32 ty = srt.transY - 0.5f;

    dst[0][0] = srt.scaleX * cr;
    dst[0][1] = srt.scaleY * sr;
    dst[0][3] = tx * cr - sr * (ty + srt.scaleY) + 0.5f;

    dst[1][0] = -srt.scaleX * sr;
    dst[1][1] = srt.scaleY * cr;
    dst[1][3] = -tx * sr - cr * (ty + srt.scaleY) + 0.5f;

    dst[0][2] = dst[1][2] = dst[2][0] = dst[2][1] = dst[2][3] = 0.0f;
    dst[2][2] = 1.0f;
}

// --- keyframes ---------------------------------------------------------------

f32 hermite(f32 frame, f32 t0, f32 v0, f32 tanOut0, f32 t1, f32 v1, f32 tanIn1) {
    // JMAHermiteInterpolation (JMath.hpp, paired-single asm) — the classic
    // cubic Hermite basis with the tangents scaled by the segment length.
    const f32 len = t1 - t0;
    if (len <= 0.0f) {
        return v0;
    }
    const f32 t = (frame - t0) / len;
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    const f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const f32 h10 = t3 - 2.0f * t2 + t;
    const f32 h01 = -2.0f * t3 + 3.0f * t2;
    const f32 h11 = t3 - t2;
    return h00 * v0 + h10 * tanOut0 * len + h01 * v1 + h11 * tanIn1 * len;
}

namespace {

template <typename T>
f32 evalTrackImpl(const KeyTrack& track, const T* data, size_t dataCount, f32 frame,
                  f32 defaultValue) {
    if (track.count == 0 || data == nullptr) {
        return defaultValue;
    }
    if (track.count == 1) {
        if (static_cast<size_t>(track.index) >= dataCount) {
            return defaultValue;
        }
        return static_cast<f32>(data[track.index]);
    }
    const int stride = (track.tangentType == 0) ? 3 : 4;
    const size_t need = static_cast<size_t>(track.index) +
                        static_cast<size_t>(track.count) * static_cast<size_t>(stride);
    if (need > dataCount) {
        return defaultValue;
    }
    const T* keys = data + track.index;
    const auto keyTime = [&](int i) { return static_cast<f32>(keys[i * stride]); };
    const auto keyValue = [&](int i) { return static_cast<f32>(keys[i * stride + 1]); };
    const auto keyTanIn = [&](int i) { return static_cast<f32>(keys[i * stride + 2]); };
    const auto keyTanOut = [&](int i) {
        return static_cast<f32>(keys[i * stride + (stride == 3 ? 2 : 3)]);
    };

    const int last = static_cast<int>(track.count) - 1;
    if (frame <= keyTime(0)) {
        return keyValue(0);
    }
    if (frame >= keyTime(last)) {
        return keyValue(last);
    }
    // J3DGetKeyFrameInterpolation: binary search for the segment.
    int lo = 0;
    int hi = last;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (frame >= keyTime(mid)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return hermite(frame, keyTime(lo), keyValue(lo), keyTanOut(lo), keyTime(hi), keyValue(hi),
                   keyTanIn(hi));
}

} // namespace

f32 evalTrackF32(const KeyTrack& track, const f32* data, size_t dataCount, f32 frame,
                 f32 defaultValue) {
    return evalTrackImpl<f32>(track, data, dataCount, frame, defaultValue);
}

f32 evalTrackS16(const KeyTrack& track, const s16* data, size_t dataCount, f32 frame) {
    return evalTrackImpl<s16>(track, data, dataCount, frame, 0.0f);
}

void FrameCtrl::init(u8 mode, f32 endFrame) {
    loopMode = mode;
    start = 0.0f;
    end = endFrame;
    frame = 0.0f;
    rate = 1.0f;
}

void FrameCtrl::update() {
    // J3DFrameCtrl::update (compat/jsystem/J3DFrameCtrlCompat.cpp) with the
    // loop point at `start`.
    frame += rate;
    switch (loopMode) {
    case 0: // once
        if (frame < start) {
            frame = start;
            rate = 0.0f;
        }
        if (frame >= end) {
            frame = end - 0.001f;
            rate = 0.0f;
        }
        break;
    case 1: // once and reset
        if (frame < start) {
            frame = start;
            rate = 0.0f;
        }
        if (frame >= end) {
            frame = start;
            rate = 0.0f;
        }
        break;
    case 2: // repeat
        if (end - start <= 0.0f) {
            frame = start;
            break;
        }
        while (frame < start) {
            frame += end - start;
        }
        while (frame >= end) {
            frame -= end - start;
        }
        break;
    case 3: // mirror once
        if (frame >= end) {
            frame = end - (frame - end);
            rate = -rate;
        }
        if (frame < start) {
            frame = start - (frame - start);
            rate = 0.0f;
        }
        break;
    case 4: // mirror repeat
    default:
        if (frame >= end - 1.0f) {
            frame = (end - 1.0f) - (frame - (end - 1.0f));
            rate = -rate;
        }
        if (frame < start) {
            frame = start - (frame - start);
            rate = -rate;
        }
        break;
    }
}

} // namespace compat::j3d
