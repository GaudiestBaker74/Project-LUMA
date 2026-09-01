// =============================================================================
// Platform::CompatInput — WPAD layer (M6).
//
// Implements the WPAD API the game calls (<revolution/wpad.h>) on top of the
// KPAD layer's channel state. Device features that have no PC equivalent are
// explicit no-ops marked TODO(PC_PORT): the Wiimote speaker and the DPD
// hardware pipeline (docs/input.md §6).
//
// Linkage: wpad.h declares the WPAD API inside `extern "C"`, so the public
// functions are defined at global scope with C linkage; the internal hooks
// used by the KPAD layer live in Platform::CompatInput.
// =============================================================================

#include "compat/kpad/KPADCompat.h"
#include "compat/kpad/WPADInternal.h"

#include <revolution/wpad.h>

#include <cstring>

// ---------------------------------------------------------------------------
// File-local state.
// ---------------------------------------------------------------------------
namespace {

constexpr int kChannels = WPAD_MAX_CONTROLLERS;  // 4

struct WpadChannel {
    WPADConnectCallback connectCb = nullptr;
    WPADExtensionCallback extCb = nullptr;
    bool connected = false;
    u32 devType = WPAD_DEV_UNKNOWN;
    u8 battery = WPAD_BATTERY_LEVEL_HIGH;
    bool motor = false;
};

WpadChannel gWpad[kChannels];
WPADAlloc gAlloc = nullptr;
WPADFree gFree = nullptr;
u8 gSensorBarPos = WPAD_SENSOR_BAR_POS_TOP;
u8 gAutoSleepMin = 15;
Platform::CompatInput::RumbleSink gRumbleSink = nullptr;

} // namespace

// ---------------------------------------------------------------------------
// Internal hooks (used by the KPAD layer).
// ---------------------------------------------------------------------------
namespace Platform::CompatInput {

void wpadSetRumbleSink(RumbleSink sink) { gRumbleSink = sink; }

void wpadNotifyConnect(int chan, uint32_t devType) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    WpadChannel& w = gWpad[chan];
    w.connected = true;
    w.devType = devType;
    if (w.connectCb != nullptr) {
        w.connectCb(chan, WPAD_ERR_NONE);
    }
    // Extension: nunchuk (FREESTYLE) by default; CLASSIC when configured.
    const u32 ext = (w.devType == WPAD_DEV_CLASSIC) ? static_cast<u32>(WPAD_DEV_CLASSIC)
                                                    : static_cast<u32>(WPAD_DEV_FREESTYLE);
    if (w.extCb != nullptr) {
        w.extCb(chan, static_cast<s32>(ext));
    }
}

void wpadNotifyDisconnect(int chan) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    WpadChannel& w = gWpad[chan];
    w.connected = false;
    w.devType = WPAD_DEV_UNKNOWN;
    if (w.connectCb != nullptr) {
        w.connectCb(chan, WPAD_ERR_NO_CONTROLLER);
    }
}

bool wpadIsConnected(int chan) {
    return chan >= 0 && chan < kChannels && gWpad[chan].connected;
}

void wpadSetMotor(int chan, bool on) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    gWpad[chan].motor = on;
    if (gRumbleSink != nullptr) {
        gRumbleSink(channelGamepadIndex(chan), on);
    }
}

bool wpadIsMotorOn(int chan) {
    return chan >= 0 && chan < kChannels && gWpad[chan].motor;
}

} // namespace Platform::CompatInput

// ---------------------------------------------------------------------------
// WPAD API (declared in <revolution/wpad.h>, extern "C").
// ---------------------------------------------------------------------------
extern "C" {

void WPADRegisterAllocator(WPADAlloc allocFunc, WPADFree freeFunc) {
    gAlloc = allocFunc;
    gFree = freeFunc;
}

u32 WPADGetWorkMemorySize(void) {
    // No work area needed: no Bluetooth/HID stack on PC.
    return 0;
}

void WPADInit(void) {
    for (int chan = 0; chan < kChannels; ++chan) {
        gWpad[chan].connected = false;
        gWpad[chan].devType = WPAD_DEV_UNKNOWN;
        gWpad[chan].battery = WPAD_BATTERY_LEVEL_HIGH;
        gWpad[chan].motor = false;
    }
}

void WPADDisconnect(s32 chan) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    if (gWpad[chan].connected) {
        Platform::CompatInput::wpadNotifyDisconnect(chan);
    }
}

s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback callback) {
    if (chan < 0 || chan >= kChannels || info == nullptr) {
        return WPAD_ERR_INVALID;
    }
    std::memset(info, 0, sizeof(*info));
    if (!gWpad[chan].connected) {
        return WPAD_ERR_NO_CONTROLLER;
    }
    info->dpd = true;
    info->speaker = false;   // TODO(PC_PORT): no Wiimote speaker on PC
    info->attach = true;
    info->lowBat = false;
    info->nearempty = false;
    info->battery = gWpad[chan].battery;
    info->led = 0;
    info->protocol = 0;
    info->firmware = 0;
    if (callback != nullptr) {
        callback(chan, WPAD_ERR_NONE);
    }
    return WPAD_ERR_NONE;
}

BOOL WPADIsSpeakerEnabled(s32 chan) {
    (void)chan;
    return false;  // TODO(PC_PORT): no Wiimote speaker on PC
}

s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback callback) {
    (void)chan;
    (void)command;
    (void)callback;
    return WPAD_ERR_NOPERM;  // TODO(PC_PORT): speaker stream not implemented
}

u8 WPADGetSpeakerVolume(void) {
    return 0;  // TODO(PC_PORT)
}

s32 WPADSendStreamData(s32 chan, void* data, u16 len) {
    (void)chan;
    (void)data;
    (void)len;
    return WPAD_ERR_NOPERM;  // TODO(PC_PORT)
}

BOOL WPADCanSendStreamData(s32 chan) {
    (void)chan;
    return false;  // TODO(PC_PORT)
}

void WPADGetAccGravityUnit(s32 chan, u32 type, WPADAcc* acc) {
    (void)chan;
    (void)type;
    if (acc != nullptr) {
        acc->x = 0;
        acc->y = 0;
        acc->z = 0;
    }
}

void WPADControlMotor(s32 chan, u32 motor) {
    if (chan < 0 || chan >= kChannels) {
        return;
    }
    Platform::CompatInput::wpadSetMotor(chan, motor == WPAD_MOTOR_RUMBLE);
}

BOOL WPADStopSimpleSync(void) {
    return true;
}

BOOL WPADIsDpdEnabled(s32 chan) {
    (void)chan;
    return true;  // pointer input is always available (mouse)
}

s32 WPADControlDpd(s32 chan, u32 command, WPADCallback callback) {
    (void)chan;
    (void)command;
    (void)callback;
    return WPAD_ERR_NONE;  // no-op: DPD is the mouse, no HW control needed
}

u32 WPADGetDataFormat(s32 chan) {
    (void)chan;
    return 0;
}

s32 WPADSetDataFormat(s32 chan, u32 fmt) {
    (void)chan;
    (void)fmt;
    return WPAD_ERR_NONE;
}

void WPADRead(s32 chan, void* status) {
    (void)chan;
    (void)status;
}

void WPADSetAutoSleepTime(u8 minute) {
    gAutoSleepMin = minute;
}

u8 WPADGetSensorBarPosition(void) {
    return gSensorBarPos;
}

s32 WPADProbe(s32 chan, u32* type) {
    if (chan < 0 || chan >= kChannels || type == nullptr) {
        return WPAD_ERR_INVALID;
    }
    if (!gWpad[chan].connected) {
        *type = WPAD_DEV_NOT_FOUND;
        return WPAD_ERR_NO_CONTROLLER;
    }
    *type = gWpad[chan].devType;
    return WPAD_ERR_NONE;
}

s32 WPADGetStatus(void) {
    return WPAD_ERR_NONE;
}

WPADConnectCallback WPADSetConnectCallback(s32 chan, WPADConnectCallback callback) {
    if (chan < 0 || chan >= kChannels) {
        return nullptr;
    }
    WPADConnectCallback previous = gWpad[chan].connectCb;
    gWpad[chan].connectCb = callback;
    return previous;
}

WPADExtensionCallback WPADSetExtensionCallback(s32 chan, WPADExtensionCallback callback) {
    if (chan < 0 || chan >= kChannels) {
        return nullptr;
    }
    WPADExtensionCallback previous = gWpad[chan].extCb;
    gWpad[chan].extCb = callback;
    return previous;
}

} // extern "C"
