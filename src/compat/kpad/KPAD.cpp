// =============================================================================
// Platform::CompatInput — KPAD layer (M6).
//
// Implements the KPAD API the game calls (<revolution/kpad.h>) on top of the
// raw Platform::Input snapshot. Semantics replicated from the vendored
// KPAD.c (docs/input.md §2):
//   - hold/trig/release edges per channel;
//   - auto-repeat via KPADSetBtnRepeat → KPAD_BUTTON_RPT in `hold`;
//   - nunchuk stick in ex_status.fs.stick ([-1,1], y up, diagonal normalized);
//   - DPD pointer: mouse → pos normalized to [-1,1] (KPAD pos space, see
//     WPadPointer::getPointingPosBasedOnScreen), dpd_valid_fg ramp 0→3;
//   - accelerometer synthesized: rest (0,1,0) (upright), shake impulse on the
//     bound shake key (no real IMU on PC).
//
// The one-frame hook is Platform::CompatInput::updateFrame, called by the
// host loop after Platform::Input::poll; KPADRead drains the samples it
// produces (newest first, index 0 = current frame).
//
// Linkage: kpad.h declares the KPAD API inside `extern "C"`, so the public
// functions are defined at global scope with C linkage; the internal state
// lives in a file-local anonymous namespace.
// =============================================================================

#include "compat/kpad/KPADCompat.h"
#include "compat/kpad/WPADInternal.h"
#include "platform/Timing/Timing.h"

#include <revolution/kpad.h>
#include <revolution/wpad.h>

#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// File-local state (visible to both the extern "C" API and CompatInput).
// ---------------------------------------------------------------------------
namespace {

constexpr int kChannels = WPAD_MAX_CONTROLLERS;  // 4
constexpr int kRingSize = 120;                   // KPAD_STATUS_ARRAY_SIZE

struct KpadChannel {
    bool initialized = false;

    // Connection (mirrors the WPAD layer's connected flag).
    bool prevConnected = false;
    uint32_t devType = WPAD_DEV_FREESTYLE;

    // Buttons.
    uint32_t hold = 0;
    uint32_t prevHold = 0;
    uint32_t trig = 0;
    uint32_t release = 0;

    // Integrated pointer position ([-1,1]) for merged channels: the mouse
    // sets it absolutely when it moves, the sticks steer it otherwise.
    float integX = 0.0f;
    float integY = 0.0f;
    float prevMouseNx = 0.0f;
    float prevMouseNy = 0.0f;
    bool havePrevMouse = false;

    // Auto-repeat (KPADSetBtnRepeat), in seconds.
    double delaySec = 1.0 / 2.4;
    double pulseSec = 1.0 / 6.0;
    double repeatAcc = 0.0;
    bool repeatFired = false;

    // Pointer calibration (KPADSet*Param). Accepted for API fidelity; the PC
    // pointer is a direct 1:1 mouse mapping (the KPAD smoothing these knobs
    // control has no hardware input to smooth).
    float posRadius = 0.03f;
    float posSensitivity = 0.5f;
    float horiRadius = 0.0f;
    float horiSensitivity = 1.0f;
    float distRadius = 0.0f;
    float distSensitivity = 1.0f;
    float sensorHeight = 0.35f;
    int dpdValid = 0;

    // PC_PORT (M10.1): "menu decide" — the pointer's confirm click.
    //
    // On the console the menus are driven by the star pointer: A decides, B
    // cancels, and MR::testDPDMenuPadDecideTrigger()/testSystemTriggerA() see
    // the Wiimote's A. On the PC the natural gesture is CLICKING with the
    // mouse, but the default config binds mouse-left to B (the gameplay
    // mapping: B = the star bits) — so clicking a menu entry played the
    // *cancel* path and the FileSelect screen looked unclickable (M10.1 bug
    // report: "no funcionan los clics").
    //
    // This records the raw mouse-left press edge of the frame, before any
    // binding, so menu screens can read it as A (getMenuDecideTrigger below)
    // while gameplay keeps its own mapping.
    bool mouseLeftTrig = false;
    bool mouseRightTrig = false;
    // Edge state for the stick-driven menu navigation (getMenuNav).
    int prevMenuNavStick = 0;

    // Previous raw mouse-button states, for the edges above.
    bool prevMouseLeft = false;
    bool prevMouseRight = false;

    // Synthesized shake (frames of impulse left).
    double shakeFrames = 0.0;

    // Sample ring (newest at ringWrite-1); unconsumed = not yet read.
    KPADStatus ring[kRingSize];
    int ringWrite = 0;
    int unconsumed = 0;
};

KpadChannel gChannels[kChannels];
Platform::CompatInput::InputConfig gConfig;
// setConfig() pins the config so a later init() (KPADInit) does not clobber it
// with a disk reload. The app's startup init() still loads config/input.ini.
bool gConfigExplicit = false;

uint32_t actionBit(Platform::CompatInput::WiiAction action) {
    using Platform::CompatInput::WiiAction;
    switch (action) {
        case WiiAction::Up: return WPAD_BUTTON_UP;
        case WiiAction::Down: return WPAD_BUTTON_DOWN;
        case WiiAction::Left: return WPAD_BUTTON_LEFT;
        case WiiAction::Right: return WPAD_BUTTON_RIGHT;
        case WiiAction::A: return WPAD_BUTTON_A;
        case WiiAction::B: return WPAD_BUTTON_B;
        case WiiAction::One: return WPAD_BUTTON_1;
        case WiiAction::Two: return WPAD_BUTTON_2;
        case WiiAction::Plus: return WPAD_BUTTON_PLUS;
        case WiiAction::Minus: return WPAD_BUTTON_MINUS;
        case WiiAction::Home: return WPAD_BUTTON_HOME;
        case WiiAction::C: return WPAD_BUTTON_C;
        case WiiAction::Z: return WPAD_BUTTON_Z;
        default: return 0;
    }
}

// Whether the channel's configured source is present this frame.
bool sourcePresent(int chan, const Platform::InputState& state) {
    const Platform::CompatInput::ChannelConfig& cfg = gConfig.channels[chan];
    switch (cfg.source) {
        case Platform::CompatInput::Source::None: return false;
        case Platform::CompatInput::Source::KeyboardMouse: return true;
        case Platform::CompatInput::Source::KeyboardMouseGamepad: return true;
        case Platform::CompatInput::Source::Gamepad:
            if (cfg.gamepadIndex < 0 || cfg.gamepadIndex >= Platform::kMaxGamepads) {
                return false;
            }
            return state.gamepads[cfg.gamepadIndex].connected;
    }
    return false;
}

// Wiimote/nunchuk button mask for a keyboard_mouse channel.
uint32_t computeKeyboardButtons(const Platform::CompatInput::ChannelConfig& cfg,
                                const Platform::InputState& state) {
    uint32_t mask = 0;
    for (int i = 0; i < static_cast<int>(Platform::CompatInput::WiiAction::Count); ++i) {
        const Platform::CompatInput::WiiAction action = static_cast<Platform::CompatInput::WiiAction>(i);
        const Platform::Key key = cfg.bind[i];
        if (key == Platform::Key::None) {
            continue;
        }
        if (state.keys[static_cast<int>(key)]) {
            mask |= actionBit(action);
        }
    }
    return mask;
}

// Wiimote/nunchuk button mask for a gamepad channel (fixed mapping).
uint32_t computeGamepadButtons(const Platform::CompatInput::ChannelConfig& cfg,
                               const Platform::InputState& state) {
    if (cfg.gamepadIndex < 0 || cfg.gamepadIndex >= Platform::kMaxGamepads) {
        return 0;
    }
    const Platform::GamepadState& g = state.gamepads[cfg.gamepadIndex];
    uint32_t mask = 0;
    if (g.dpadUp) mask |= WPAD_BUTTON_UP;
    if (g.dpadDown) mask |= WPAD_BUTTON_DOWN;
    if (g.dpadLeft) mask |= WPAD_BUTTON_LEFT;
    if (g.dpadRight) mask |= WPAD_BUTTON_RIGHT;
    if (g.a) mask |= WPAD_BUTTON_A;
    if (g.b) mask |= WPAD_BUTTON_B;
    if (g.x) mask |= WPAD_BUTTON_C;   // Nunchuk C
    if (g.y) mask |= WPAD_BUTTON_Z;   // Nunchuk Z
    if (g.start) mask |= WPAD_BUTTON_PLUS;
    if (g.back) mask |= WPAD_BUTTON_MINUS;
    if (g.guide) mask |= WPAD_BUTTON_HOME;
    if (g.leftShoulder) mask |= WPAD_BUTTON_1;
    if (g.rightShoulder) mask |= WPAD_BUTTON_2;
    return mask;
}

// Classic-controller mask (ex_status.cl) for a gamepad channel.
uint32_t computeClassicButtons(const Platform::CompatInput::ChannelConfig& cfg,
                               const Platform::InputState& state) {
    if (cfg.gamepadIndex < 0 || cfg.gamepadIndex >= Platform::kMaxGamepads) {
        return 0;
    }
    const Platform::GamepadState& g = state.gamepads[cfg.gamepadIndex];
    uint32_t mask = 0;
    if (g.dpadUp) mask |= WPAD_CL_BUTTON_UP;
    if (g.dpadDown) mask |= WPAD_CL_BUTTON_DOWN;
    if (g.dpadLeft) mask |= WPAD_CL_BUTTON_LEFT;
    if (g.dpadRight) mask |= WPAD_CL_BUTTON_RIGHT;
    if (g.a) mask |= WPAD_CL_BUTTON_A;
    if (g.b) mask |= WPAD_CL_BUTTON_B;
    if (g.x) mask |= WPAD_CL_BUTTON_X;
    if (g.y) mask |= WPAD_CL_BUTTON_Y;
    if (g.start) mask |= WPAD_CL_BUTTON_PLUS;
    if (g.back) mask |= WPAD_CL_BUTTON_MINUS;
    if (g.guide) mask |= WPAD_CL_BUTTON_HOME;
    return mask;
}

// Stick in [-1,1] (y up), diagonal normalized.
Vec2 computeStick(int chan, const Platform::InputState& state) {
    Vec2 s{0.0f, 0.0f};
    const Platform::CompatInput::ChannelConfig& cfg = gConfig.channels[chan];
    if (cfg.source == Platform::CompatInput::Source::KeyboardMouse ||
        cfg.source == Platform::CompatInput::Source::KeyboardMouseGamepad) {
        const bool up = state.keys[static_cast<int>(cfg.stickUsesArrows ? Platform::Key::Up : Platform::Key::W)];
        const bool down = state.keys[static_cast<int>(cfg.stickUsesArrows ? Platform::Key::Down : Platform::Key::S)];
        const bool left = state.keys[static_cast<int>(cfg.stickUsesArrows ? Platform::Key::Left : Platform::Key::A)];
        const bool right = state.keys[static_cast<int>(cfg.stickUsesArrows ? Platform::Key::Right : Platform::Key::D)];
        s.x = (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
        s.y = (up ? 1.0f : 0.0f) - (down ? 1.0f : 0.0f);
        if (s.x != 0.0f && s.y != 0.0f) {
            s.x *= 0.70710678f;  // normalize diagonals
            s.y *= 0.70710678f;
        }
    }
    if (cfg.source == Platform::CompatInput::Source::KeyboardMouseGamepad &&
        cfg.gamepadIndex >= 0 && cfg.gamepadIndex < Platform::kMaxGamepads) {
        const Platform::GamepadState& g = state.gamepads[cfg.gamepadIndex];
        if (std::fabs(g.leftX) > std::fabs(s.x)) {
            s.x = g.leftX;
        }
        if (std::fabs(-g.leftY) > std::fabs(s.y)) {
            s.y = -g.leftY;  // SDL: +Y down; KPAD nunchuk: +Y up
        }
    } else if (cfg.source == Platform::CompatInput::Source::Gamepad &&
               cfg.gamepadIndex >= 0 && cfg.gamepadIndex < Platform::kMaxGamepads) {
        const Platform::GamepadState& g = state.gamepads[cfg.gamepadIndex];
        s.x = g.leftX;
        s.y = -g.leftY;  // SDL: +Y down; KPAD nunchuk: +Y up
    }
    return s;
}

// Synthesized accelerometer + shake. Rest = upright Wiimote (specific force
// +1g on Y; the game's WPadAcceleration negates it to get the gravity vector).
void computeAccel(int chan, bool shakeHeld, Vec& acc, float& accValue, float& accSpeed) {
    acc.x = 0.0f;
    acc.y = 1.0f;
    acc.z = 0.0f;
    accValue = 1.0f;
    accSpeed = 0.0f;

    const Platform::CompatInput::ChannelConfig& cfg = gConfig.channels[chan];
    KpadChannel& c = gChannels[chan];
    if (shakeHeld) {
        c.shakeFrames = 6.0;  // ~0.1 s at 60 Hz
    }
    if (c.shakeFrames > 0.0) {
        c.shakeFrames -= 1.0;
        acc.x += cfg.shakeStrength;  // horizontal impulse
        accValue = std::sqrt(acc.x * acc.x + 1.0f);
        accSpeed = c.shakeFrames > 3.0 ? 8.0f : 0.0f;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// CompatInput hook (the host calls this once per frame).
// ---------------------------------------------------------------------------
namespace Platform::CompatInput {

void updateFrame(const InputState& state, double dt) {
    for (int chan = 0; chan < kChannels; ++chan) {
        KpadChannel& c = gChannels[chan];
        if (!c.initialized) {
            continue;  // KPADInit not called yet
        }
        const ChannelConfig& cfg = gConfig.channels[chan];

        // --- connection -----------------------------------------------------
        const bool present = sourcePresent(chan, state);
        if (!present) {
            if (c.prevConnected) {
                c.prevConnected = false;
                wpadNotifyDisconnect(chan);
            }
            continue;
        }
        c.devType = cfg.useClassic ? static_cast<uint32_t>(WPAD_DEV_CLASSIC)
                                   : static_cast<uint32_t>(WPAD_DEV_FREESTYLE);
        if (!c.prevConnected) {
            c.prevConnected = true;
            wpadNotifyConnect(chan, c.devType);
        }

        // --- buttons --------------------------------------------------------
        uint32_t newHold = 0u;
        if (cfg.source == Source::Gamepad) {
            newHold = cfg.useClassic ? 0u : computeGamepadButtons(cfg, state);
        } else {
            newHold = computeKeyboardButtons(cfg, state);
            if (cfg.source == Source::KeyboardMouseGamepad) {
                // Union with the pad on the same Wii channel (0 when the pad
                // is absent: computeGamepadButtons reads disconnected=false).
                newHold |= computeGamepadButtons(cfg, state);
            }
        }
        c.trig = newHold & ~c.prevHold;
        c.release = c.prevHold & ~newHold;
        c.hold = newHold;
        c.prevHold = newHold;

        // PC_PORT (M10.1): raw pointer-button edges for the menus, taken from
        // the platform frame (not from the bindings — see mouseLeftTrig).
        {
            const bool hasMouse = cfg.source == Source::KeyboardMouse ||
                                  cfg.source == Source::KeyboardMouseGamepad;
            const bool left = hasMouse && state.keys[static_cast<int>(Platform::Key::MouseLeft)];
            const bool right = hasMouse && state.keys[static_cast<int>(Platform::Key::MouseRight)];
            c.mouseLeftTrig = left && !c.prevMouseLeft;
            c.mouseRightTrig = right && !c.prevMouseRight;
            c.prevMouseLeft = left;
            c.prevMouseRight = right;
        }

        // Auto-repeat: while any button is held, after `delay` the
        // KPAD_BUTTON_RPT flag stays set (pulse cadence `pulse`). RPT only in
        // `hold` (the game's WPadButton derives repeats from it; KPAD_BUTTON_MASK
        // keeps it out of trig).
        if (newHold != 0) {
            c.repeatAcc += dt;
            if (!c.repeatFired && c.repeatAcc >= c.delaySec) {
                c.repeatFired = true;
                c.repeatAcc = 0.0;
            } else if (c.repeatFired && c.repeatAcc >= c.pulseSec) {
                c.repeatAcc = 0.0;
            }
            if (c.repeatFired) {
                c.hold |= KPAD_BUTTON_RPT;
            }
        } else {
            c.repeatFired = false;
            c.repeatAcc = 0.0;
        }

        // --- stick / pointer / accel ----------------------------------------
        const Vec2 stick = computeStick(chan, state);

        Vec2 pos{0.0f, 0.0f};
        int dpdValid = 0;
        if (cfg.source == Source::KeyboardMouse) {
            pos.x = state.mouseNx * 2.0f - 1.0f;
            pos.y = state.mouseNy * 2.0f - 1.0f;
            c.dpdValid = state.mouseInWindow ? (c.dpdValid + 1 > 3 ? 3 : c.dpdValid + 1) : 0;
            dpdValid = c.dpdValid;
        } else if (cfg.source == Source::KeyboardMouseGamepad) {
            // The mouse owns the pointer when it moves; otherwise the sticks
            // steer it gyro-style (right stick first, left as fallback) so a
            // gamepad-only player can reach the menus and the FileSelect
            // cursor.
            const float mx = state.mouseNx * 2.0f - 1.0f;
            const float my = state.mouseNy * 2.0f - 1.0f;
            const bool mouseMoved =
                !c.havePrevMouse || mx != c.prevMouseNx || my != c.prevMouseNy;
            c.prevMouseNx = mx;
            c.prevMouseNy = my;
            c.havePrevMouse = true;
            if (mouseMoved) {
                c.integX = mx;
                c.integY = my;
            } else if (cfg.gamepadIndex >= 0 && cfg.gamepadIndex < Platform::kMaxGamepads) {
                const Platform::GamepadState& g = state.gamepads[cfg.gamepadIndex];
                float sx = g.rightX;
                float sy = -g.rightY;
                if (sx == 0.0f && sy == 0.0f) {
                    sx = g.leftX;
                    sy = -g.leftY;
                }
                if (sx != 0.0f || sy != 0.0f) {
                    c.integX += sx * 1.8f * static_cast<float>(dt);
                    c.integY += sy * 1.8f * static_cast<float>(dt);
                    if (c.integX < -1.0f) c.integX = -1.0f;
                    if (c.integX > 1.0f) c.integX = 1.0f;
                    if (c.integY < -1.0f) c.integY = -1.0f;
                    if (c.integY > 1.0f) c.integY = 1.0f;
                }
            }
            pos.x = c.integX;
            pos.y = c.integY;
            c.dpdValid = c.dpdValid + 1 > 3 ? 3 : c.dpdValid + 1;
            dpdValid = c.dpdValid;
        } else {
            c.dpdValid = 0;
        }

        bool shakeHeld = false;
        if (cfg.source == Source::KeyboardMouse || cfg.source == Source::KeyboardMouseGamepad) {
            const Platform::Key shakeKey = cfg.bind[static_cast<int>(WiiAction::Shake)];
            shakeHeld = shakeKey != Platform::Key::None && state.keys[static_cast<int>(shakeKey)];
        }
        if (cfg.source == Source::Gamepad || cfg.source == Source::KeyboardMouseGamepad) {
            shakeHeld = shakeHeld ||
                        (cfg.gamepadIndex >= 0 && cfg.gamepadIndex < Platform::kMaxGamepads &&
                         state.gamepads[cfg.gamepadIndex].misc1);
        }

        Vec acc{0.0f, 0.0f, 0.0f};
        float accValue = 1.0f;
        float accSpeed = 0.0f;
        computeAccel(chan, shakeHeld, acc, accValue, accSpeed);

        // --- sample ---------------------------------------------------------
        KPADStatus s;
        std::memset(&s, 0, sizeof(s));
        s.hold = c.hold;
        s.trig = c.trig;
        s.release = c.release;
        s.acc = acc;
        s.acc_value = accValue;
        s.acc_speed = accSpeed;
        s.pos = pos;
        s.vec = {0.0f, 0.0f};
        s.speed = 0.0f;
        s.horizon = {0.0f, 0.0f};
        s.hori_vec = {0.0f, 0.0f};
        s.hori_speed = 0.0f;
        s.dist = 1.0f;  // normalized max distance (pointer at the screen)
        s.dist_vec = 0.0f;
        s.dist_speed = 0.0f;
        s.acc_vertical = {0.0f, 0.0f};
        s.dev_type = static_cast<uint8_t>(c.devType);
        s.wpad_err = WPAD_ERR_NONE;
        s.dpd_valid_fg = static_cast<int8_t>(dpdValid);
        s.data_format = 0;
        if (cfg.useClassic) {
            s.ex_status.cl.hold = computeClassicButtons(cfg, state);
            s.ex_status.cl.lstick = stick;
            if (cfg.gamepadIndex >= 0 && cfg.gamepadIndex < Platform::kMaxGamepads) {
                const GamepadState& g = state.gamepads[cfg.gamepadIndex];
                s.ex_status.cl.rstick = {g.rightX, -g.rightY};
                s.ex_status.cl.ltrigger = g.leftTrigger;
                s.ex_status.cl.rtrigger = g.rightTrigger;
            }
        } else {
            s.ex_status.fs.stick = stick;
            s.ex_status.fs.acc = acc;
            s.ex_status.fs.acc_value = accValue;
            s.ex_status.fs.acc_speed = accSpeed;
        }

        c.ring[c.ringWrite] = s;
        c.ringWrite = (c.ringWrite + 1) % kRingSize;
        if (c.unconsumed < kRingSize) {
            ++c.unconsumed;
        }
    }
}

void init() { KPADInit(); }
void shutdown() {}

// --- boot-path feed (see KPADCompat.h) ---------------------------------------
namespace {
    Platform::Input* sInputSource = nullptr;
    Platform::Window* sWindowSource = nullptr;
    Platform::Timing::TimePoint sLastPump{};
    bool sHaveLastPump = false;
}  // namespace

bool getPointerPos(int chan, float* outX, float* outY) {
    if (chan < 0 || chan >= kChannels || outX == nullptr || outY == nullptr) {
        return false;
    }
    const KpadChannel& c = gChannels[chan];
    if (c.ringWrite == 0 && c.unconsumed == 0) {
        return false;
    }
    const int last = (c.ringWrite + kRingSize - 1) % kRingSize;
    *outX = c.ring[last].pos.x;
    *outY = c.ring[last].pos.y;
    return true;
}

void setInputSource(Platform::Input* input, Platform::Window* window) {
    sInputSource = input;
    sWindowSource = window;
    sHaveLastPump = false;
}

void pumpFrame() {
    if (sInputSource == nullptr || sWindowSource == nullptr) {
        return;  // headless / demo path: updateFrame is called explicitly
    }

    const Platform::Timing::TimePoint now = Platform::Timing::now();
    double dt = 1.0 / 60.0;
    if (sHaveLastPump) {
        dt = Platform::Timing::secondsBetween(sLastPump, now);
        if (dt < 0.0) {
            dt = 0.0;
        }
        if (dt > 0.25) {
            dt = 0.25;  // same clamp the demo loop uses after a hitch
        }
    }
    sLastPump = now;
    sHaveLastPump = true;

    updateFrame(sInputSource->sample(sWindowSource->width(), sWindowSource->height()), dt);
}

void setConfig(const InputConfig& config) {
    gConfig = config;
    gConfigExplicit = true;
}

bool channelConnected(int chan) {
    if (chan < 0 || chan >= kChannels || !gChannels[chan].initialized) {
        return false;
    }
    return gChannels[chan].prevConnected;
}

int channelGamepadIndex(int chan) {
    if (chan < 0 || chan >= kChannels) {
        return -1;
    }
    const ChannelConfig& cfg = gConfig.channels[chan];
    return cfg.source == Source::Gamepad ? cfg.gamepadIndex : -1;
}

void setRumbleSink(RumbleSink sink) { wpadSetRumbleSink(sink); }

} // namespace Platform::CompatInput

// ---------------------------------------------------------------------------
// KPAD API (declared in <revolution/kpad.h>, extern "C").
// ---------------------------------------------------------------------------
extern "C" {

void KPADInit() {
    if (!gConfigExplicit) {
        gConfig = Platform::CompatInput::InputConfig::load();
    }
    for (int chan = 0; chan < kChannels; ++chan) {
        KpadChannel& c = gChannels[chan];
        c.initialized = true;
        c.prevConnected = false;
        c.hold = 0;
        c.prevHold = 0;
        c.trig = 0;
        c.release = 0;
        c.repeatAcc = 0.0;
        c.repeatFired = false;
        c.dpdValid = 0;
        c.shakeFrames = 0.0;
        c.ringWrite = 0;
        c.unconsumed = 0;
        std::memset(c.ring, 0, sizeof(c.ring));
        // Defaults mirror the SDK (WPadPointer::reset re-sets these via the
        // KPADSet*Param calls).
        c.posRadius = 0.03f;
        c.posSensitivity = 0.5f;
        c.horiRadius = 0.0f;
        c.horiSensitivity = 1.0f;
        c.distRadius = 0.0f;
        c.distSensitivity = 1.0f;
        c.sensorHeight = 0.35f;
        c.delaySec = 1.0 / 2.4;
        c.pulseSec = 1.0 / 6.0;
    }
}

void KPADReset() {
    for (int chan = 0; chan < kChannels; ++chan) {
        KpadChannel& c = gChannels[chan];
        if (!c.initialized) {
            continue;
        }
        c.prevConnected = false;
        c.hold = 0;
        c.prevHold = 0;
        c.trig = 0;
        c.release = 0;
        c.repeatAcc = 0.0;
        c.repeatFired = false;
        c.dpdValid = 0;
        c.shakeFrames = 0.0;
        c.ringWrite = 0;
        c.unconsumed = 0;
        std::memset(c.ring, 0, sizeof(c.ring));
        // Calibration parameters are kept (KPADReset clears the buffer, not
        // the configuration).
    }
}

void KPADSetBtnRepeat(s32 chan, f32 delay_sec, f32 pulse_sec) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    gChannels[chan].delaySec = delay_sec;
    gChannels[chan].pulseSec = pulse_sec;
}

void KPADSetSensorHeight(s32 chan, f32 level) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    gChannels[chan].sensorHeight = level;
}

void KPADSetPosParam(s32 chan, f32 play_radius, f32 sensitivity) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    gChannels[chan].posRadius = play_radius;
    gChannels[chan].posSensitivity = sensitivity;
}

void KPADSetHoriParam(s32 chan, f32 play_radius, f32 sensitivity) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    gChannels[chan].horiRadius = play_radius;
    gChannels[chan].horiSensitivity = sensitivity;
}

void KPADSetDistParam(s32 chan, f32 play_radius, f32 sensitivity) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    gChannels[chan].distRadius = play_radius;
    gChannels[chan].distSensitivity = sensitivity;
}

s32 KPADRead(s32 chan, KPADStatus samplingBufs[], u32 length) {
    if (chan < 0 || chan >= kChannels) {
        return 0;
    }
    KpadChannel& c = gChannels[chan];
    if (!c.initialized || !c.prevConnected) {
        return 0;
    }
    const s32 n = static_cast<s32>(c.unconsumed < static_cast<int>(length) ? c.unconsumed : length);
    for (s32 i = 0; i < n; ++i) {
        const int idx = (c.ringWrite - 1 - i + kRingSize) % kRingSize;
        samplingBufs[i] = c.ring[idx];
    }
    c.unconsumed -= n;
    return n;
}

} // extern "C"

namespace Platform::CompatInput {

uint32_t getHoldButtons(int chan) {
    if (chan < 0 || chan >= kChannels) {
        return 0;
    }
    const KpadChannel& c = gChannels[chan];
    return c.initialized && c.prevConnected ? c.hold : 0;
}

uint32_t getTrigButtons(int chan) {
    if (chan < 0 || chan >= kChannels) {
        return 0;
    }
    const KpadChannel& c = gChannels[chan];
    return c.initialized && c.prevConnected ? c.trig : 0;
}

// PC_PORT (M10.1): see KPADCompat.h — A, or the pointer's confirm click.
bool getMenuDecideTrigger(int chan) {
    if (chan < 0 || chan >= kChannels) {
        return false;
    }
    const KpadChannel& c = gChannels[chan];
    if (!c.initialized || !c.prevConnected) {
        return false;
    }
    if ((c.trig & WPAD_BUTTON_A) != 0) {
        return true;
    }
    // The click only counts as "decide" when the pointer is live (the mouse is
    // over the window): a click outside the play area must not activate the
    // item under a stale pointer position.
    return c.mouseLeftTrig && c.dpdValid > 0;
}

// PC_PORT (M10.1): the pointer's cancel click (right button), same rule.
bool getMenuCancelTrigger(int chan) {
    if (chan < 0 || chan >= kChannels) {
        return false;
    }
    const KpadChannel& c = gChannels[chan];
    if (!c.initialized || !c.prevConnected) {
        return false;
    }
    if ((c.trig & WPAD_BUTTON_B) != 0) {
        return true;
    }
    return c.mouseRightTrig && c.dpdValid > 0;
}

// PC_PORT (M10.1): a menu D-pad built from the bound direction buttons AND the
// analog stick of a pad on the same channel, so a selection can be moved with
// the D-pad, the arrow keys or the stick without touching the pointer.
int getMenuNav(int chan) {
    if (chan < 0 || chan >= kChannels) {
        return 0;
    }
    KpadChannel& c = gChannels[chan];
    if (!c.initialized || !c.prevConnected) {
        return 0;
    }

    int nav = 0;
    const uint32_t t = c.trig;
    if ((t & WPAD_BUTTON_UP) != 0) nav |= kMenuNavUp;
    if ((t & WPAD_BUTTON_DOWN) != 0) nav |= kMenuNavDown;
    if ((t & WPAD_BUTTON_LEFT) != 0) nav |= kMenuNavLeft;
    if ((t & WPAD_BUTTON_RIGHT) != 0) nav |= kMenuNavRight;

    // Stick (digital threshold, edge-triggered against the previous frame's
    // level so a held stick does not scroll at 60 Hz).
    if (c.unconsumed > 0 || c.ringWrite != 0) {
        const int last = (c.ringWrite + kRingSize - 1) % kRingSize;
        const Vec2 stick = c.ring[last].ex_status.fs.stick;
        const bool up = stick.y > 0.55f;
        const bool down = stick.y < -0.55f;
        const bool left = stick.x < -0.55f;
        const bool right = stick.x > 0.55f;
        const int level = (up ? kMenuNavUp : 0) | (down ? kMenuNavDown : 0) | (left ? kMenuNavLeft : 0) |
                          (right ? kMenuNavRight : 0);
        nav |= level & ~c.prevMenuNavStick;
        c.prevMenuNavStick = level;
    }

    return nav;
}


} // namespace Platform::CompatInput
