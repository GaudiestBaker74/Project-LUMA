// =============================================================================
// PC_PORT PATCH of the vendored Game/Screen/EncouragePal60Window.cpp (see
// patches/README.md).
//
// The real window ("we recommend component cables for PAL60") needs the
// IconAButton system and the SysPALInfo layout; on a PC host PAL60 is
// impossible, so MR::isDisplayEncouragePal60Window() is false and the window
// never appears. TitleSequenceProduct still CONSTRUCTS one unconditionally
// (and calls initWithoutIter + kill), so the class must exist and stay inert.
// Only the members the ctor path touches are defined; the exe* nerve handlers
// are unreachable (no nerve is ever set) so they are not defined at all.
// =============================================================================

#include "Game/Screen/EncouragePal60Window.hpp"

EncouragePal60Window::EncouragePal60Window() : LayoutActor("PAL60推奨画面", true), mAButtonIcon(nullptr) {
}

void EncouragePal60Window::init(const JMapInfoIter& rIter) {
    (void)rIter;
    // PC_PORT: no SysPALInfo layout, no icon — the window is never shown.
    kill();
}

void EncouragePal60Window::appear() {
    // PC_PORT: unreachable (isDisplayEncouragePal60Window is false); keep the
    // actor dead so any stray isDead() stays true.
    kill();
}
