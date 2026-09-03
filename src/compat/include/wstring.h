#pragma once
// ============================================================================
// PC_PORT PATCH (compat/include override, wins over the vendored MSL_C header).
//
// The Metrowerks library header <wstring.h> provides the wide-string helpers.
// nw4r/lyt/lyt_textBox.cpp includes it for wcslen(). On host toolchains these
// live in <cwchar>/<wchar.h>.
//
// Note: the console's wchar_t is 16-bit (MSL default) while host wchar_t is
// 32-bit on Linux / 16-bit on Windows. TextBox wide-string paths are only
// exercised by the game's UTF-16 text; the narrow (char*) paths, which are
// what SMG's layouts use, are unaffected.
// ============================================================================
#ifdef __cplusplus
#include <cwchar>
#else
#include <wchar.h>
#endif
