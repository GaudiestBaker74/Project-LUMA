// =============================================================================
// PC_PORT PATCH of the vendored nw4r/lyt/lyt_layout.cpp (see
// src/compat/patches/README.md).
//
// Changes vs. upstream (diagnostics for the first real-brlyt builds):
//   * Build() logs one INFO breadcrumb per data block (kind + size) and a
//     final "done" line, so a crash inside Build pinpoints the exact block
//     from the boot log.
//   * 'pae1' handling gained a null guard: upstream dereferences pLastPane
//     unconditionally, which faults on malformed/unexpected block sequences.
//
// Everything else is identical to upstream.
// =============================================================================
#include "nw4r/lyt/animation.h"
#include "nw4r/lyt/common.h"
#include "nw4r/lyt/group.h"
#include "nw4r/lyt/layout.h"
#include "nw4r/lyt/material.h"
#include "nw4r/lyt/pane.h"
#include "nw4r/lyt/resources.h"
#include "nw4r/lyt/util.h"
#include "nw4r/ut/Rect.h"
#include "revolution/mem/allocator.h"

#include "platform/Log/Log.h"

namespace nw4r {
    namespace lyt {
        namespace {
            bool IsIncludeAnimationGroupRef(GroupContainer* pGroupContainer, const AnimationGroupRef* const groupRefs, u16 bindGroupNum,
                                            bool bDescendingBind, Pane* pTargetPane) {
                for (u16 grpIdx = 0; grpIdx < bindGroupNum; ++grpIdx) {
                    Group* const pGroup = pGroupContainer->FindGroupByName(groupRefs[grpIdx].GetName());
                    PaneLinkList& paneList = pGroup->GetPaneList();
                    for (PaneLinkList::Iterator it = paneList.GetBeginIter(); it != paneList.GetEndIter(); ++it) {
                        if (it->mTarget == pTargetPane) {
                            return true;
                        }

                        if (bDescendingBind) {
                            for (Pane* pParentPane = pTargetPane->GetParent(); pParentPane; pParentPane = pParentPane->GetParent()) {
                                if (it->mTarget == pParentPane) {
                                    return true;
                                }
                            }
                        }
                    }
                }

                return false;
            }
        };  // namespace

        MEMAllocator* Layout::mspAllocator = nullptr;

        Layout::Layout() : mpRootPane(nullptr), mpGroupContainer(nullptr), mLayoutSize(0.0f, 0.0f), _20(0) {
        }

        Layout::~Layout() {
            DeleteObj(mpGroupContainer);

            if (mpRootPane != nullptr && !mpRootPane->IsUserAllocated()) {
                DeleteObj(mpRootPane);
            }

            for (AnimTransformList::Iterator it = mAnimTransList.GetBeginIter(); it != mAnimTransList.GetEndIter();) {
                AnimTransformList::Iterator currIt = it++;
                mAnimTransList.Erase(currIt);
                DeleteObj(&(*currIt));
            }
        }

        bool Layout::Build(const void* lytResBuf, ResourceAccessor* pResAcsr) {
            const res::BinaryFileHeader* const pFileHead = static_cast< const res::BinaryFileHeader* >(lytResBuf);
            if (!detail::TestFileHeader(*pFileHead, 'RLYT')) {
                return false;
            }

            if (!detail::TestFileVersion(*pFileHead)) {
                return false;
            }

            ResBlockSet resBlockSet = {0};
            resBlockSet.pResAccessor = pResAcsr;
            Pane* pParentPane = nullptr;
            Pane* pLastPane = nullptr;
            bool bReadRootGroup = false;
            int groupNestLevel = 0;

            const void* dataPtr = static_cast< const u8* >(lytResBuf) + pFileHead->headerSize;
            for (int i = 0; i < pFileHead->dataBlocks; ++i) {
                const res::DataBlockHeader* pDataBlockHead = static_cast< const res::DataBlockHeader* >(dataPtr);
                const s32 blockSig = detail::GetSignatureInt(pDataBlockHead->kind);

                // PC_PORT: per-block breadcrumb (crash pinpointer).
                PL_LOG_INFO("compat.lyt", "Build: block %d/%d '%.4s' size %u", i, pFileHead->dataBlocks,
                            pDataBlockHead->kind, static_cast< unsigned >(pDataBlockHead->size));

                switch (blockSig) {
                case 'lyt1': {
                    const res::Layout* pResLyt = static_cast< const res::Layout* >(dataPtr);
                    _20 = pResLyt->originType != 0 ? ORIGINTYPE_CENTER : ORIGINTYPE_TOPLEFT;
                    mLayoutSize = pResLyt->layoutSize;
                    break;
                }
                case 'txl1':
                    resBlockSet.pTextureList = static_cast< const res::TextureList* >(dataPtr);
                    break;
                case 'fnl1':
                    resBlockSet.pFontList = static_cast< const res::FontList* >(dataPtr);
                    break;
                case 'mat1':
                    resBlockSet.pMaterialList = static_cast< const res::MaterialList* >(dataPtr);
                    break;

                case 'pan1':
                case 'pic1':
                case 'txt1':
                case 'wnd1':
                case 'bnd1': {
                    Pane* pPane = BuildPaneObj(blockSig, dataPtr, resBlockSet);

                    if (pPane != nullptr) {
                        if (mpRootPane == nullptr) {
                            mpRootPane = pPane;
                        }

                        if (pParentPane != nullptr) {
                            pParentPane->AppendChild(pPane);
                            PL_LOG_INFO("compat.lyt", "Build: pane attached to parent %p",
                                        static_cast< const void* >(pParentPane));
                        }

                        pLastPane = pPane;
                    }
                } break;
                case 'pas1':
                    pParentPane = pLastPane;
                    PL_LOG_INFO("compat.lyt", "Build: 'pas1' descend (parent %p)",
                                static_cast< const void* >(pParentPane));
                    break;
                case 'pae1':
                    // PC_PORT: upstream dereferences pLastPane unconditionally.
                    if (pParentPane == nullptr) {
                        PL_LOG_WARN("compat.lyt", "Build: 'pae1' without a matching 'pas1' parent (ignored)");
                        break;
                    }
                    pLastPane = pParentPane;
                    pParentPane = pLastPane->GetParent();
                    PL_LOG_INFO("compat.lyt", "Build: 'pae1' ascend (parent now %p)",
                                static_cast< const void* >(pParentPane));
                    break;
                case 'grp1': {
                    if (!bReadRootGroup) {
                        bReadRootGroup = true;
                        PL_LOG_INFO("compat.lyt", "Build: 'grp1' root -> creating GroupContainer ...");
                        mpGroupContainer = NewObj< GroupContainer >();
                        PL_LOG_INFO("compat.lyt", "Build: GroupContainer created (%p)",
                                    static_cast< const void* >(mpGroupContainer));
                    } else {
                        PL_LOG_INFO("compat.lyt", "Build: 'grp1' (nest level %d) -> creating Group ...", groupNestLevel);
                        if (mpGroupContainer != nullptr && groupNestLevel == 1) {
                            if (Group* pGroup = NewObj< Group >(reinterpret_cast< const res::Group* >(pDataBlockHead), mpRootPane)) {
                                PL_LOG_INFO("compat.lyt", "Build: Group created (%p) -> appending", static_cast< const void* >(pGroup));
                                mpGroupContainer->AppendGroup(pGroup);
                                PL_LOG_INFO("compat.lyt", "Build: Group appended");
                            }
                        }
                    }
                    break;
                }
                case 'grs1':
                    groupNestLevel++;
                    break;
                case 'gre1':
                    groupNestLevel--;
                    break;
                default:
                    break;
                }

                dataPtr = static_cast< const u8* >(dataPtr) + pDataBlockHead->size;
            }

            PL_LOG_INFO("compat.lyt", "Build: done (root pane %p, group container %p)",
                        static_cast< const void* >(mpRootPane), static_cast< const void* >(mpGroupContainer));
            return true;
        }

        AnimTransform* Layout::CreateAnimTransform() {
            AnimTransformBasic* const pAnimTrans = NewObj< AnimTransformBasic >();
            if (pAnimTrans != nullptr) {
                mAnimTransList.PushBack(pAnimTrans);
            }
            return pAnimTrans;
        }

        AnimTransform* Layout::CreateAnimTransform(const void* animResBuf, ResourceAccessor* pResAcsr) {
            return CreateAnimTransform(AnimResource(animResBuf), pResAcsr);
        }

        AnimTransform* Layout::CreateAnimTransform(const AnimResource& animRes, ResourceAccessor* pResAcsr) {
            const res::AnimationBlock* const pAnimBlock = animRes.GetResourceBlock();

            if (pAnimBlock == nullptr) {
                return nullptr;
            }

            AnimTransform* const pAnimTrans = CreateAnimTransform();
            if (pAnimTrans != nullptr) {
                pAnimTrans->SetResource(pAnimBlock, pResAcsr);
            }
            return pAnimTrans;
        }

        void Layout::BindAnimation(AnimTransform* pAnimTrans) {
            if (mpRootPane != nullptr) {
                mpRootPane->BindAnimation(pAnimTrans, true);
            }
        }

        void Layout::UnbindAnimation(AnimTransform* pAnimTrans) {
            if (mpRootPane != nullptr) {
                mpRootPane->UnbindAnimation(pAnimTrans, true);
            }
        }

        void Layout::UnbindAllAnimation() {
            UnbindAnimation(0);
        }

        /*
        bool Layout::BindAnimationAuto(const AnimResource &animRes, ResourceAccessor *pResAcsr) {
            if (mpRootPane == nullptr) {
                return false;
            }

            if (!animRes.GetResourceBlock()) {
                return false;
            }

            AnimTransform *const pAnimTrans = CreateAnimTransform();
            const u16 bindGroupNum = animRes.GetGroupNum();
            u16 animNum = 0;

            if (bindGroupNum == 0) {
                animNum = animRes.GetResourceBlock()->animContNum;
                pAnimTrans->SetResource(animRes.GetResourceBlock(), pResAcsr, animNum);
                const bool bRecursive = true;
                mpRootPane->BindAnimation(pAnimTrans, bRecursive, true);
            }
            else {
                const AnimationGroupRef* const groupRefs = animRes.GetGroupArray();
                for (int grpIdx = 0; grpIdx < bindGroupNum; ++grpIdx) {
                    Group *const pGroup = mpGroupContainer->FindGroupByName(groupRefs[grpIdx].GetName());
                    animNum += animRes.CalcAnimationNum(pGroup, animRes.IsDescendingBind());
                }

                pAnimTrans->SetResource(animRes.GetResourceBlock(), pResAcsr, animNum);

                for (int grpIdx = 0; grpIdx < bindGroupNum; ++grpIdx) {
                    Group* const pGroup = mpGroupContainer->FindGroupByName(groupRefs[grpIdx].GetName());
                    nw4r::lyt::BindAnimation(pGroup, pAnimTrans, animRes.IsDescendingBind(), true);
                }
            }

            const u16 animShareInfoNum = animRes.GetAnimationShareInfoNum();
            if (animShareInfoNum > 0) {
                const AnimationShareInfo* const animShareInfoAry = animRes.GetAnimationShareInfoArray();

                for (int i = 0; i < animShareInfoNum; ++i) {
                    Pane* const pSrcPane = mpRootPane->FindPaneByName(animShareInfoAry[i].GetSrcPaneName(), true);
                    detail::AnimPaneTree animPaneTree(pSrcPane, animRes);
                    if (!animPaneTree.IsEnabled()) {
                        continue;
                    }

                    Group* const pGroup = GetGroupContainer()->FindGroupByName(animShareInfoAry[i].GetTargetGroupName());
                    PaneLinkList& paneList = pGroup->GetPaneList();
                    u32 animIdx = 0;
                    for (PaneLinkList::Iterator it = paneList.GetBeginIter(); it != paneList.GetEndIter(); ++it, ++animIdx) {
                        if (it->mTarget != pSrcPane) {
                            if (bindGroupNum > 0) {
                                const bool bInclude = IsIncludeAnimationGroupRef(mpGroupContainer, animRes.GetGroupArray(), bindGroupNum,
        animRes.IsDescendingBind(), it->mTarget);

                                if (!bInclude) {
                                    continue;
                                }
                            }

                            animPaneTree.Bind(this, it->mTarget, pResAcsr);
                        }
                    }
                }
            }

            return true;
        }
        */

        void Layout::SetAnimationEnable(AnimTransform* pAnimTrans, bool bEnable) {
            if (mpRootPane != nullptr) {
                mpRootPane->SetAnimationEnable(pAnimTrans, bEnable, true);
            }
        }

        void Layout::CalculateMtx(const DrawInfo& rInfo) {
            if (mpRootPane == nullptr) {
                return;
            }

            mpRootPane->CalculateMtx(rInfo);
        }

        void Layout::Draw(const DrawInfo& rInfo) {
            if (mpRootPane == nullptr) {
                return;
            }

            mpRootPane->Draw(rInfo);
        }

        void Layout::Animate(u32 opt) {
            if (mpRootPane == nullptr) {
                return;
            }

            mpRootPane->Animate(opt);
        }

        const ut::Rect Layout::GetLayoutRect() const {
            if (_20 == 1) {
                return ut::Rect(-mLayoutSize.width / 2, mLayoutSize.height / 2, mLayoutSize.width / 2, -mLayoutSize.height / 2);
            }

            return ut::Rect(0.0f, 0.0f, mLayoutSize.width, mLayoutSize.height);
        }

        // have to do more headers to decomp these two
        // nw4r::lyt::Layout::SetTagProcessor
        // nw4r::lyt::Layout::BuildPaneObj
    };  // namespace lyt
};  // namespace nw4r
