#pragma once
// =============================================================================
// compat::audio — shared helpers for the M8 audio compat layer.
//
// The console audio ABI is 32-bit: wave-data pointers, FX buffer pointers and
// the AI/DSP address words all travel as u32. On PC, host pointers are 64-bit,
// so the compat layer keeps a registry (token ↔ host pointer) and passes
// 32-bit tokens through the console-shaped APIs. See docs/audio.md
// §"32-bit pointer ABI".
// =============================================================================

#include <cstdint>

namespace compat::audio {

// Registers `ptr` and returns a stable 32-bit token (the token is NOT the
// pointer value; round-tripping is guaranteed while the registry lives).
uint32_t storePtr(void* ptr);

// Resolves a token back to the host pointer (nullptr if unknown).
void* loadPtr(uint32_t token);

// Clears the registry (called at shutdown; invalidates every outstanding
// token). Thread-safe.
void clearPtrs();

} // namespace compat::audio
