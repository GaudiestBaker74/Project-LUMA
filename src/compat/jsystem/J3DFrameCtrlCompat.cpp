// =============================================================================
// PC_PORT extraction of J3DFrameCtrl from the vendored
// JSystem/J3DGraphAnimator/J3DAnimation.cpp (M9.5.3a).
//
// The full J3DAnimation.cpp (1300+ lines) pulls in J3DMaterialAttach and
// J3DModelData — J3D model territory that lands with M9.5.4. The layout anim
// players only need the frame controller, so its three out-of-line methods
// (init/checkPass/update) are compiled here VERBATIM from the decompilation;
// every other J3DFrameCtrl member is inline in the header.
// =============================================================================
#include "JSystem/J3DGraphAnimator/J3DAnimation.hpp"

void J3DFrameCtrl::init(s16 endFrame) {
    mAttribute = EMode_LOOP;
    mState = 0;
    mStart = 0;
    mEnd = endFrame;
    mLoop = 0;
    mRate = 1.0f;
    mFrame = 0.0f;
}

int J3DFrameCtrl::checkPass(f32 passFrame) {
    f32 next_frame = mFrame + mRate;

    switch (mAttribute) {
    case 0:
    case 1:
        if (next_frame < mStart) {
            next_frame = mStart;
        }

        if (next_frame >= mEnd) {
            next_frame = mEnd - 0.001f;
        }

        if (mFrame <= next_frame) {
            if (mFrame <= passFrame && passFrame < next_frame) {
                return true;
            } else {
                return false;
            }
        }

        if (next_frame <= passFrame && passFrame < mFrame) {
            return true;
        }
        return false;
    case 2:
        if (mFrame < mStart) {
            while (next_frame < mStart) {
                if (mLoop - mStart <= 0.0f) {
                    break;
                }
                next_frame += mLoop - mStart;
            }

            if (next_frame <= passFrame && passFrame < mLoop) {
                return true;
            } else {
                return false;
            }
        } else if (mEnd <= mFrame) {
            while (next_frame >= mEnd) {
                if (mEnd - mLoop <= 0.0f) {
                    break;
                }
                next_frame -= mEnd - mLoop;
            }

            if (mLoop <= passFrame && passFrame < next_frame) {
                return true;
            } else {
                return false;
            }
        } else if (next_frame < mStart) {
            while (next_frame < mStart) {
                if (mLoop - mStart <= 0.0f) {
                    break;
                }
                next_frame += mLoop - mStart;
            }

            if ((mStart <= passFrame && passFrame < mFrame) || (next_frame <= passFrame && passFrame < mLoop)) {
                return true;
            } else {
                return false;
            }
        } else if (mEnd <= next_frame) {
            while (next_frame >= mEnd) {
                if (mEnd - mLoop <= 0.0f) {
                    break;
                }

                next_frame -= mEnd - mLoop;
            }

            if ((mFrame <= passFrame && passFrame < mEnd) || (mLoop <= passFrame && passFrame < next_frame)) {
                return true;
            } else {
                return false;
            }
        } else if (mFrame <= next_frame) {
            if (mFrame <= passFrame && passFrame < next_frame) {
                return true;
            } else {
                return false;
            }
        } else if (next_frame <= passFrame && passFrame < mFrame) {
            return true;
        }
        return false;
    case 3:
    case 4:
        if (next_frame >= mEnd) {
            next_frame = mEnd - 0.001f;
        }

        if (next_frame < mStart) {
            next_frame = mStart;
        }

        if (mFrame <= next_frame) {
            if (mFrame <= passFrame && passFrame < next_frame) {
                return true;
            } else {
                return false;
            }
        }

        if (next_frame <= passFrame && passFrame < mFrame) {
            return true;
        }
        return false;
    default:
        return false;
    }
}

void J3DFrameCtrl::update() {
    mState = 0;
    mFrame += mRate;

    switch (mAttribute) {
    case EMode_NONE:
        if (mFrame < mStart) {
            mFrame = mStart;
            mRate = 0.0f;
            mState |= (u8)1;
        }
        if (mFrame >= mEnd) {
            mFrame = mEnd - 0.001f;
            mRate = 0.0f;
            mState |= (u8)1;
        }
        break;
    case EMode_RESET:
        if (mFrame < mStart) {
            mFrame = mStart;
            mRate = 0.0f;
            mState |= (u8)1;
        }
        if (mFrame >= mEnd) {
            mFrame = mStart;
            mRate = 0.0f;
            mState |= (u8)1;
        }
        break;
    case EMode_LOOP:
        while (mFrame < mStart) {
            mState |= (u8)2;
            if (mLoop - mStart <= 0.0f) {
                break;
            }
            mFrame += mLoop - mStart;
        }
        while (mFrame >= mEnd) {
            mState |= (u8)2;
            if (mEnd - mLoop <= 0.0f) {
                break;
            }
            mFrame -= mEnd - mLoop;
        }
        break;
    case EMode_REVERSE:
        if (mFrame >= mEnd) {
            mFrame = mEnd - (mFrame - mEnd);
            mRate = -mRate;
        }
        if (mFrame < mStart) {
            mFrame = mStart - (mFrame - mStart);
            mRate = 0.0f;
            mState |= (u8)1;
        }
        break;
    case EMode_LOOP_REVERSE:
        if (mFrame >= mEnd - 1.0f) {
            mFrame = (mEnd - 1.0f) - (mFrame - (mEnd - 1.0f));
            mRate = -mRate;
        }
        if (mFrame < mStart) {
            mFrame = mStart - (mFrame - mStart);
            mRate = -mRate;
            mState |= (u8)2;
        }
        break;
    }
}
