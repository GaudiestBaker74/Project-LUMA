#pragma once
// =============================================================================
// PC_PORT PATCH — override of the vendored JSystem/JUtility/JUTAssert.hpp
// (this directory is FIRST in the include path; see compat/include/README.md).
//
// The vendored header declares varargs functions with the Metrowerks
// `__va_list` typedef (not a host type). We provide it (glibc uses
// __builtin_va_list) and then include the vendored header unchanged.
// =============================================================================
#include <cstdarg>
#if defined(_MSC_VER)
// PC_PORT: MSVC has no __builtin_va_list; va_list (from <cstdarg>) is already
// the host type (char* on x64), so the alias is direct.
typedef va_list __va_list;
#else
typedef __builtin_va_list __va_list;
#endif
#include "../../../../../third_party/petari/libs/JSystem/include/JSystem/JUtility/JUTAssert.hpp"
