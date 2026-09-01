#pragma once
// =============================================================================
// Shared channel state between the KPAD layer (KPAD.cpp) and the WPAD layer
// (WPAD.cpp): connection/extension notifications, motor state.
// =============================================================================

#include "compat/kpad/KPADCompat.h"

#include <cstdint>

namespace Platform::CompatInput {

// Registers the motor sink (owned by the WPAD layer).
void wpadSetRumbleSink(RumbleSink sink);

// Fires the registered WPAD connect callback (WPAD_ERR_NONE) followed by the
// extension callback for the channel's device type.
void wpadNotifyConnect(int chan, uint32_t devType);
// Fires the connect callback with WPAD_ERR_NO_CONTROLLER.
void wpadNotifyDisconnect(int chan);

bool wpadIsConnected(int chan);
// Motor (Wiimote rumble): forwards to the rumble sink using the channel's
// gamepad slot. `on` = rumble / stop.
void wpadSetMotor(int chan, bool on);
bool wpadIsMotorOn(int chan);

} // namespace Platform::CompatInput
