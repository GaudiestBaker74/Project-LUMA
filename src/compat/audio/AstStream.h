#pragma once
// =============================================================================
// compat::audio::AstStream — host playback of the game's streamed music
// (M9.5.4 v7).
//
// SMG's background music is not sequenced: every BGM the game starts through
// MR::startStageBGM("STM_xxx") is a streamed file in /AudioRes/Stream/*.ast.
// On the console the JAudio2 stream player (JAUStdSoundInfo → JASStream, not
// compiled on the host) decodes those files on the DVD thread and mixes them
// through the DSP. The host does not have that path yet, so this module plays
// the .ast directly: a decode thread parses the file (STRM header + BLCK
// blocks, PCM16 big-endian or AFC ADPCM, planar per channel), converts to
// interleaved s16 and pushes into Platform::Audio (SDL device). Loop points of
// the header are honoured (loopEnd → loopStart).
//
// Scope: music only. Sound effects (startSystemSE/startCSSound) need the
// sequenced JAudio2 driver + wave banks and stay stubbed (docs/audio.md).
//
// AST layout (vgmstream ast.c / blocked_ast.c):
//   0x00 "STRM"            0x04 u32 bytes after the 0x40 header
//   0x08 u16 codec         0 = AFC ADPCM, 1 = PCM16BE
//   0x0A u16 bits          16
//   0x0C u16 channels      0x0E u16 loop flag
//   0x10 u32 sample rate   0x14 u32 total samples
//   0x18 u32 loop start    0x1C u32 loop end (samples)
//   0x40 first block: "BLCK", u32 bytes per channel, 0x18 pad, then the
//        channel payloads one after another (planar); next block follows.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <vector>

namespace compat::audio {

struct AstInfo {
    uint16_t codec = 1;      // 0 = AFC, 1 = PCM16BE
    uint16_t channels = 2;
    uint32_t sampleRate = 32000;
    uint32_t numSamples = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    bool looped = false;
};

// Parses the 0x40-byte STRM header. Returns false on a foreign/corrupt file.
bool parseAstHeader(const uint8_t* data, size_t size, AstInfo& out);

// Decodes the whole file into interleaved s16 stereo (mono is duplicated,
// extra channels beyond the first two are dropped). `out` receives
// numSamples*2 values at most. Returns false when the header is unusable.
// Used by the tests and by the small-file path; the player streams block by
// block with the same decoder.
bool decodeAstToStereo(const uint8_t* data, size_t size, AstInfo& info, std::vector<int16_t>& out);

// AFC ADPCM (the Nintendo "AFC" 9-byte frame format used by AST/AW files):
// decodes `frames` frames from `src` into `dst` (16 samples per frame),
// carrying the two history samples in `hist`.
void decodeAfcFrames(const uint8_t* src, size_t frames, int16_t* dst, int16_t hist[2]);

// --- Player ------------------------------------------------------------------
// One music stream at a time (the game only ever has one stage BGM). All
// functions are safe to call from the game thread; the decode thread is
// internal. Files are addressed by the game's absolute virtual path
// ("/AudioRes/Stream/SMG_title_strm.ast") through Platform::Filesystem.

// Maps an SMG stream label ("STM_TITLE") to its file path. Returns false when
// the label is unknown (the caller then tries "/AudioRes/Stream/<label>.ast").
bool streamPathForLabel(const char* label, char* out, size_t outSize);

// Starts (or restarts) the music stream. `fadeInFrames` ramps the volume
// from 0 over that many 60 Hz frames. Returns false when the file cannot be
// opened/parsed (the game keeps running silently).
bool startStream(const char* virtualPath, bool loop, int fadeInFrames = 0);

// Fades the current stream out over `fadeOutFrames` (60 Hz frames; 0 = now)
// and stops it.
void stopStream(int fadeOutFrames);

bool isStreamPlaying();
const char* currentStreamPath();  // "" when idle

// Volume of the music bus (0..1), applied on top of the fades.
void setStreamVolume(float volume);

// Stops the stream and joins the decode thread (process exit / tests).
void shutdownStreams();

} // namespace compat::audio
