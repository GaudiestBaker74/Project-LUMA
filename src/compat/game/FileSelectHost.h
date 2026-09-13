#pragma once
// =============================================================================
// PC_PORT (M10) — FileSelectHost: host-side logic for the file-select screen.
//
// On console the FileSelector is a ~1585-line LiveActor (40 nerves, Mii/RFL,
// save game, 3D camera) living inside the TitleScene after the title sequence
// decides (A+B). The port instead mounts the REAL FileSelect.arc layout
// through the vendored SimpleLayout (same path as TitleLogo/PressStart) and
// puts the interaction on the host here:
//
//   * the star cursor follows the KPAD pointer (mouse / stick, see
//     docs/input.md) — drawn as a host disc until the real StarPointer actor
//     lands,
//   * A confirms the slot under the pointer (provisional thirds mapping until
//     the pane names of the arc are known from a runtime pane-tree dump),
//   * the selection persists to a host save store (saves/slotN.bin),
//   * B requests "back" (the console replays the title; v1 only logs).
//
// Buttons/pointer come from Platform::CompatInput getters that do NOT drain
// the KPADRead ring, so the vendored WPadHolder keeps its samples.
// =============================================================================

namespace compat::game {

class FileSelectHost {
public:
    // One step from the scene's movement list. Returns true when the screen
    // should stay alive (always for now).
    bool update();

    // Host stand-in for the star cursor, drawn AFTER the layout pass in the
    // scene's 2D space (MR::drawInitFor228Model-style, y up, layout units).
    void drawCursor() const;

    // Slot under the pointer with the provisional mapping (0/1/2).
    int provisionalSlot() const { return mSlot; }
    bool hasSave(int slot) const;

private:
    float mPointerX = 0.0f;  // KPAD pos space [-1,1]
    float mPointerY = 0.0f;
    bool mHavePointer = false;
    int mSlot = 0;
    int mConfirmedSlot = -1;  // last slot confirmed with A (-1 = none yet)
    bool mBackRequested = false;
};

}  // namespace compat::game
