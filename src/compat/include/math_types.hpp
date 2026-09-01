#pragma once

// =============================================================================
// PC_PORT host shim — patched copy of third_party/petari/include/math_types.hpp
// (vendored original is untouched).
//
// Changes vs upstream:
//  * Removed the `namespace std { inline f32 atan2(f32 x, f32 y) { return
//    ::atan2(x, y); } }` block. That was a shim for Metrowerks' math library,
//    which lacked std::atan2; libstdc++/MSVC STL already provide
//    std::atan2(f32, f32), so redefining it is a hard error.
// =============================================================================

#include <cmath>
#include <revolution.h>

const f32 HALF_PI = 1.5707964f;
const f64 HALF_PI_D = 1.57079637050628662109375;
const f32 PI = 3.1415927f;
const f32 TWO_PI = 6.2831855f;
const f64 TWO_PI_D = 6.283185482025146;

const f32 PI_180 = 0.017453292f;
const f32 _180_PI = 57.29578f;
const f32 DEGREE_TO_S16 = 182.04445f;

const f32 FLOAT_MAX = 3.4028235e38;
const f32 FLOAT_ZERO = 0.0f;

extern const Vec gZeroVec;
