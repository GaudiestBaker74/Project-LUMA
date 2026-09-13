#pragma once
// =============================================================================
// Platform::CompatInput — internal contract for the KPAD/WPAD compat layer
// (M6). Turns the raw Platform::Input snapshot into KPAD/WPAD semantics for
// the game code, which keeps calling <revolution/kpad.h> / <revolution/wpad.h>
// unchanged. See docs/input.md §2.
// =============================================================================

#include "compat/kpad/InputConfig.h"
#include "platform/Input/Input.h"

#include <cstdint>

namespace Platform::CompatInput {

// Motor sink: the compat layer calls this when the game requests Wiimote
// rumble. The host registers a function forwarding to its Platform::Input
// instance (no-op in headless tests). `gamepadIndex` is the SDL gamepad slot
// (-1 when the channel has no gamepad).
using RumbleSink = void (*)(int gamepadIndex, bool on);
void setRumbleSink(RumbleSink sink);

// One-frame hook: advances all channels (buttons, auto-repeat, stick, pointer,
// accelerometer, connection state) from a raw platform frame. `dt` = seconds
// since the last call (used for button auto-repeat).
void updateFrame(const InputState& state, double dt);

// --- boot-path feed ---------------------------------------------------------
// The demo loop polls SDL itself and calls updateFrame() directly, but the
// real boot (gameMain) never returns to main.cpp: its frame loop lives inside
// the vendored code and the SDL event queue is pumped by compat/vi
// (pumpHostEvents, once per retrace). Registering a raw-device source lets
// that pump feed the KPAD/WPAD layer without consuming events (Input::sample
// is a pure state query), so A+B actually reaches the title sequence.
// Both pointers must outlive the boot (gameMain never returns). No-op pair:
// without a source, pumpFrame() does nothing (headless tests).
void setInputSource(Platform::Input* input, Platform::Window* window);
void pumpFrame();

// Resets all channels and (re)loads the input config (calls KPADInit).
void init();
void shutdown();

// Current per-channel button masks for the compat GamePadUtil replacements
// (testCorePadButtonA/B, testCorePadTriggerAnyWithoutHome). `hold` = buttons
// held this frame; `trig` = this-frame press edges. 0 for invalid channels.
uint32_t getHoldButtons(int chan);
uint32_t getTrigButtons(int chan);

// Latest DPD pointer of a channel in KPAD pos space ([-1,1], y up), without
// draining the KPADRead ring (host-side UI stand-ins need the pointer while
// the vendored WPadHolder keeps consuming the samples). false when the
// channel has no samples yet.
bool getPointerPos(int chan, float* outX, float* outY);

// --- Test hooks -------------------------------------------------------------
// Replaces the config used by updateFrame (bypasses the config file).
void setConfig(const InputConfig& config);
bool channelConnected(int chan);
// The SDL gamepad slot backing a channel (-1 = keyboard/mouse or none).
int channelGamepadIndex(int chan);

} // namespace Platform::CompatInput
