#pragma once
// =============================================================================
// compat::jsystem::jas — host side of the JAS audio driver (M8).
//
// The game's audio driver (JSystem/JAudio2: JASDriver + JASDsp + JASDSPChannel)
// is console-shaped: it expects an AI DMA "buffer done" interrupt per frame
// and DSP subframe syncs, both delivered to a high-priority audio thread
// (JASAudioThread). This module reproduces that thread on a PC thread
// (Platform::Threading) and channels the interrupts from compat/ai
// (soft-clock) and compat/dsp (subframe syncs).
//
// The vendored JASDriver/… .cpp files are compiled unmodified (except the two
// pointer-ABI patches, see patches/README.md); this file provides only the
// symbols that lived in the not-yet-portable JKRThread/JASAudioThread.
// =============================================================================

namespace compat::jsystem {

// One-shot boot of the audio subsystem. `enableDevice` = false keeps the
// pipeline running in Platform::Audio virtual mode (headless/CI). Returns
// false on hard failure.
bool initAudioSystem(bool enableDevice);

// Graceful shutdown: STOP the audio thread, join it, tear down DSP worker /
// AI clock / SDL audio.
void shutdownAudioSystem();

bool isAudioDriverRunning();

} // namespace compat::jsystem
