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

namespace {

// Fills the JMath lookup tables with their exact mathematical content.
// The PPC game baked these as static data; the decomp has not reconstructed
// them yet, so the host computes them (deterministic, matches the access
// patterns in JMATrigonometric.hpp).
struct JMathTableInit {
    JMathTableInit() {
        constexpr u32 kSinCosLen = JMath::TSinCosTable<14, f32>::LEN;  // 16384
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        JMath::TSinCosTable<14, f32>& sc = JMath::sSinCosTable;
        for (u32 i = 0; i < kSinCosLen; ++i) {
            const double a = kTwoPi * static_cast<double>(i) / kSinCosLen;
            sc.table[i].a1 = static_cast<f32>(std::sin(a));  // sine slot
            sc.table[i].b1 = static_cast<f32>(std::cos(a));  // cosine slot
        }

        // asin lookup table: acos_(x) computes RADIAN_DEG090 - mTable[(u32)(x*1023.5f)]
        // for x >= 0, so mTable[k] must equal asin(k / 1023.5).
        JMath::TAsinAcosTable<1024, f32>& aa = JMath::sAsinAcosTable;
        for (u32 i = 0; i < 1024; ++i) {
            aa.mTable[i] = static_cast<f32>(std::asin(static_cast<double>(i) / 1023.5));
        }
    }
};

JMathTableInit gJMathTableInit;  // runs before main()

}  // namespace

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

// The decomp declares the table constructors but never defines them (the
// baked data was not reconstructed); the tables are filled by the static
// init above, so the ctors just value-initialize.
template <s32 Len, typename T>
TAtanTable<Len, T>::TAtanTable() : mTable(), _1000() {}

template <s32 Len, typename T>
TAsinAcosTable<Len, T>::TAsinAcosTable() : mTable(), _1000() {}

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

}  // extern "C"
