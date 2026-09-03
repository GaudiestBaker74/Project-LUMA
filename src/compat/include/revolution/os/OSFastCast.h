#ifndef OSFASTCAST_H
#define OSFASTCAST_H
// =============================================================================
// PC_PORT PATCH of the vendored RVL_SDK/include/revolution/os/OSFastCast.h
// (see src/compat/patches/README.md).
//
// Upstream defines OSf32tou16/OSu16tof32 as unconditional GCC PowerPC
// `psq_st`/`psq_l` inline asm and OSInitFastCast as MWERKS paired-single SPR
// setup. Neither exists on x86-64/ARM64 hosts: nw4r::lyt::LytInit() calls
// OSInitFastCast() and nw4r/math/arithmetic.h uses the casts, so compiling
// the nw4r sources fails on the asm.
//
// Change vs. upstream: scalar C equivalents with identical semantics.
//   - psq_st with quantize type 3 (u16) rounds to nearest -> lrintf.
//   - psq_l with quantize type 3 is an exact u16 -> f32 widening.
//   - OSInitFastCast configures GQR registers for those quantize types; the
//     scalar fallback needs no configuration, so it is a no-op.
//
// Everything else is identical to upstream.
// =============================================================================

#include <cmath>

#include "revolution/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OSf32tou16(in, out) *(out) = static_cast<u16>(lrintf(*(in)))
#define OSu16tof32(in, out) *(out) = static_cast<f32>(*(in))

static inline void OSInitFastCast(void) {}

#ifdef __cplusplus
}
#endif

#endif  // OSFASTCAST_H
