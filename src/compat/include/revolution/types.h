#ifndef TYPES_H
#define TYPES_H

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Changes vs. the upstream vendored header (third_party/petari/libs/RVL_SDK/
// include/revolution/types.h):
//
// 1. s8..u64 now use fixed-width types (stdint.h). The original uses
//    `signed long`/`unsigned long`, which is 32-bit on the original PPC32
//    toolchain and on Windows (LLP64), but 64-bit on Linux x86-64/aarch64
//    (LP64). Leaving it untouched would silently make s32/u32 64-bit on
//    Linux and corrupt every struct layout in the game.
//
// 2. Removed the `#define nullptr 0` / `#define override` blocks. Those exist
//    so the original (pre-C++11) code compiles on mwcc, but on a modern
//    toolchain `nullptr`/`override` are keywords and macro-redefining them
//    breaks modern C++ code (including our own compat/platform code that
//    includes <revolution.h>).
//
// Everything else is byte-for-byte identical to upstream.
// ============================================================================

#include <stdint.h>

typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;

typedef float f32;
typedef double f64;
typedef volatile f32 vf32;
typedef volatile f64 vf64;

typedef int BOOL;

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

#ifdef __MWERKS__
#define __REGISTER register
#else
#define __REGISTER
#endif

#if !defined(AT_ADDRESS)
#if defined(__MWERKS__)
#define AT_ADDRESS(x)					: x
#else
#define AT_ADDRESS(x)
#endif
#endif

// MetroTRK includes the Revolution SDK but it doesn't support the noinline attribute,
// so we ignore it if we are dealing with MetroTRK but define it for everything else
#if __MWERKS__
#ifndef METRO_TRK
#define ALWAYS_INLINE __attribute__((always_inline))
#define NO_INLINE __attribute__((noinline))
#else
#define ALWAYS_INLINE
#define NO_INLINE
#endif
#else
// PC_PORT: guard against redefinition — JSystem's Inline.hpp defines these
// first (with the real attributes) when both headers are included.
#ifndef ALWAYS_INLINE
#define ALWAYS_INLINE
#endif
#ifndef NO_INLINE
#define NO_INLINE
#endif
#endif

#if __MWERKS__
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#else
#define ATTRIBUTE_ALIGN(num)
#endif

#if __MWERKS__
#define ATTRIBUTE_PACKED __attribute__((packed))
#else
#define ATTRIBUTE_PACKED
#endif

#if __MWERKS__
#define ATTRIBUTE_WEAK __attribute__((weak))
#else
#define ATTRIBUTE_WEAK
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#define ROUND_UP(x, align) (((x) + (align) - 1) & (-(align)))
// PC_PORT: the original casts the pointer to u32, which truncates on 64-bit
// hosts. Use uintptr_t so the macro works on x86-64/aarch64.
#define ROUND_UP_PTR(x, align) ((void*)((((uintptr_t)(x)) + (align) - 1) & (~((uintptr_t)(align) - 1))))

#define ALIGN_PREV(X, N) ((X) & ~((uintptr_t)(N) - 1))
// PC_PORT: upstream is `((X) & ~((N) - 1))`. `~(N - 1)` is a 32-bit mask
// (int/unsigned int), which zero-extends to 64 bits and truncates any 64-bit
// pointer passed as X (the high bits of the result are lost). Casting N to
// uintptr_t keeps the mask 64-bit wide. Correct for u32 values too (their
// high bits are already 0).
#define ALIGN_NEXT(X, N) ALIGN_PREV(((X) + (uintptr_t)(N) - 1), N)

#define ARRAY_SIZE(o) (s32)(sizeof(o) / sizeof(o[0]))
#define ARRAY_SIZEU(o) (sizeof(o) / sizeof(o[0]))

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define FOURCC(c0, c1, c2, c3) (u32)((c0 & 0xFF) << 24 | (c1 & 0xFF) << 16 | (c2 & 0xFF) << 8 | (c3 & 0xFF))

#define IS_ALIGNED(x, align) (((unsigned long)(x) & ((align) - 1)) == 0)
#define IS_NOT_ALIGNED(X, N) (((X) & ((N) - 1)) != 0)

// Comparing a non-volatile reference type to NULL is tautological
// and triggers a warning on modern compilers, but in some cases is
// required to match the original assembly.
#if defined(__MWERKS__) || defined(DECOMPCTX)
#define IS_REF_NULL(r) (&(r) == NULL)
#define IS_REF_NONNULL(r) (&(r) != NULL)
#else
#define IS_REF_NULL(r) (0)
#define IS_REF_NONNULL(r) (1)
#endif

/* just some common intrinsics */

#ifndef __MWERKS__
f32 __frsqrte(f32);
u32 __cntlzw(u32);
s32 __abs(s32);
f32 __fabsf(f32);
void* __memcpy(void*, const void*, int);
#endif

#endif  // TYPES_H
