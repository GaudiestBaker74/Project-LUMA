#pragma once
// Title A+B decide sound (SE_SY_GAME_START). The sequenced JAudio2 driver is
// not ported; this loads the original wave the sound table points at
// (bank file AudioRes/Waves/B21kawa_0.aw, metadata in SMR.szs) and plays it
// once through Platform::Audio::playOneShot. It does not synthesize a tone.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace compat::audio {

// One call plays the named system SE once. Only SE_SY_GAME_START is wired.
// Returns false if the original asset is missing or cannot be decoded.
bool playNamedSystemSe(const char* name);

// Decode `seName` from an already-loaded BAA (raw AA_< or Yaz0) plus the
// .aw bytes that BAA names. `stereoOut` is interleaved s16 at `outRate`.
// Used by the runtime path and by the headless test.
bool decodeNamedSystemSe(const uint8_t* baa, size_t baaSize, const uint8_t* aw, size_t awSize, const char* seName,
                         int outRate, std::vector<int16_t>& stereoOut);

}  // namespace compat::audio
