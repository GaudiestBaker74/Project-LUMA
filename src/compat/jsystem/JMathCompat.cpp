// =============================================================================
// Host implementations of the JMath / JMathInlineVEC functions that the
// vendored JSystem headers declare for non-Metrowerks toolchains but that are
// normally provided by the PPC build's asm implementations (JMath.cpp is
// mostly __MWERKS__-only asm and is not compiled on the host).
//
// Only the functions actually referenced by host-compiled code are defined
// here; add more when a link error demands it. All are trivial scalar
// equivalents of the PS paired-single operations.
//
// The vendored JMATrigonometric.cpp declares the tables (sSinCosTable,
// sAtanTable, sAsinAcosTable) but the decomp has NOT reconstructed their
// baked data, and TAtanTable::atan2_/get_ + TAsinAcosTable::get_ have no
// definition at all. The PC build fills the tables with the mathematically
// correct values at startup and provides scalar implementations of the
// lookup methods (see the init below). When the decomp lands the real data,
// the static init should be removed (or the data applied on top).
// =============================================================================

#include <cmath>

#include <JSystem/JMath/JMath.hpp>
#include <JSystem/JMath/JMATrigonometric.hpp>  // TSinCosTable/TAtanTable/TAsinAcosTable + tables
#include <JSystem/JGeometry/TVec.hpp>  // TVec3<f32> non-inline ctors
#include <revolution/mtx.h>

namespace JMath {

f32 fastReciprocal(f32 value) {
    return 1.0f / value;
}

}  // namespace JMath

// The declarations for these live in the `#else` (non-__MWERKS__) branch of
// JMath.hpp → JMathInlineVEC. C++ linkage.
namespace JMathInlineVEC {

void PSVECCopy(const Vec* src, Vec* dest) {
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
}

void PSVECAdd(const Vec* vec1, const Vec* vec2, Vec* dst) {
    dst->x = vec1->x + vec2->x;
    dst->y = vec1->y + vec2->y;
    dst->z = vec1->z + vec2->z;
}

void PSVECSubtract(const Vec* vec1, const Vec* vec2, Vec* dst) {
    dst->x = vec1->x - vec2->x;
    dst->y = vec1->y - vec2->y;
    dst->z = vec1->z - vec2->z;
}

f32 PSVECDotProduct(const Vec* vec1, const Vec* vec2) {
    return vec1->x * vec2->x + vec1->y * vec2->y + vec1->z * vec2->z;
}

f32 PSVECSquareMag(const Vec* src) {
    // Qualified: the global extern "C" PSVECDotProduct (revolution/mtx.h) is
    // also visible here and would make the call ambiguous.
    return JMathInlineVEC::PSVECDotProduct(src, src);
}

void PSVECNegate(const Vec* src, Vec* dst) {
    dst->x = -src->x;
    dst->y = -src->y;
    dst->z = -src->z;
}

f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
    const Vec d = {a->x - b->x, a->y - b->y, a->z - b->z};
    return JMathInlineVEC::PSVECSquareMag(&d);
}

void PSVECMultiply(const Vec* vec1, const Vec* vec2, Vec* dst) {
    dst->x = vec1->x * vec2->x;
    dst->y = vec1->y * vec2->y;
    dst->z = vec1->z * vec2->z;
}

}  // namespace JMathInlineVEC

namespace JMath {

// The decomp never defines these lookup methods (their PPC versions read the
// baked tables). Scalar host implementations: exact std math for atan2, the
// asin table for get_ — same contract the inline wrappers in
// JMATrigonometric.hpp rely on.
template <s32 Len, typename T>
T TAtanTable<Len, T>::atan2_(T x, T y) const {
    return static_cast<T>(std::atan2(x, y));
}

template <s32 Len, typename T>
T TAtanTable<Len, T>::get_(T x, T y) const {
    return static_cast<T>(std::atan2(x, y));
}

template <s32 Len, typename T>
T TAsinAcosTable<Len, T>::get_(T x, T y) const {
    (void)y;
    if (x >= static_cast<T>(1.0)) {
        return static_cast<T>(0.0);
    }
    if (x <= static_cast<T>(-1.0)) {
        return TAngleConstant_<T>::RADIAN_DEG180();
    }
    if (x < static_cast<T>(0.0)) {
        return mTable[static_cast<u32>(-x * 1023.5f)] + TAngleConstant_<T>::RADIAN_DEG090();
    }
    return TAngleConstant_<T>::RADIAN_DEG090() - mTable[static_cast<u32>(x * 1023.5f)];
}

// The decomp declares the tables but never defines them (the baked data was
// not reconstructed), so the ctors compute their exact mathematical content.
// PC_PORT: filling from a separate TU's static init (the old gJMathTableInit)
// is order-dependent across TUs — MSVC ran the filler BEFORE these ctors,
// which then zeroed the tables again (JMASin/JMACos/JMAAcos returned 0).
// Filling inside each ctor is order-independent.
template <int Bits, typename T>
TSinCosTable<Bits, T>::TSinCosTable() {
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    for (u32 i = 0; i < LEN; ++i) {
        const double a = kTwoPi * static_cast<double>(i) / LEN;
        table[i].a1 = static_cast<T>(std::sin(a));  // sine slot
        table[i].b1 = static_cast<T>(std::cos(a));  // cosine slot
    }
}

template <s32 Len, typename T>
TAtanTable<Len, T>::TAtanTable() : mTable(), _1000() {}

template <s32 Len, typename T>
TAsinAcosTable<Len, T>::TAsinAcosTable() : mTable(), _1000() {
    // acos_(x) computes RADIAN_DEG090 - mTable[(u32)(x * scale)] for x >= 0
    // (scale = Len - 0.5 = 1023.5 for the baked 1024-entry console table),
    // so mTable[k] must equal asin(k / scale).
    const double scale = static_cast<double>(Len) - 0.5;
    for (s32 i = 0; i < Len; ++i) {
        mTable[i] = static_cast<T>(std::asin(static_cast<double>(i) / scale));
    }
}

template class TSinCosTable<14, f32>;
template class TAtanTable<1024, f32>;
template class TAsinAcosTable<1024, f32>;

}  // namespace JMath

// --- JGeometry::TVec3<f32> non-inline ctors -------------------------------
// TVec.hpp declares these (non-Metrowerks branch) without defining them; the
// PPC build inlined them via asm. Component copies.
JGeometry::TVec3<f32>::TVec3(const Vec& vec) {
    x = vec.x;
    y = vec.y;
    z = vec.z;
}

JGeometry::TVec3<f32>::TVec3(const TVec3<f32>& vec) {
    x = vec.x;
    y = vec.y;
    z = vec.z;
}

// --- global PS vector ops (extern "C", declared in revolution/mtx.h) ------
// Scalar host equivalents; the PPC build provided these as paired-single asm.
extern "C" {

f32 PSVECMag(const Vec* src) {
    return std::sqrt(src->x * src->x + src->y * src->y + src->z * src->z);
}

f32 PSVECDotProduct(const Vec* a, const Vec* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* dst) {
    dst->x = a->y * b->z - a->z * b->y;
    dst->y = a->z * b->x - a->x * b->z;
    dst->z = a->x * b->y - a->y * b->x;
}

void PSVECNormalize(const Vec* src, Vec* dst) {
    const f32 mag = PSVECMag(src);
    if (mag > 0.0f) {
        dst->x = src->x / mag;
        dst->y = src->y / mag;
        dst->z = src->z / mag;
    } else {
        dst->x = 0.0f;
        dst->y = 0.0f;
        dst->z = 0.0f;
    }
}

// --- global PS matrix ops (extern "C", declared in revolution/mtx.h) --------
// The vendored RVL_SDK/mtx/mtx.c is pure Paired-Singles assembly (psq_st /
// ps_merge01); only the functions the host-compiled code actually references
// are reimplemented here, scalar and equivalent. Add more when a link error
// demands it (same policy as the PSVEC ops above).

void PSMTXIdentity(Mtx m) {
    m[0][0] = 1.0f;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = 1.0f;
    m[1][2] = 0.0f;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 1.0f;
    m[2][3] = 0.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            dst[r][c] = src[r][c];
        }
    }
}

// M9.5.2: the nw4r lyt pane matrix pipeline (Pane::CalculateMtx) references
// these. Same scalar-reimplementation policy as above; semantics are the RVL
// SDK's row-vector convention (v' = v * M).

void PSMTXConcat(const Mtx a, const Mtx b, Mtx d) {
    // d = a * b (3x4 affine; b's implicit fourth row is (0,0,0,1)).
    // Computed through a temp so d may alias a or b (the console asm is
    // likewise alias-safe).
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
    PSMTXCopy(tmp, d);
}

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) {
    PSMTXIdentity(m);
    m[0][3] = xT;
    m[1][3] = yT;
    m[2][3] = zT;
}

void PSMTXTransApply(const Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) {
    // dst = src * Translate(xT, yT, zT): rotation rows copied, the
    // translation column gets src's linear part applied to (x, y, z).
    f32 t0 = src[0][0] * xT + src[0][1] * yT + src[0][2] * zT + src[0][3];
    f32 t1 = src[1][0] * xT + src[1][1] * yT + src[1][2] * zT + src[1][3];
    f32 t2 = src[2][0] * xT + src[2][1] * yT + src[2][2] * zT + src[2][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            dst[i][j] = src[i][j];
        }
    }
    dst[0][3] = t0;
    dst[1][3] = t1;
    dst[2][3] = t2;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) {
    // Matches the vendored asm: pure scale, zeroed translation column.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = 0.0f;
        }
    }
    m[0][0] = xS;
    m[1][1] = yS;
    m[2][2] = zS;
}

void PSMTXRotRad(Mtx m, char axis, f32 rad) {
    const f32 c = std::cos(rad);
    const f32 s = std::sin(rad);
    PSMTXIdentity(m);
    switch (axis) {
    case 'x':
    case 'X':
        m[1][1] = c;
        m[1][2] = s;
        m[2][1] = -s;
        m[2][2] = c;
        break;
    case 'y':
    case 'Y':
        m[0][0] = c;
        m[0][2] = -s;
        m[2][0] = s;
        m[2][2] = c;
        break;
    case 'z':
    case 'Z':
        m[0][0] = c;
        m[0][1] = s;
        m[1][0] = -s;
        m[1][1] = c;
        break;
    default:
        break;
    }
}

}  // extern "C"
