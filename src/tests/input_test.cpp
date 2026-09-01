// =============================================================================
// M6 input tests: the compat KPAD/WPAD layer (Platform::CompatInput) driven
// with synthetic raw frames, plus one real-game-code integration test
// (vendored WPadButton.cpp over the KPAD layer).
//
// These tests do NOT need SDL video: they drive Platform::CompatInput directly
// with hand-built InputState frames, so they run headless.
// =============================================================================

#include "compat/kpad/InputConfig.h"
#include "compat/kpad/KPADCompat.h"
#include "compat/kpad/WPADInternal.h"
#include "tests/test_runner.h"

#include <revolution/kpad.h>
#include <revolution/wpad.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ostream>

// Diagnostic printing for Platform::Key (used by CHECK_EQ below).
std::ostream& operator<<(std::ostream& os, Platform::Key key) {
    const char* name = Platform::keyName(key);
    return os << (name != nullptr ? name : "Key::None");
}

namespace {

constexpr double kStep = 1.0 / 60.0;

// Fresh compat state: no config file, built-in defaults, channels reset.
void resetCompat() {
    Platform::CompatInput::InputConfig::setOverridePath("/nonexistent/input.ini");
    Platform::CompatInput::setConfig(Platform::CompatInput::InputConfig::defaults());
    Platform::CompatInput::init();  // calls KPADInit (loads defaults)
}

void setKey(Platform::InputState& s, Platform::Key key) {
    s.keys[static_cast<int>(key)] = true;
}

void clearKeys(Platform::InputState& s) {
    std::memset(s.keys, 0, sizeof(s.keys));
}

int readChannel(int chan, KPADStatus* buf, int capacity) {
    return KPADRead(chan, buf, static_cast<u32>(capacity));
}

} // namespace

// ---------------------------------------------------------------------------
// KPADRead before any init → 0 samples (no state).
// ---------------------------------------------------------------------------
TEST_CASE(kpad_read_zero_before_init) {
    Platform::CompatInput::InputConfig::setOverridePath("/nonexistent/input.ini");
    KPADStatus buf[120];
    CHECK_EQ(KPADRead(0, buf, 120), 0);
}

// ---------------------------------------------------------------------------
// Button hold/trig/release edges on the keyboard_mouse channel.
// ---------------------------------------------------------------------------
TEST_CASE(kpad_buttons_edges) {
    resetCompat();
    Platform::InputState s;
    KPADStatus buf[4];

    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK_EQ(buf[0].hold, 0u);

    // Press A (Space by default).
    setKey(s, Platform::Key::Space);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK((buf[0].trig & WPAD_BUTTON_A) != 0);
    CHECK((buf[0].hold & WPAD_BUTTON_A) != 0);
    CHECK_EQ(buf[0].release, 0u);

    // Still held: trig clears.
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK((buf[0].hold & WPAD_BUTTON_A) != 0);
    CHECK_EQ(buf[0].trig, 0u);

    // Release: release edge, hold clears.
    clearKeys(s);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK_EQ(buf[0].hold, 0u);
    CHECK((buf[0].release & WPAD_BUTTON_A) != 0);
}

// ---------------------------------------------------------------------------
// Auto-repeat: KPADSetBtnRepeat → KPAD_BUTTON_RPT in hold (never in trig).
// ---------------------------------------------------------------------------
TEST_CASE(kpad_button_repeat) {
    resetCompat();
    KPADSetBtnRepeat(0, 0.1f, 0.05f);
    Platform::InputState s;
    KPADStatus buf[4];

    setKey(s, Platform::Key::Space);
    Platform::CompatInput::updateFrame(s, kStep);  // t=0.0166, acc=0.0166
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK_EQ(buf[0].hold & KPAD_BUTTON_RPT, 0u);

    Platform::CompatInput::updateFrame(s, 0.10);  // acc=0.1166 ≥ 0.1 → fired
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK((buf[0].hold & KPAD_BUTTON_RPT) != 0);
    CHECK_EQ(buf[0].trig & KPAD_BUTTON_RPT, 0u);  // RPT never in trig

    // Held: RPT stays while the button is held.
    Platform::CompatInput::updateFrame(s, 0.06);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK((buf[0].hold & KPAD_BUTTON_RPT) != 0);
    CHECK((buf[0].hold & WPAD_BUTTON_A) != 0);

    // Release: RPT cleared.
    clearKeys(s);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK_EQ(buf[0].hold, 0u);
}

// ---------------------------------------------------------------------------
// Nunchuk stick from WASD: normalized, diagonals normalized.
// ---------------------------------------------------------------------------
TEST_CASE(kpad_stick_wasd) {
    resetCompat();
    Platform::InputState s;
    KPADStatus buf[4];

    setKey(s, Platform::Key::W);
    setKey(s, Platform::Key::D);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    const Vec2& stick = buf[0].ex_status.fs.stick;
    CHECK(std::fabs(stick.x - 0.70710678f) < 1e-4f);
    CHECK(std::fabs(stick.y - 0.70710678f) < 1e-4f);

    clearKeys(s);
    setKey(s, Platform::Key::S);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK(std::fabs(buf[0].ex_status.fs.stick.x) < 1e-6f);
    CHECK(std::fabs(buf[0].ex_status.fs.stick.y + 1.0f) < 1e-6f);
}

// ---------------------------------------------------------------------------
// Pointer: mouse → pos normalized [-1,1]; dpd_valid_fg ramp.
// ---------------------------------------------------------------------------
TEST_CASE(kpad_pointer_mouse) {
    resetCompat();
    Platform::InputState s;
    KPADStatus buf[4];

    s.mouseNx = 0.25f;
    s.mouseNy = 0.75f;
    s.mouseInWindow = true;

    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK_EQ(buf[0].dpd_valid_fg, 1);
    CHECK(std::fabs(buf[0].pos.x + 0.5f) < 1e-5f);  // 0.25*2-1 = -0.5
    CHECK(std::fabs(buf[0].pos.y - 0.5f) < 1e-5f);  // 0.75*2-1 = +0.5

    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK(buf[0].dpd_valid_fg >= 2);  // history threshold for WPadPointer

    // Mouse outside the window → pointer invalid.
    s.mouseInWindow = false;
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK_EQ(buf[0].dpd_valid_fg, 0);
}

// ---------------------------------------------------------------------------
// Accelerometer: rest (0,1,0) ~1g, shake impulse while the shake key is held.
// ---------------------------------------------------------------------------
TEST_CASE(kpad_accel_rest_shake) {
    resetCompat();
    Platform::InputState s;
    KPADStatus buf[4];

    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK(std::fabs(buf[0].acc.x) < 1e-6f);
    CHECK(std::fabs(buf[0].acc.y - 1.0f) < 1e-6f);
    CHECK(std::fabs(buf[0].acc.z) < 1e-6f);
    CHECK(std::fabs(buf[0].acc_value - 1.0f) < 1e-4f);
    CHECK(std::fabs(buf[0].acc_speed) < 1e-6f);

    // Shake (K by default).
    setKey(s, Platform::Key::K);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK(buf[0].acc_value > 2.5f);  // 3g impulse → |acc| ≈ 3.16
    clearKeys(s);

    // Impulse decays after a few frames (6-frame transient).
    for (int i = 0; i < 8; ++i) {
        Platform::CompatInput::updateFrame(s, kStep);
        CHECK_EQ(readChannel(0, buf, 4), 1);
    }
    CHECK(std::fabs(buf[0].acc_value - 1.0f) < 1e-4f);
}

// ---------------------------------------------------------------------------
// KPADRead: newest sample first, up to 120 accumulated.
// ---------------------------------------------------------------------------
TEST_CASE(kpad_read_newest_first) {
    resetCompat();
    Platform::InputState s;
    KPADStatus buf[8];

    s.mouseNx = 0.1f;
    Platform::CompatInput::updateFrame(s, kStep);
    s.mouseNx = 0.5f;
    Platform::CompatInput::updateFrame(s, kStep);
    s.mouseNx = 0.9f;
    Platform::CompatInput::updateFrame(s, kStep);

    CHECK_EQ(readChannel(0, buf, 8), 3);
    CHECK(std::fabs(buf[0].pos.x - 0.8f) < 1e-5f);  // newest (0.9*2-1)
    CHECK(std::fabs(buf[1].pos.x - 0.0f) < 1e-5f);  // middle
    CHECK(std::fabs(buf[2].pos.x + 0.8f) < 1e-5f);  // oldest (0.1*2-1)

    // Second read drains the buffer.
    CHECK_EQ(readChannel(0, buf, 8), 0);
}

// ---------------------------------------------------------------------------
// WPAD: connect/extend callbacks, probe, motor, info, speaker no-op.
// ---------------------------------------------------------------------------
namespace {
int gConnectCalls = 0;
int gConnectReason = -99;
int gExtCalls = 0;
int gExtType = -99;
int gInfoCalls = 0;
int gInfoResult = -99;

void connectCb(s32 chan, s32 reason) {
    (void)chan;
    ++gConnectCalls;
    gConnectReason = reason;
}

void extCb(s32 chan, s32 extension) {
    (void)chan;
    ++gExtCalls;
    gExtType = extension;
}

void infoCb(s32 chan, s32 result) {
    (void)chan;
    ++gInfoCalls;
    gInfoResult = result;
}

struct RumbleRecord {
    int calls = 0;
    int lastIndex = -99;
    bool lastOn = false;
};
RumbleRecord gRumble;

void rumbleSink(int gamepadIndex, bool on) {
    ++gRumble.calls;
    gRumble.lastIndex = gamepadIndex;
    gRumble.lastOn = on;
}
} // namespace

TEST_CASE(wpad_connect_probe_extension) {
    resetCompat();
    gConnectCalls = gExtCalls = 0;
    WPADSetConnectCallback(0, connectCb);
    WPADSetExtensionCallback(0, extCb);

    Platform::InputState s;
    Platform::CompatInput::updateFrame(s, kStep);

    // Channel 0 (keyboard+mouse) connects on the first frame.
    CHECK_EQ(gConnectCalls, 1);
    CHECK_EQ(gConnectReason, WPAD_ERR_NONE);
    CHECK_EQ(gExtCalls, 1);
    CHECK_EQ(gExtType, WPAD_DEV_FREESTYLE);

    // Probe: connected channel reports its type; absent channel reports none.
    u32 type = 999;
    CHECK_EQ(WPADProbe(0, &type), WPAD_ERR_NONE);
    CHECK_EQ(type, WPAD_DEV_FREESTYLE);
    type = 999;
    CHECK_EQ(WPADProbe(1, &type), WPAD_ERR_NO_CONTROLLER);
    CHECK_EQ(type, WPAD_DEV_NOT_FOUND);

    // Disconnect (WPADDisconnect) fires the connect callback with NO_CONTROLLER.
    WPADDisconnect(0);
    CHECK_EQ(gConnectCalls, 2);
    CHECK_EQ(gConnectReason, WPAD_ERR_NO_CONTROLLER);
    CHECK_EQ(WPADProbe(0, &type), WPAD_ERR_NO_CONTROLLER);
}

TEST_CASE(wpad_motor_rumble_sink) {
    resetCompat();
    Platform::InputState s;
    Platform::CompatInput::updateFrame(s, kStep);  // connect channel 0

    Platform::CompatInput::setRumbleSink(rumbleSink);
    gRumble = RumbleRecord{};

    WPADControlMotor(0, WPAD_MOTOR_RUMBLE);
    CHECK(Platform::CompatInput::wpadIsMotorOn(0));
    CHECK_EQ(gRumble.calls, 1);
    CHECK_EQ(gRumble.lastOn, true);
    CHECK_EQ(gRumble.lastIndex, -1);  // channel 0 = keyboard/mouse, no gamepad

    WPADControlMotor(0, WPAD_MOTOR_STOP);
    CHECK(!Platform::CompatInput::wpadIsMotorOn(0));
    CHECK_EQ(gRumble.calls, 2);
    CHECK_EQ(gRumble.lastOn, false);

    Platform::CompatInput::setRumbleSink(nullptr);
}

TEST_CASE(wpad_info_battery) {
    resetCompat();
    Platform::InputState s;
    Platform::CompatInput::updateFrame(s, kStep);

    gInfoCalls = gInfoResult = 0;
    WPADInfo info;
    std::memset(&info, 0, sizeof(info));
    CHECK_EQ(WPADGetInfoAsync(0, &info, infoCb), WPAD_ERR_NONE);
    CHECK_EQ(gInfoCalls, 1);
    CHECK_EQ(gInfoResult, WPAD_ERR_NONE);
    CHECK_EQ(info.battery, WPAD_BATTERY_LEVEL_HIGH);
    CHECK_EQ(info.attach, true);
    CHECK_EQ(info.speaker, false);

    // Not connected → error, no callback.
    gInfoCalls = 0;
    CHECK_EQ(WPADGetInfoAsync(1, &info, infoCb), WPAD_ERR_NO_CONTROLLER);
    CHECK_EQ(gInfoCalls, 0);
}

TEST_CASE(wpad_speaker_noop) {
    resetCompat();
    Platform::InputState s;
    Platform::CompatInput::updateFrame(s, kStep);

    CHECK_EQ(WPADIsSpeakerEnabled(0), false);
    CHECK_EQ(WPADCanSendStreamData(0), false);
    CHECK_EQ(WPADControlSpeaker(0, 1, nullptr), WPAD_ERR_NOPERM);
    CHECK_EQ(WPADSendStreamData(0, nullptr, 0), WPAD_ERR_NOPERM);
    CHECK_EQ(WPADGetWorkMemorySize(), 0u);
    CHECK_EQ(WPADGetSensorBarPosition(), WPAD_SENSOR_BAR_POS_TOP);
}

// ---------------------------------------------------------------------------
// Config: a custom config/input.ini remaps buttons.
// ---------------------------------------------------------------------------
TEST_CASE(input_config_parse) {
    const char* path = "/tmp/galaxy_pc_input_test.ini";
    FILE* f = std::fopen(path, "w");
    REQUIRE(f != nullptr);
    std::fputs("[channel0]\n"
               "source=keyboard_mouse\n"
               "bind_a=key:enter\n"
               "bind_b=none\n"
               "bind_up=key:up\n"
               "stick=arrows\n"
               "shake=key:j\n",
               f);
    std::fclose(f);

    Platform::CompatInput::InputConfig::setOverridePath(path);
    Platform::CompatInput::InputConfig cfg = Platform::CompatInput::InputConfig::load();
    CHECK_EQ(cfg.channels[0].bind[static_cast<int>(Platform::CompatInput::WiiAction::A)], Platform::Key::Enter);
    CHECK_EQ(cfg.channels[0].bind[static_cast<int>(Platform::CompatInput::WiiAction::B)], Platform::Key::None);
    CHECK_EQ(cfg.channels[0].bind[static_cast<int>(Platform::CompatInput::WiiAction::Shake)], Platform::Key::J);
    CHECK_EQ(cfg.channels[0].stickUsesArrows, true);

    // Behavior: Enter now maps to A; Space is unbound.
    Platform::CompatInput::setConfig(cfg);
    Platform::CompatInput::init();
    Platform::InputState s;
    KPADStatus buf[4];
    setKey(s, Platform::Key::Enter);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK((buf[0].hold & WPAD_BUTTON_A) != 0);

    clearKeys(s);
    setKey(s, Platform::Key::Space);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK_EQ(buf[0].hold, 0u);

    // Stick now follows the arrows instead of WASD.
    clearKeys(s);
    setKey(s, Platform::Key::Up);
    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(0, buf, 4), 1);
    CHECK(std::fabs(buf[0].ex_status.fs.stick.y - 1.0f) < 1e-6f);

    Platform::CompatInput::InputConfig::setOverridePath(nullptr);
}

// ---------------------------------------------------------------------------
// Gamepad channel: nunchuk-style (default) and classic (use_classic).
// ---------------------------------------------------------------------------
TEST_CASE(kpad_gamepad_nunchuk) {
    resetCompat();
    Platform::CompatInput::InputConfig cfg = Platform::CompatInput::InputConfig::defaults();
    cfg.channels[1].source = Platform::CompatInput::Source::Gamepad;
    cfg.channels[1].gamepadIndex = 0;
    Platform::CompatInput::setConfig(cfg);
    Platform::CompatInput::init();

    Platform::InputState s;
    s.gamepads[0].connected = true;
    s.gamepads[0].a = true;
    s.gamepads[0].leftX = 0.5f;
    s.gamepads[0].leftY = -0.25f;  // up
    KPADStatus buf[4];

    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(1, buf, 4), 1);
    CHECK_EQ(buf[0].dev_type, WPAD_DEV_FREESTYLE);
    CHECK((buf[0].hold & WPAD_BUTTON_A) != 0);
    CHECK((buf[0].hold & WPAD_BUTTON_Z) == 0);
    CHECK(std::fabs(buf[0].ex_status.fs.stick.x - 0.5f) < 1e-5f);
    CHECK(std::fabs(buf[0].ex_status.fs.stick.y - 0.25f) < 1e-5f);

    u32 type = 0;
    CHECK_EQ(WPADProbe(1, &type), WPAD_ERR_NONE);
    CHECK_EQ(type, WPAD_DEV_FREESTYLE);
}

TEST_CASE(kpad_gamepad_classic) {
    resetCompat();
    Platform::CompatInput::InputConfig cfg = Platform::CompatInput::InputConfig::defaults();
    cfg.channels[1].source = Platform::CompatInput::Source::Gamepad;
    cfg.channels[1].gamepadIndex = 0;
    cfg.channels[1].useClassic = true;
    Platform::CompatInput::setConfig(cfg);
    Platform::CompatInput::init();

    Platform::InputState s;
    s.gamepads[0].connected = true;
    s.gamepads[0].a = true;
    s.gamepads[0].leftX = -1.0f;
    s.gamepads[0].rightX = 1.0f;
    KPADStatus buf[4];

    Platform::CompatInput::updateFrame(s, kStep);
    CHECK_EQ(readChannel(1, buf, 4), 1);
    CHECK_EQ(buf[0].dev_type, WPAD_DEV_CLASSIC);
    CHECK((buf[0].ex_status.cl.hold & WPAD_CL_BUTTON_A) != 0);
    CHECK(std::fabs(buf[0].ex_status.cl.lstick.x + 1.0f) < 1e-5f);
    CHECK(std::fabs(buf[0].ex_status.cl.rstick.x - 1.0f) < 1e-5f);

    u32 type = 0;
    CHECK_EQ(WPADProbe(1, &type), WPAD_ERR_NONE);
    CHECK_EQ(type, WPAD_DEV_CLASSIC);
}

// ---------------------------------------------------------------------------
// WPadButton semantics (mirror of the vendored Game/System/WPadButton.cpp).
//
// The real file cannot be compiled on the host yet (its WPad.hpp → JGeometry
// chain needs Metrowerks-header shims — see tests/CMakeLists.txt). This test
// mirrors WPadButton::update() and the constructor's KPADSetBtnRepeat call
// verbatim, pinning the exact KPAD behavior the game code depends on:
//   mTrigger = trig & ~mHold & KPAD_BUTTON_MASK
//   mHold = hold; mRepeat = mTrigger | (hold&KPAD_BUTTON_RPT ? mHold : 0)
//   mRelease = release (only when wpad_err is NONE/BUSY)
// ---------------------------------------------------------------------------
TEST_CASE(kpad_wpadbutton_semantics) {
    resetCompat();
    Platform::InputState s;
    KPADStatus status[1];
    u32 mHold = 0, mTrigger = 0, mRelease = 0, mRepeat = 0;

    auto wpadButtonUpdate = [&]() {
        // Mirrors WPadButton::update (vendored, M6 input semantics).
        const KPADStatus* pStatus = &status[0];
        if (mHold != 0) {
            mTrigger = pStatus->trig & ~mHold & KPAD_BUTTON_MASK;
        } else {
            mTrigger = pStatus->trig;
        }
        if (pStatus->wpad_err == WPAD_ERR_NONE || pStatus->wpad_err == WPAD_ERR_BUSY) {
            mHold = pStatus->hold;
            mRelease = pStatus->release;
        }
        mRepeat = mTrigger;
        if ((pStatus->hold & KPAD_BUTTON_RPT) != 0) {
            mRepeat |= mHold;
        }
    };

    // WPadButton ctor calls KPADSetBtnRepeat(channel, 1/2.4, 1/6).
    KPADSetBtnRepeat(0, 1.0f / 2.4f, 1.0f / 6.0f);

    // Connect channel 0, then press A (Space).
    Platform::CompatInput::updateFrame(s, kStep);
    setKey(s, Platform::Key::Space);
    Platform::CompatInput::updateFrame(s, kStep);
    REQUIRE(KPADRead(0, status, 1) == 1);
    wpadButtonUpdate();
    CHECK((mHold & WPAD_BUTTON_A) != 0);
    CHECK((mTrigger & WPAD_BUTTON_A) != 0);

    // Held: hold stays, trigger clears.
    Platform::CompatInput::updateFrame(s, kStep);
    REQUIRE(KPADRead(0, status, 1) == 1);
    wpadButtonUpdate();
    CHECK((mHold & WPAD_BUTTON_A) != 0);
    CHECK_EQ(mTrigger & WPAD_BUTTON_A, 0u);

    // Release: release edge, hold clears.
    clearKeys(s);
    Platform::CompatInput::updateFrame(s, kStep);
    REQUIRE(KPADRead(0, status, 1) == 1);
    wpadButtonUpdate();
    CHECK_EQ(mHold, 0u);
    CHECK((mRelease & WPAD_BUTTON_A) != 0);

    // Auto-repeat: after the delay, hold carries KPAD_BUTTON_RPT and mRepeat
    // reports the held button (menu scrolling).
    KPADSetBtnRepeat(0, 0.1f, 0.05f);
    setKey(s, Platform::Key::Space);
    Platform::CompatInput::updateFrame(s, 0.0);
    Platform::CompatInput::updateFrame(s, 0.11);
    REQUIRE(KPADRead(0, status, 1) == 1);
    wpadButtonUpdate();
    CHECK((mHold & KPAD_BUTTON_RPT) != 0);
    CHECK((mRepeat & WPAD_BUTTON_A) != 0);
    CHECK_EQ(mTrigger & KPAD_BUTTON_RPT, 0u);  // RPT never leaks into trig
    clearKeys(s);
}
