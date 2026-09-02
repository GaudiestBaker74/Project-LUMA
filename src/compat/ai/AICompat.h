#pragma once
// =============================================================================
// compat/ai — the console's audio-interface (AI) registers + DMA, emulated
// over Platform::Audio for the JAS audio driver (M8).
//
// The game's audio never touches the AI registers directly: JASDriver
// (JSystem/JAudio2/JASAiCtrl.cpp, vendored — compiles as-is except the
// pointer-ABI patch) calls AIInit/AIInitDMA/AIStartDMA/AIStopDMA/
// AIRegisterDMACallback/AISetDSPSampleRate. This module implements those APIs
// and adds the PC-side glue the driver needs:
//
//   * a soft-clock ("ai-clock" thread) that fires the registered DMA callback
//     at the console's buffer cadence (one JAS frame per callback) — on the
//     Wii this is the AI DMA "buffer done" interrupt;
//   * a small resampler (AI output rate → the Platform::Audio input rate)
//     because PC sound devices don't run at 32028.5 Hz;
//   * the hooks that hand the freshly mixed frame to Platform::Audio.
//
// Console-faithful parts: the callback contract (one call per DMA buffer,
// from a non-game thread), start/stop semantics, and the sample-rate select.
// See docs/audio.md.
//
// The console API itself (AIInit, AIInitDMA, ...) is declared by
// <revolution/ai.h> with C linkage and defined in AI.cpp at global scope —
// exactly as the vendored JASAiCtrl expects. Only the PC glue lives in
// namespace compat::ai.
// =============================================================================

#include <revolution/ai.h> // AIDCallback + the AIInit*/AI*DMA declarations

#include <cstdint>

namespace compat::ai {

// --- PC-only glue -------------------------------------------------------------

// Initializes the module (must run after Platform::Audio::init).
void init();
void shutdown();

// True when the DMA stream is running (AIStartDMA, not paused/stopped).
bool isStreaming();
bool isInitialized();

// The game-side driver announces its frame geometry so the "interrupt" clock
// can run at the console cadence (JASDriver::getFrameSamples / getDacRate).
void setFrameClock(uint32_t frameSamples, float dacRate);

// Starts/stops the soft-clock (one DMA callback per frame period).
void startClock();
void stopClock();

// Handles one completed AI DMA buffer: resamples `interleaved` (frameSamples
// frames at the DSP output rate) and pushes it to Platform::Audio.
void pushAudioFrame(const int16_t* interleaved, uint32_t frames);

// Called from the Platform::Audio device thread (statistics only).
void onDeviceTick(void* user, int framesRequested);

// The DMA "buffer done" handler — used by the JAS host glue *before* the
// console-style registration path is active; the registered IRQ callback
// (AIRegisterDMACallback) takes precedence on the clock (see docs/audio.md).
using DmaCallback = void (*)(void);
void setDmaHandler(DmaCallback cb);

// Test helpers -----------------------------------------------------------
// Synchronously completes one DMA buffer (as the clock would).
void pumpOnce();

} // namespace compat::ai
