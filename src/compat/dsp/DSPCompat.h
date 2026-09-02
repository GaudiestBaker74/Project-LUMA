#pragma once
// =============================================================================
// compat/dsp — the console's DSP (audio processor) emulated in software (M8).
//
// The game talks to the DSP through the classic SDK surface: mailboxes
// (CPU→DSP) and the "task" work queue. On the Wii the DSP executes the JAS
// mixer microcode (jdsp); on the PC, `compat::dsp::Mixer` does the same work
// in C++ on a worker thread ("dsp-worker"), after which the JAS descriptors
// (JASDsp::CH_BUF/FX_BUF) mean the exact same thing as on hardware.
//
// Console-shaped functions (DspBoot, DsetupTable, DsyncFrame2, the mailboxes,
// ...) are declared by the vendored JSystem headers the driver uses
// (dsptask.hpp / dspproc.hpp / osdsp_task.hpp) and by <revolution/dsp.h>, and
// are DEFINED in DSP.cpp at global scope with that linkage. This header only
// declares the PC-side glue; see docs/audio.md for the flow.
// =============================================================================

#include <cstdint>

namespace compat::dsp {

// --- PC glue -----------------------------------------------------------------

// When true (tests), a frame sync is executed inline on the calling thread
// instead of being queued to the dsp-worker (deterministic tests).
void setSynchronous(bool sync);
bool isSynchronous();

// The host-side "DSP interrupt" hook: called by the dsp-worker once per
// subframe with the sync mail word (the console's DSP→CPU mail interrupt).
using DspCallback = void (*)(uint32_t mail);
void setDspCallback(DspCallback cb);

// Frame sync with host pointers (used by the patched vendored JAS files — see
// docs/audio.md §"32-bit pointer ABI").
void syncFrameHost(uint32_t subFrames, int16_t* outL, int16_t* outR);

// Starts the dsp-worker thread (created lazily on the first frame sync too).
void startWorker();

// Stops the worker (drains the queue) and joins the thread.
void shutdownDsp();

// Console-shaped verbs kept for tests/tooling (vendored code does not call):
void DSPAssertInt();
int Dsp_Running_Check();
void Dsp_Running_Start();

// The mixer instance (exposed for stats/tests).
class Mixer;
Mixer* mixer();

} // namespace compat::dsp
