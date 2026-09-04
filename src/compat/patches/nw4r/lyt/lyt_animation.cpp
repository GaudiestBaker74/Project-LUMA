// =============================================================================
// PC_PORT PATCH of the vendored nw4r/lyt/lyt_animation.cpp (see
// src/compat/patches/README.md).
//
// Changes vs. upstream:
//   1. SetResource logs the pai1 file-table texture resolution once per
//      transform (INFO on success, WARN on null). RLTP (texture pattern)
//      silently stored nulls before — the strap screen's second Wiimote
//      image never appeared with no trace in the log.
//   2. Upstream memsets `animContNum` AnimationLinks while sizing the array
//      with `animNum` — an overflow when the transform is sized for a subset
//      (group binds: animNum < animContNum). Zero the SIZED amount instead.
// Everything else is identical to upstream.
// =============================================================================
#include "nw4r/lyt/animation.h"
#include "platform/Log/Log.h"
#include "nw4r/lyt/layout.h"
#include "nw4r/lyt/resourceAccessor.h"
#include "nw4r/lyt/resources.h"

namespace nw4r {
    namespace lyt {
        AnimTransform::AnimTransform() : mpRes(nullptr), mFrame(0) {
        }

        AnimTransform::~AnimTransform() {
        }

        u16 AnimTransform::GetFrameSize() const {
            return mpRes->frameSize;
        }

        bool AnimTransform::IsLoopData() const {
            return mpRes->loop != 0;
        }

        AnimTransformBasic::AnimTransformBasic() : mpFileResAry(nullptr), mAnimLinkAry(nullptr), mAnimLinkNum(0) {
        }

        AnimTransformBasic::~AnimTransformBasic() {
            Layout::DeleteArray(mAnimLinkAry, mAnimLinkNum);
            Layout::DeletePrimArray(mpFileResAry);
        }

        void AnimTransformBasic::SetResource(const res::AnimationBlock* pBlock, ResourceAccessor* pResAccessor) {
            SetResource(pBlock, pResAccessor, pBlock->animContNum);
        }

        void AnimTransformBasic::SetResource(const res::AnimationBlock* pBlock, ResourceAccessor* pResAccessor, u16 animNum) {
            mpRes = pBlock;
            mpFileResAry = nullptr;

            if (pBlock->fileNum > 0) {
                mpFileResAry = Layout::NewArray< void* >(pBlock->fileNum);
                if (mpFileResAry != nullptr) {
                    const u32* fileNameOffs = detail::ConvertOffsToPtr< u32 >(mpRes, sizeof(*mpRes));
                    for (int i = 0; i < mpRes->fileNum; i++) {
                        const char* const name = detail::GetStrTableStr(fileNameOffs, i);
                        mpFileResAry[i] = pResAccessor->GetResource('timg', name, 0);
                        // PC_PORT: file-table resolution feeds the RLTP
                        // texture-pattern swaps; a null here silently killed
                        // them (the strap screen's second Wiimote image).
                        if (mpFileResAry[i] != nullptr) {
                            PL_LOG_INFO("compat.lyt", "SetResource: file table[%d] '%s' ok", i, name);
                        } else {
                            PL_LOG_WARN("compat.lyt", "SetResource: file table[%d] '%s' NOT FOUND — RLTP patterns using it will be dropped", i, name);
                        }
                    }
                }
            }

            mAnimLinkAry = Layout::NewArray< AnimationLink >(animNum);
            if (mAnimLinkAry != nullptr) {
                mAnimLinkNum = animNum;
                // PC_PORT: upstream zeroed animContNum links while the array
                // holds animNum — an overflow for group-bound subsets.
                memset(mAnimLinkAry, 0, animNum * sizeof(AnimationLink));

                for (u16 i = 0; i < animNum; i++) {
                    new (&mAnimLinkAry[i]) AnimationLink();
                }
            }
        }
    };  // namespace lyt
};  // namespace nw4r
