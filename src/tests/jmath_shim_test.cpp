// =============================================================================
// M6.5 shim tests: the host replacements for Petari's Metrowerks math headers
// (JGeometry TVec/TUtil, JMath tables, PPC intrinsics, math_types constants).
//
// These headers are the ones that previously blocked compiling the real
// Game/System/WPad*.cpp files on the host (see compat/include/README.md and
// docs/input.md §5.3). The tests pin the semantics the vendored code relies
// on: TVec3 arithmetic, TUtil sqrt/inv_sqrt/clamp, the sin/cos/atan/acos
// lookup tables (filled by JMathCompat.cpp's static init), and the intrinsics
// declared in revolution/types.h.
// =============================================================================

#include "tests/test_runner.h"

#include <JSystem/JGeometry/TMatrix.hpp>
#include <JSystem/JGeometry/TUtil.hpp>
#include <JSystem/JGeometry/TVec.hpp>
#include <JSystem/JMath/JMath.hpp>
#include <JSystem/JMath/JMATrigonometric.hpp>
#include <math_types.hpp>
#include <revolution/types.h>

#include <cmath>
#include <cstdio>

namespace {

constexpr float kTol = 1e-5f;

// --- JGeometry::TVec3<f32> ------------------------------------------------

TEST_CASE(shim_tvec3_arithmetic) {
    JGeometry::TVec3<f32> a(1.0f, 2.0f, 3.0f);
    JGeometry::TVec3<f32> b(4.0f, 5.0f, 6.0f);

    JGeometry::TVec3<f32> sum = a + b;  // JMathInlineVEC::PSVECAdd
    CHECK(std::fabs(sum.x - 5.0f) < kTol);
    CHECK(std::fabs(sum.y - 7.0f) < kTol);
    CHECK(std::fabs(sum.z - 9.0f) < kTol);

    JGeometry::TVec3<f32> diff = a - b;  // subInternal
    CHECK(std::fabs(diff.x + 3.0f) < kTol);
    CHECK(std::fabs(diff.y + 3.0f) < kTol);
    CHECK(std::fabs(diff.z + 3.0f) < kTol);

    JGeometry::TVec3<f32> cross = a.cross(b);
    CHECK(std::fabs(cross.x - (2.0f * 6.0f - 3.0f * 5.0f)) < kTol);  // -3
    CHECK(std::fabs(cross.y - (3.0f * 4.0f - 1.0f * 6.0f)) < kTol);  //  6
    CHECK(std::fabs(cross.z - (1.0f * 5.0f - 2.0f * 4.0f)) < kTol);  // -3

    // dot: the vendored asm body is Metrowerks-only; the shim must not
    // return garbage (x*xb + y*yb + z*zb = 1*4 + 2*5 + 3*6 = 32).
    CHECK(std::fabs(a.dot(b) - 32.0f) < kTol);
    CHECK(std::fabs(a.dot(a) - 14.0f) < kTol);

    // operator=: the vendored asm body is Metrowerks-only; without the shim
    // body assignments would silently do nothing.
    JGeometry::TVec3<f32> assigned;
    assigned = a;
    CHECK(std::fabs(assigned.y - 2.0f) < kTol);
    CHECK(std::fabs(assigned.z - 3.0f) < kTol);

    CHECK(std::fabs(a.squared() - 14.0f) < kTol);
    CHECK(std::fabs(a.length() - std::sqrt(14.0f)) < kTol);

    // scale / negate
    JGeometry::TVec3<f32> s = a;
    s.scale(2.0f);
    CHECK(std::fabs(s.x - 2.0f) < kTol);
    CHECK(std::fabs(s.y - 4.0f) < kTol);
    CHECK(std::fabs(s.z - 6.0f) < kTol);

    JGeometry::TVec3<f32> n = -a;
    CHECK(std::fabs(n.x + 1.0f) < kTol);
    CHECK(std::fabs(n.z + 3.0f) < kTol);

    // set<f32> with 3 args (the syntax that only parsed under Metrowerks)
    JGeometry::TVec3<f32> t;
    t.set<f32>(7.0f, 8.0f, 9.0f);
    CHECK(std::fabs(t.y - 8.0f) < kTol);
}

TEST_CASE(shim_tvec3_normalize_zero) {
    JGeometry::TVec3<f32> v(3.0f, 0.0f, 4.0f);
    v.normalize();
    CHECK(std::fabs(v.x - 0.6f) < kTol);
    CHECK(std::fabs(v.z - 0.8f) < kTol);

    JGeometry::TVec3<f32> z;
    z.zero();
    CHECK(std::fabs(z.x) < kTol);
    CHECK(std::fabs(z.y) < kTol);
    CHECK(std::fabs(z.z) < kTol);
}

// --- JGeometry::TUtil<f32> ------------------------------------------------

TEST_CASE(shim_tutil_sqrt_math) {
    CHECK(std::fabs(JGeometry::TUtil<f32>::sqrt(9.0f) - 3.0f) < kTol);
    CHECK(std::fabs(JGeometry::TUtil<f32>::sqrtInline(2.0f) - std::sqrt(2.0f)) < 1e-4f);
    CHECK(std::fabs(JGeometry::TUtil<f32>::inv_sqrt(4.0f) - 0.5f) < 1e-4f);
    // non-positive inputs return the input (PPC frsqrte contract)
    CHECK(JGeometry::TUtil<f32>::sqrt(-1.0f) == -1.0f);
    CHECK(JGeometry::TUtil<f32>::sqrt(0.0f) == 0.0f);

    CHECK(std::fabs(JGeometry::TUtil<f32>::epsilon() - 32.0f * FLT_EPSILON) < 1e-30f);
    CHECK_EQ(JGeometry::TUtil<f32>::clamp(5.0f, 0.0f, 1.0f), 1.0f);
    CHECK_EQ(JGeometry::TUtil<f32>::clamp(-2.0f, 0.0f, 1.0f), 0.0f);
    CHECK_EQ(JGeometry::TUtil<f32>::clamp(0.5f, 0.0f, 1.0f), 0.5f);
    CHECK_EQ(JGeometry::TUtil<f32>::epsilonEquals(1.0f, 1.0000001f, 0.001f), 1);
    CHECK_EQ(JGeometry::TUtil<f32>::epsilonEquals(1.0f, 2.0f, 0.001f), 0);
}

// --- JMath tables (filled by JMathCompat.cpp's static init) ---------------

TEST_CASE(shim_jmath_trig_tables) {
    // sinShort/cosShort index = u16(v) >> 2 over 16384 entries covering one
    // full turn, i.e. sinShort(v) == sin(v * 2PI / 65536). 8192 = PI/4.
    constexpr s16 kQuarter = 8192;
    const float kPiOver4 = 3.1415927f / 4.0f;
    CHECK(std::fabs(JMASinShort(kQuarter) - std::sin(kPiOver4)) < 1e-3f);
    CHECK(std::fabs(JMACosShort(0) - 1.0f) < 1e-3f);
    CHECK(std::fabs(JMASinShort(-kQuarter) + std::sin(kPiOver4)) < 1e-3f);
    CHECK(std::fabs(JMACosShort(kQuarter) - std::cos(kPiOver4)) < 1e-3f);
    CHECK(std::fabs(JMASinShort(16384) - std::sin(3.1415927f / 2.0f)) < 1e-3f);  // 16384 = PI/2

    CHECK(std::fabs(JMASinRadian(3.1415927f / 6.0f) - 0.5f) < 1e-3f);
    CHECK(std::fabs(JMACosRadian(3.1415927f / 3.0f) - 0.5f) < 1e-3f);
    CHECK(std::fabs(JMASinDegree(90.0f) - 1.0f) < 1e-3f);
    CHECK(std::fabs(JMACosDegree(180.0f) + 1.0f) < 1e-3f);

    // acos table: acos_(0) = PI/2 - asin(0) = PI/2, acos_(1) = 0, acos_(-1) = PI
    CHECK(std::fabs(JMAAcosRadian(0.0f) - 3.1415927f / 2.0f) < 1e-3f);
    CHECK(std::fabs(JMAAcosRadian(1.0f)) < 1e-3f);
    CHECK(std::fabs(JMAAcosRadian(-1.0f) - 3.1415927f) < 1e-3f);
    CHECK(std::fabs(JMAAcosRadian(0.5f) - std::acos(0.5f)) < 1e-3f);

    // JMAATan2 delegates to the (now defined) TAtanTable::atan2_
    CHECK(std::fabs(JMAATan2(1.0f, 1.0f) - 3.1415927f / 4.0f) < 1e-4f);
    CHECK(std::fabs(JMAATan2(0.0f, 1.0f)) < 1e-4f);

    // JMAFastSqrt host body
    CHECK(std::fabs(JMAFastSqrt(16.0f) - 4.0f) < 1e-5f);
    CHECK(JMAFastSqrt(-1.0f) == -1.0f);
}

// --- PPC intrinsics (revolution/types.h, PPCIntrinsics.cpp) --------------

TEST_CASE(shim_ppc_intrinsics) {
    CHECK_EQ(__cntlzw(0u), 32u);
    CHECK_EQ(__cntlzw(1u), 31u);
    CHECK_EQ(__cntlzw(0x80000000u), 0u);
    CHECK_EQ(__cntlzw(0x00FF0000u), 8u);

    CHECK_EQ(__fabsf(-3.5f), 3.5f);
    CHECK_EQ(__abs(-7), 7);
    CHECK_EQ(__abs(7), 7);

    CHECK(std::fabs(__frsqrte(4.0f) - 0.5f) < 1e-4f);
    CHECK(__frsqrte(-1.0f) == -1.0f);  // non-positive passthrough

    // math_types constants
    CHECK(std::fabs(PI - 3.1415927f) < 1e-6f);
    CHECK(std::fabs(TWO_PI - 2.0f * PI) < 1e-5f);
    CHECK(std::fabs(PI_180 * 180.0f - PI) < 1e-4f);
}

}  // namespace
