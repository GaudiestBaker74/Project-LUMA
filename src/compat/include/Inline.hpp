#pragma once

// ============================================================================
// PC_PORT PATCH (compat/include overrides this header for the PC build).
//
// Change vs. upstream:
//   1. `NO_INLINE` upstream is `__attribute__((noinline))` unconditionally —
//      invalid on MSVC (it needs `__declspec(noinline)`).
//   2. The game code uses NO_INLINE POSTFIX (`int f() NO_INLINE {`). MSVC
//      only accepts __declspec at the START of a declaration; postfix
//      `__declspec(noinline)` is a syntax error (C2143/C2059), and postfix
//      `__attribute__` is unknown to MSVC entirely. So on MSVC the macro must
//      expand to NOTHING (losing the noinline hint is harmless — it is an
//      optimization only).
//   3. GCC/Clang also reject postfix attributes on function DEFINITIONS
//      inside class templates ("attributes are not allowed on a
//      function-definition"), and JGeometry/TVec.hpp uses NO_INLINE exactly
//      that way (`void set(...) NO_INLINE { ... }`). So on the PC build the
//      macro is empty on every toolchain; the hint is purely an optimization
//      and does not affect ODR or semantics.
//   4. Guarded with #ifndef so it never redefines the fallback that
//      revolution/types.h may have already set (avoids C4005 and keeps the
//      final definition consistent regardless of include order).
//
// Everything else is identical to upstream.
// ============================================================================

// Macro which is put after the function definition (only in the header) to prevent the function from being inlined.
// Example: void someFunction(int someArg) const NO_INLINE;
#if !defined(NO_INLINE)
#define NO_INLINE
#endif

// Macros which should be used when a function is both inlined and not inlined (so it's auto-inlined by the compiler).
// This is used to declare the inline version of the function. If not a constructor, the non-inline version
// should call the inline version.
// Example:
// int someFunction(int SomeArg)
// inline int INLINE_FUNC_DECL(someFunction, int someArg);
// CALL_INLINE_FUNC(someFunction, 0);
#define INLINE_FUNC_DECL(name, ...) name(void*****, __VA_ARGS__)
#define INLINE_FUNC_DECL_NO_ARG(name) name(void*****)

#define CALL_INLINE_FUNC(name, ...) name((void*****)0, __VA_ARGS__)
#define CALL_INLINE_FUNC_NO_ARG(name) name((void*****)0)
