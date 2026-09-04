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

// Resets all channels and (re)loads the input config (calls KPADInit).
void init();
void shutdown();

// Current per-channel button masks for the compat GamePadUtil replacements
// (testCorePadButtonA/B, testCorePadTriggerAnyWithoutHome). `hold` = buttons
// held this frame; `trig` = this-frame press edges. 0 for invalid channels.
uint32_t getHoldButtons(int chan);
uint32_t getTrigButtons(int chan);

// --- Test hooks -------------------------------------------------------------
// Replaces the config used by updateFrame (bypasses the config file).
void setConfig(const InputConfig& config);
bool channelConnected(int chan);
// The SDL gamepad slot backing a channel (-1 = keyboard/mouse or none).
int channelGamepadIndex(int chan);

} // namespace Platform::CompatInput
