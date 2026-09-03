#pragma once
// ============================================================================
// PC_PORT PATCH (compat/include override, wins over the vendored MSL_C header).
//
// The Metrowerks library header <extras.h> provides the case-insensitive
// string compares. nw4r/lyt/lyt_arcResourceAccessor.cpp includes it for
// stricmp(). Host toolchains spell these differently (POSIX strcasecmp,
// MSVC _stricmp) and neither name is standard, so provide both spellings
// here rather than pulling in the vendored MSL_C tree.
// ============================================================================
#include <cstring>

#ifdef _WIN32
#include <string.h>  // _stricmp / _strnicmp
#else
#include <strings.h>  // strcasecmp / strncasecmp
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define stricmp _stricmp
#define strnicmp _strnicmp
#else
#define stricmp strcasecmp
#define strnicmp strncasecmp
#endif

#ifdef __cplusplus
}
#endif
