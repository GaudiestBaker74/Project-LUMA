#pragma once
// ============================================================================
// PC_PORT PATCH (compat/include override, wins over the vendored headers).
//
// Case shim: the vendored JAudio2 sources include "JSystem/JAudio2/
// JASDSPInterface.hpp" but the vendored header is named JASDspInterface.hpp
// (mixed case). That mismatch is invisible on Windows (case-insensitive
// filesystem) but breaks Linux. This override resolves the uppercase include
// spelling to the real vendored header — via a repo-relative path that cannot
// alias this file on ANY filesystem (on a case-insensitive FS both spellings
// are the same physical file, so a same-directory include would recurse).
// ============================================================================
#include "../../../../../third_party/petari/libs/JSystem/include/JSystem/JAudio2/JASDspInterface.hpp"
