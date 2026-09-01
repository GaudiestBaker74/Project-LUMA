#pragma once
// =============================================================================
// Platform::CompatInput::InputConfig — editable input mapping (M6).
//
// Loaded from config/input.ini (relative to the working directory, or the
// path set with setOverridePath for tests). Format:
//
//   [channel0]
//   source=keyboard_mouse        ; keyboard_mouse | gamepad | none
//   gamepad=0                    ; gamepad slot index for source=gamepad
//   use_classic=false            ; gamepad channels: present as Classic instead of Nunchuk
//   stick=wasd                   ; wasd | arrows (keyboard_mouse channels)
//   bind_a=key:space             ; key:<name> | mouse:left/right/middle | none
//   bind_b=mouse:left
//   bind_c=key:lshift
//   bind_z=mouse:right
//   bind_one=key:q
//   bind_two=key:e
//   bind_plus=key:enter
//   bind_minus=key:backspace
//   bind_home=key:escape
//   bind_up=key:up
//   bind_down=key:down
//   bind_left=key:left
//   bind_right=key:right
//   shake=key:k
//   pointer_sensitivity=1.0
//   sensor_height=0.35
//   shake_strength=3.0
//
// Missing file/sections → built-in defaults (see InputConfig::defaults()).
// =============================================================================

#include "platform/Input/Input.h"

#include <cstdint>

namespace Platform::CompatInput {

// Where a channel's input comes from.
enum class Source : uint8_t { None, KeyboardMouse, Gamepad };

// Wiimote actions that can be bound to a key/mouse button (keyboard_mouse
// channels). Gamepad channels use a fixed mapping (see KPAD.cpp).
enum class WiiAction : uint8_t {
    Up, Down, Left, Right,   // D-pad
    A, B, One, Two, Plus, Minus, Home,
    C, Z,                    // Nunchuk
    Shake,
    Count
};

// Name of a WiiAction ("a", "plus", "shake", ...). Used by the config parser.
const char* wiiActionName(WiiAction action);
WiiAction wiiActionFromName(const char* name);

struct ChannelConfig {
    Source source = Source::KeyboardMouse;
    int gamepadIndex = 0;
    bool useClassic = false;       // gamepad channels: Classic instead of Nunchuk
    bool stickUsesArrows = false;  // keyboard: WASD (false) or arrows (true)
    // Key binding per action (only meaningful for KeyboardMouse channels).
    Platform::Key bind[static_cast<int>(WiiAction::Count)] = {};
    float pointerSensitivity = 1.0f;  // accepted for API fidelity; direct 1:1 pointer
    float sensorHeight = 0.35f;       // KPADSetSensorHeight default
    float shakeStrength = 3.0f;       // g impulse while the shake key is held
};

struct InputConfig {
    ChannelConfig channels[4];  // index = Wiimote channel (WPAD_CHAN0..3)

    // Built-in defaults: channel 0 = keyboard+mouse, channels 1-3 = the first
    // three SDL gamepads (if present).
    static InputConfig defaults();
    // Reads config/input.ini (or the override path). Never fails: missing
    // keys fall back to defaults().
    static InputConfig load();
    // Test hook: reads from `path` instead of config/input.ini.
    static void setOverridePath(const char* path);
};

} // namespace Platform::CompatInput
