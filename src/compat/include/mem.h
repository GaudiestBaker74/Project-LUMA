#pragma once
// ============================================================================
// PC_PORT PATCH (compat/include override, wins over the vendored headers).
//
// The Metrowerks library header <mem.h> (memcpy/memset/memmove) is included by
// JSystem/JAudio2/JASGadget.hpp. On a host toolchain <cstring> provides the
// same functions; pulling the vendored MSL_C header tree in just for this is
// not worth it.
// ============================================================================
#include <cstring>
