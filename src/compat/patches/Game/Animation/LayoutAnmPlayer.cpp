// =============================================================================
// PC_PORT PATCH of the vendored Game/Animation/LayoutAnmPlayer.cpp (M9.5.3a).
//
// Changes vs. upstream (everything else is verbatim):
//   1. start() gains a null-transform guard. The decompilation dereferences
//      mAnimTransform unconditionally; on the host the transform can be null
//      when the brlan is missing from the arc or is still unparsed (the brlan
//      big-endian half of the engine lands in M9.5.3b, and
//      Layout::CreateAnimTransform returns null for unparsed resources). The
//      guard keeps nerves that wait on isAnimStopped() from hanging or
//      crashing in that window.
//   2. isStop() — commented out upstream ("// LayoutAnmPlayer::isStop") — is
//      reconstructed: an animation counts as stopped when there is no
//      transform bound (nothing will ever play, see 1) or when the frame
//      controller flagged a stop. J3DFrameCtrl::update() sets state bit 1
//      when a non-looping animation reaches its end frame (see the vendored
//      JSystem/J3DGraphAnimator/J3DAnimation.cpp), which is exactly the
//      condition MR::isAnimStopped() callers wait for.
// =============================================================================
#include "Game/Animation/LayoutAnmPlayer.hpp"
#include "Game/Screen/LayoutManager.hpp"
#include <nw4r/lyt/animation.h>

LayoutAnmPlayer::LayoutAnmPlayer(const LayoutManager* pManager) : mManager(pManager), mAnimName(nullptr), mAnimTransform(nullptr), mFrameCtrl(0) {
}

void LayoutAnmPlayer::movement() {
    if (mAnimTransform != nullptr) {
        mFrameCtrl.update();
    }
}

void LayoutAnmPlayer::reflectFrame() {
    if (mAnimTransform != nullptr) {
        // TODO: Should be AnimTransform::SetFrame
        mAnimTransform->mFrame = mFrameCtrl.getFrame();
    }
}

void LayoutAnmPlayer::start(const char* pAnimName) {
    nw4r::lyt::AnimTransform* pAnimTransform = mManager->getAnimTransform(pAnimName);

    if (pAnimTransform != mAnimTransform) {
        mAnimTransform = pAnimTransform;
    }

    // PC_PORT: null-transform guard (see banner, point 1).
    if (mAnimTransform == nullptr) {
        mAnimName = pAnimName;
        return;
    }

    u32 frameSize = mAnimTransform->GetFrameSize();

    mFrameCtrl.init(static_cast< s16 >(frameSize));

    if (mAnimTransform->IsLoopData()) {
        mFrameCtrl.setAttribute(2);
    } else {
        mFrameCtrl.setAttribute(0);
    }

    mFrameCtrl.setFrame(0.0f);
    mFrameCtrl.setRate(1.0f);
    mAnimName = pAnimName;
}

void LayoutAnmPlayer::stop() {
    mFrameCtrl.setRate(0.0f);
}

// PC_PORT reconstruction (banner, point 2).
bool LayoutAnmPlayer::isStop() const {
    if (mAnimTransform == nullptr) {
        return true;
    }

    return mFrameCtrl.checkState(1);
}
