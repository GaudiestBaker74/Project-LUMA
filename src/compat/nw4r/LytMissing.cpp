// =============================================================================
// compat/nw4r — symbols the Petari nw4r decompilation declares but never
// defines (the recurring "decomp omission" pattern), reconstructed for the
// host (M9.5.2).
//
//   * Layout::BuildPaneObj — petari left a `// nw4r::lyt::Layout::
//     BuildPaneObj` comment where the body should be. Reconstructed from the
//     Build() switch that calls it: 'pan1'/'bnd1'/'pic1'/'txt1'/'wnd1' map to
//     Pane/Bounding/Picture/TextBox/Window (ctor signatures per the lyt
//     headers). Placement is Layout::NewObj (mspAllocator), exactly like the
//     rest of Build().
//   * Layout::SetTagProcessor — this nw4r version's Layout has no tag
//     processor member (TextBox owns mpTagProcessor and the game sets it per
//     text box), so the virtual is a documented no-op to satisfy the vtable.
//   * detail::UnbindAnimationLink — reconstructed from its two call sites
//     (Pane/Material::UnbindAnimationSelf): drop every link of `pAnimTrans`
//     (or all links when null) from the pane/material animation list.
//   * AnimTransformBasic::Bind/Animate + AnimResource::Set + accessors — the
//     brlan animation half of the engine, reconstructed for M9.5.3b from the
//     RLAN format (mkwiiki BRLAN documentation + the res:: structs petari
//     kept). Pane-side: RLPA (SRT/size), RLVI (visibility), RLVC (vertex
//     colors + pane alpha). Material-side: RLTS (texture SRT), RLMC (material
//     /tev colors), RLTP (texture pattern via the SetResource file table).
//     Curves: hermite (cubic, per-key slopes) and step. Group/share binding
//     (BindAnimationAuto) stays unused by SMG's LayoutManager path.
// =============================================================================
#include <nw4r/lyt/animation.h>
#include <nw4r/lyt/group.h>
#include <nw4r/ut/CharWriter.h>
#include <nw4r/ut/TextWriterBase.h>
#include <nw4r/lyt/bounding.h>
#include <nw4r/lyt/common.h>
#include <nw4r/lyt/layout.h>
#include <nw4r/lyt/material.h>
#include <nw4r/lyt/pane.h>
#include <nw4r/lyt/picture.h>
#include <nw4r/lyt/textBox.h>
#include <nw4r/lyt/window.h>

#include "platform/Log/Log.h"

#include <cstring>

#include <revolution/tpl.h>

#include <cstring>

namespace nw4r {
    namespace lyt {

        Pane* Layout::BuildPaneObj(s32 sig, const void* pData, const ResBlockSet& rBlockSet) {
            // PC_PORT: breadcrumb for the first real-brlyt builds. Every pane
            // resource starts with the res::Pane header, whose 16-byte name
            // field is not guaranteed NUL-terminated.
            char nameBuf[17];
            std::memcpy(nameBuf, static_cast< const res::Pane* >(pData)->name, 16);
            nameBuf[16] = '\0';
            PL_LOG_INFO("compat.lyt", "BuildPaneObj: '%c%c%c%c' pane '%s' ...",
                        static_cast< char >((sig >> 24) & 0xFF), static_cast< char >((sig >> 16) & 0xFF),
                        static_cast< char >((sig >> 8) & 0xFF), static_cast< char >(sig & 0xFF), nameBuf);

            Pane* pPane = nullptr;

            switch (sig) {
            case 'pan1':
                pPane = NewObj< Pane >(static_cast< const res::Pane* >(pData));
                break;
            case 'pic1':
                pPane = NewObj< Picture >(static_cast< const res::Picture* >(pData), rBlockSet);
                break;
            case 'txt1':
                pPane = NewObj< TextBox >(static_cast< const res::TextBox* >(pData), rBlockSet);
                break;
            case 'wnd1':
                pPane = NewObj< Window >(static_cast< const res::Window* >(pData), rBlockSet);
                break;
            case 'bnd1':
                pPane = NewObj< Bounding >(static_cast< const res::Bounding* >(pData), rBlockSet);
                break;
            default:
                PL_LOG_WARN("compat.lyt", "BuildPaneObj: unknown pane signature 0x%08x",
                            static_cast<unsigned>(sig));
                return nullptr;
            }

            PL_LOG_INFO("compat.lyt", "BuildPaneObj: pane '%s' built (%p)", nameBuf,
                        static_cast< const void* >(pPane));
            return pPane;
        }

        void Layout::SetTagProcessor(ut::TagProcessorBase<wchar_t>* /*pTagProcessor*/) {
            // No-op by design: this nw4r revision stores tag processors per
            // TextBox (TextBox::mpTagProcessor), not per Layout. The virtual
            // exists only for SDK compatibility.
        }

        namespace detail {

            AnimationLink* FindAnimationLink(AnimationList* pAnimList, AnimTransform* pAnimTrans) {
                if (pAnimList == nullptr) {
                    return nullptr;
                }
                for (AnimationList::Iterator it = pAnimList->GetBeginIter();
                     it != pAnimList->GetEndIter(); ++it) {
                    AnimationLink& link = *it;
                    if (link.GetAnimTransform() == pAnimTrans) {
                        return &link;
                    }
                }
                return nullptr;
            }

            void UnbindAnimationLink(AnimationList* pAnimList, AnimTransform* pAnimTrans) {
                if (pAnimList == nullptr) {
                    return;
                }
                for (AnimationList::Iterator it = pAnimList->GetBeginIter();
                     it != pAnimList->GetEndIter();) {
                    AnimationList::Iterator curr = it++;
                    AnimationLink& link = *curr;
                    if (pAnimTrans == nullptr || link.GetAnimTransform() == pAnimTrans) {
                        link.Reset();
                        pAnimList->Erase(curr);
                    }
                }
            }

        } // namespace detail

        // --- brlan animation engine (M9.5.3b) ---------------------------------
        //
        // On-image structures for the pai1 payload that petari's resources.h
        // does not define (the decomp never needed them: Bind/Animate were
        // left body-less). Layout per the RLAN documentation; all fields are
        // host-native after Platform::CompatLyt::convertBrlan.

        namespace {

            // One animated object ("animation content" in the docs).
            struct BrlanContent {
                char name[0x14];   // pane (16B) or material (20B) name, NUL-padded
                u8 tagNum;
                u8 targetKind;     // 0 = pane, 1 = material
                u16 padding;
                // u32 tagOffsets[tagNum] follow (content-relative).

                const u32* GetTagOffsets() const {
                    return reinterpret_cast< const u32* >(this + 1);
                }
            };

            // One property group ('RLPA', 'RLTS', 'RLVI', 'RLVC', 'RLMC', 'RLTP').
            struct BrlanTag {
                char kind[4];
                u8 entryNum;
                u8 padding[3];
                // u32 entryOffsets[entryNum] follow (tag-relative).

                const u32* GetEntryOffsets() const {
                    return reinterpret_cast< const u32* >(this + 1);
                }
            };

            // One animated property curve.
            struct BrlanEntry {
                u8 index;     // texture slot (RLTS/RLTP)
                u8 target;    // property id (per-tag table below)
                u8 keyType;   // 1 = step (u16 value), 2 = hermite (f32 value+slope)
                u8 padding0;
                u16 keyNum;
                u16 padding1;
                u32 keysOffset;  // entry-relative (0x0C on real files)
            };

            bool tagKindIs(const BrlanTag* pTag, const char* pKind) {
                return std::memcmp(pTag->kind, pKind, 4) == 0;
            }

            const BrlanContent* GetContent(const res::AnimationBlock* pBlock, u32 idx) {
                const u32* const offs = detail::ConvertOffsToPtr< u32 >(pBlock, pBlock->animContOffsetsOffset);
                return detail::ConvertOffsToPtr< BrlanContent >(pBlock, offs[idx]);
            }

            const BrlanTag* GetTag(const BrlanContent* pContent, u32 idx) {
                return detail::ConvertOffsToPtr< BrlanTag >(pContent, pContent->GetTagOffsets()[idx]);
            }

            const BrlanEntry* GetEntry(const BrlanTag* pTag, u32 idx) {
                return detail::ConvertOffsToPtr< BrlanEntry >(pTag, pTag->GetEntryOffsets()[idx]);
            }

            // Cubic hermite over (frame, value, slope) keys: slope is the
            // per-key tangent, scaled by the segment length (standard brlan
            // evaluation, matches Benzin/Wii Layout Editor playback).
            f32 EvalHermite(f32 frame, const res::HermiteKey* pKeys, u32 keyNum) {
                if (keyNum == 0) {
                    return 0.0f;
                }
                if (keyNum == 1 || frame <= pKeys[0].frame) {
                    return pKeys[0].value;
                }
                if (frame >= pKeys[keyNum - 1].frame) {
                    return pKeys[keyNum - 1].value;
                }

                u32 i = 0;
                while (i + 2 < keyNum && frame >= pKeys[i + 1].frame) {
                    ++i;
                }

                const res::HermiteKey& k0 = pKeys[i];
                const res::HermiteKey& k1 = pKeys[i + 1];
                const f32 d = k1.frame - k0.frame;
                if (d <= 0.0f) {
                    return k1.value;
                }

                const f32 t = (frame - k0.frame) / d;
                const f32 t2 = t * t;
                const f32 t3 = t2 * t;
                return (2.0f * t3 - 3.0f * t2 + 1.0f) * k0.value +
                       (t3 - 2.0f * t2 + t) * (k0.slope * d) +
                       (-2.0f * t3 + 3.0f * t2) * k1.value +
                       (t3 - t2) * (k1.slope * d);
            }

            f32 EvalStep(f32 frame, const res::StepKey* pKeys, u32 keyNum) {
                if (keyNum == 0) {
                    return 0.0f;
                }
                f32 value = static_cast< f32 >(pKeys[0].value);
                for (u32 i = 1; i < keyNum; ++i) {
                    if (frame < pKeys[i].frame) {
                        break;
                    }
                    value = static_cast< f32 >(pKeys[i].value);
                }
                return value;
            }

            f32 EvalEntry(const BrlanEntry* pEntry, f32 frame) {
                const void* pKeys = detail::ConvertOffsToPtr< void >(pEntry, pEntry->keysOffset);

                if (pEntry->keyType == 2) {
                    return EvalHermite(frame, static_cast< const res::HermiteKey* >(pKeys), pEntry->keyNum);
                }
                if (pEntry->keyType == 1) {
                    return EvalStep(frame, static_cast< const res::StepKey* >(pKeys), pEntry->keyNum);
                }
                return 0.0f;
            }

            u8 ClampU8(f32 v) {
                return static_cast< u8 >(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
            }

            s16 ClampS10(f32 v) {
                return static_cast< s16 >(v < -1024.0f ? -1024.0f : (v > 1023.0f ? 1023.0f : v));
            }

        }  // namespace

        // Declared in animation.h, never defined by the decomp (only the
        // converting ctor is inline there).
        AnimResource::AnimResource()
            : mpFileHeader(nullptr), mpResBlock(nullptr), mpTagBlock(nullptr), mpShareBlock(nullptr) {
        }

        void AnimResource::Set(const void* pAnimResBuf) {
            mpFileHeader = nullptr;
            mpResBlock = nullptr;
            mpTagBlock = nullptr;
            mpShareBlock = nullptr;

            if (pAnimResBuf == nullptr) {
                return;
            }

            const res::BinaryFileHeader* const pFileHead = static_cast< const res::BinaryFileHeader* >(pAnimResBuf);
            if (!detail::TestFileHeader(*pFileHead, 'RLAN')) {
                PL_LOG_WARN("compat.lyt", "AnimResource::Set: not a host-native RLAN blob (%p)", pAnimResBuf);
                return;
            }

            mpFileHeader = pFileHead;

            const u8* pData = static_cast< const u8* >(pAnimResBuf) + pFileHead->headerSize;
            for (u16 i = 0; i < pFileHead->dataBlocks; ++i) {
                const res::DataBlockHeader* const pBlockHead = reinterpret_cast< const res::DataBlockHeader* >(pData);

                switch (detail::GetSignatureInt(pBlockHead->kind)) {
                case 'pai1':
                    mpResBlock = reinterpret_cast< const res::AnimationBlock* >(pBlockHead);
                    break;
                case 'tag1':
                    mpTagBlock = reinterpret_cast< const res::AnimationTagBlock* >(pBlockHead);
                    break;
                case 'shr1':
                    mpShareBlock = reinterpret_cast< const res::AnimationShareBlock* >(pBlockHead);
                    break;
                default:
                    break;
                }

                if (pBlockHead->size == 0) {
                    break;  // malformed; refuse to walk off
                }
                pData += pBlockHead->size;
            }

            if (mpResBlock == nullptr) {
                PL_LOG_WARN("compat.lyt", "AnimResource::Set: RLAN holds no pai1 block");
            }
        }

        bool AnimResource::IsDescendingBind() const {
            return mpTagBlock != nullptr && (mpTagBlock->flag & 1) != 0;
        }

        u16 AnimResource::GetGroupNum() const {
            return mpTagBlock != nullptr ? mpTagBlock->groupNum : 0;
        }

        const AnimationGroupRef* AnimResource::GetGroupArray() const {
            if (mpTagBlock == nullptr) {
                return nullptr;
            }
            return detail::ConvertOffsToPtr< AnimationGroupRef >(mpTagBlock, mpTagBlock->groupsOffset);
        }

        u16 AnimResource::GetAnimationShareInfoNum() const {
            return mpShareBlock != nullptr ? mpShareBlock->shareNum : 0;
        }

        const AnimationShareInfo* AnimResource::GetAnimationShareInfoArray() const {
            if (mpShareBlock == nullptr) {
                return nullptr;
            }
            return detail::ConvertOffsToPtr< AnimationShareInfo >(mpShareBlock, mpShareBlock->animShareInfoOffset);
        }

        u16 AnimResource::CalcAnimationNum(Pane* pPane, bool bRecursive) const {
            if (mpResBlock == nullptr || pPane == nullptr) {
                return 0;
            }

            u16 num = 0;
            const u16 contNum = mpResBlock->animContNum;
            for (u16 i = 0; i < contNum; ++i) {
                const BrlanContent* pContent = GetContent(mpResBlock, i);
                if (pContent->targetKind == 0) {
                    if (detail::EqualsResName(pContent->name, pPane->mName)) {
                        ++num;
                    }
                } else if (pPane->mpMaterial != nullptr) {
                    if (detail::EqualsMaterialName(pContent->name, pPane->mpMaterial->GetName())) {
                        ++num;
                    }
                }
            }

            if (bRecursive) {
                for (PaneList::Iterator it = pPane->mChildList.GetBeginIter();
                     it != pPane->mChildList.GetEndIter(); ++it) {
                    num = static_cast< u16 >(num + CalcAnimationNum(&*it, true));
                }
            }
            return num;
        }

        u16 AnimResource::CalcAnimationNum(Material* pMaterial) const {
            if (mpResBlock == nullptr || pMaterial == nullptr) {
                return 0;
            }

            u16 num = 0;
            const u16 contNum = mpResBlock->animContNum;
            for (u16 i = 0; i < contNum; ++i) {
                const BrlanContent* pContent = GetContent(mpResBlock, i);
                if (pContent->targetKind == 1 &&
                    detail::EqualsMaterialName(pContent->name, pMaterial->GetName())) {
                    ++num;
                }
            }
            return num;
        }

        u16 AnimResource::CalcAnimationNum(Group* pGroup, bool bDescendingBind) const {
            if (mpResBlock == nullptr || pGroup == nullptr) {
                return 0;
            }

            u16 num = 0;
            PaneLinkList& paneList = pGroup->GetPaneList();
            for (PaneLinkList::Iterator it = paneList.GetBeginIter(); it != paneList.GetEndIter(); ++it) {
                num = static_cast< u16 >(num + CalcAnimationNum(it->mTarget, bDescendingBind));
            }
            return num;
        }

        namespace {

            // Free link slot in the transform's preallocated array (SetResource
            // sized it to animContNum, which bounds the matches for one layout
            // tree: every content names a single pane/material).
            AnimationLink* AllocAnimLink(AnimTransformBasic* pSelf) {
                for (u16 i = 0; i < pSelf->mAnimLinkNum; ++i) {
                    if (pSelf->mAnimLinkAry[i].GetAnimTransform() == nullptr) {
                        return &pSelf->mAnimLinkAry[i];
                    }
                }
                return nullptr;
            }

        }  // namespace

        void AnimTransformBasic::Bind(Pane* pPane, bool bRecursive) {
            if (mpRes == nullptr || pPane == nullptr) {
                return;
            }

            const u16 contNum = mpRes->animContNum;
            for (u16 i = 0; i < contNum; ++i) {
                const BrlanContent* pContent = GetContent(mpRes, i);

                if (pContent->targetKind == 0) {
                    if (!detail::EqualsResName(pContent->name, pPane->mName)) {
                        continue;
                    }
                } else {
                    Material* const pMaterial = pPane->GetMaterial();
                    if (pMaterial == nullptr ||
                        !detail::EqualsMaterialName(pContent->name, pMaterial->GetName())) {
                        continue;
                    }
                }

                AnimationLink* const pLink = AllocAnimLink(this);
                if (pLink == nullptr) {
                    PL_LOG_WARN("compat.lyt", "AnimTransformBasic::Bind: link array exhausted (content %u)",
                                static_cast< unsigned >(i));
                    break;
                }
                pLink->Set(this, i, false);

                if (pContent->targetKind == 0) {
                    pPane->AddAnimationLink(pLink);
                } else {
                    pPane->GetMaterial()->AddAnimationLink(pLink);
                }
            }

            if (bRecursive) {
                for (PaneList::Iterator it = pPane->mChildList.GetBeginIter();
                     it != pPane->mChildList.GetEndIter(); ++it) {
                    it->BindAnimation(this, bRecursive);
                }
            }
        }

        void AnimTransformBasic::Bind(Material* pMaterial) {
            if (mpRes == nullptr || pMaterial == nullptr) {
                return;
            }

            const u16 contNum = mpRes->animContNum;
            for (u16 i = 0; i < contNum; ++i) {
                const BrlanContent* pContent = GetContent(mpRes, i);
                if (pContent->targetKind != 1 ||
                    !detail::EqualsMaterialName(pContent->name, pMaterial->GetName())) {
                    continue;
                }

                AnimationLink* const pLink = AllocAnimLink(this);
                if (pLink == nullptr) {
                    PL_LOG_WARN("compat.lyt", "AnimTransformBasic::Bind(Material): link array exhausted");
                    break;
                }
                pLink->Set(this, i, false);
                pMaterial->AddAnimationLink(pLink);
            }
        }

        void AnimTransformBasic::Animate(u32 contentIdx, Pane* pPane) {
            if (mpRes == nullptr || pPane == nullptr || contentIdx >= mpRes->animContNum) {
                return;
            }

            const BrlanContent* const pContent = GetContent(mpRes, contentIdx);
            if (pContent->targetKind != 0 || !detail::EqualsResName(pContent->name, pPane->mName)) {
                return;
            }

            const f32 frame = mFrame;

            for (u32 t = 0; t < pContent->tagNum; ++t) {
                const BrlanTag* const pTag = GetTag(pContent, t);

                for (u32 e = 0; e < pTag->entryNum; ++e) {
                    const BrlanEntry* const pEntry = GetEntry(pTag, e);
                    const f32 value = EvalEntry(pEntry, frame);

                    if (tagKindIs(pTag, "RLPA")) {
                        switch (pEntry->target) {
                        case 0x00: pPane->mTranslate.x = value; break;
                        case 0x01: pPane->mTranslate.y = value; break;
                        case 0x02: pPane->mTranslate.z = value; break;
                        case 0x03: pPane->mRotate.x = value; break;
                        case 0x04: pPane->mRotate.y = value; break;
                        case 0x05: pPane->mRotate.z = value; break;
                        case 0x06: pPane->mScale.x = value; break;
                        case 0x07: pPane->mScale.y = value; break;
                        case 0x08: pPane->mSize.width = value; break;
                        case 0x09: pPane->mSize.height = value; break;
                        default: break;
                        }
                    } else if (tagKindIs(pTag, "RLVI")) {
                        // Visibility: bit 0 of the pane flags.
                        if (value != 0.0f) {
                            pPane->mFlag = static_cast< u8 >(pPane->mFlag | 1);
                        } else {
                            pPane->mFlag = static_cast< u8 >(pPane->mFlag & ~1u);
                        }
                    } else if (tagKindIs(pTag, "RLVC")) {
                        // Targets 0x00-0x0F are the four corner colors, 0x10 is
                        // the pane alpha — exactly Pane::SetColorElement's
                        // switch (0x10 -> mAlpha, default -> vtx color).
                        pPane->SetColorElement(pEntry->target, ClampU8(value));
                    }
                    // Material-side tags on a pane content: ignored.
                }
            }
        }

        void AnimTransformBasic::Animate(u32 contentIdx, Material* pMaterial) {
            if (mpRes == nullptr || pMaterial == nullptr || contentIdx >= mpRes->animContNum) {
                return;
            }

            const BrlanContent* const pContent = GetContent(mpRes, contentIdx);
            if (pContent->targetKind != 1 ||
                !detail::EqualsMaterialName(pContent->name, pMaterial->GetName())) {
                return;
            }

            const f32 frame = mFrame;

            for (u32 t = 0; t < pContent->tagNum; ++t) {
                const BrlanTag* const pTag = GetTag(pContent, t);

                for (u32 e = 0; e < pTag->entryNum; ++e) {
                    const BrlanEntry* const pEntry = GetEntry(pTag, e);
                    const f32 value = EvalEntry(pEntry, frame);

                    if (tagKindIs(pTag, "RLTS")) {
                        const u32 texIdx = pEntry->index;
                        if (texIdx >= pMaterial->GetTextureNum()) {
                            continue;
                        }
                        TexSRT& srt = pMaterial->GetTexSRTAry()[texIdx];
                        switch (pEntry->target) {
                        case 0x00: srt.translate.x = value; break;
                        case 0x01: srt.translate.y = value; break;
                        case 0x02: srt.rotate = value; break;
                        case 0x03: srt.scale.x = value; break;
                        case 0x04: srt.scale.y = value; break;
                        default: break;
                        }
                    } else if (tagKindIs(pTag, "RLMC")) {
                        const u32 target = pEntry->target;
                        if (target < 0x04) {
                            if (pMaterial->IsMatColorCap()) {
                                ut::Color* pCol = &pMaterial->GetMatColAry()[0];
                                switch (target) {
                                case 0: pCol->r = ClampU8(value); break;
                                case 1: pCol->g = ClampU8(value); break;
                                case 2: pCol->b = ClampU8(value); break;
                                default: pCol->a = ClampU8(value); break;
                                }
                            }
                        } else if (target < 0x10) {
                            GXColorS10& col = pMaterial->mTevCols[(target - 0x04) / 4];
                            switch (target % 4) {
                            case 0: col.r = ClampS10(value); break;
                            case 1: col.g = ClampS10(value); break;
                            case 2: col.b = ClampS10(value); break;
                            default: col.a = ClampS10(value); break;
                            }
                        } else if (target < 0x20) {
                            ut::Color& col = pMaterial->mTevKCols[(target - 0x10) / 4];
                            switch (target % 4) {
                            case 0: col.r = ClampU8(value); break;
                            case 1: col.g = ClampU8(value); break;
                            case 2: col.b = ClampU8(value); break;
                            default: col.a = ClampU8(value); break;
                            }
                        }
                    } else if (tagKindIs(pTag, "RLTP")) {
                        // Texture pattern: step value = index into the file
                        // table SetResource resolved through the accessor.
                        const u32 texIdx = pEntry->index;
                        const u32 fileIdx = static_cast< u32 >(value);
                        if (texIdx >= pMaterial->GetTextureNum() || mpFileResAry == nullptr ||
                            fileIdx >= mpRes->fileNum || mpFileResAry[fileIdx] == nullptr) {
                            continue;
                        }
                        pMaterial->GetTexMapAry()[texIdx].ReplaceImage(
                            static_cast< TPLPalettePtr >(mpFileResAry[fileIdx]), 0);
                    }
                    // Pane-side tags on a material content: ignored.
                }
            }
        }

        // Declared in lyt/types.h, never defined by the decomp. Plain
        // memberwise copy (the declaration exists so the copy stays out of
        // the paired-singles inline path on the console).
        TexSRT& TexSRT::operator=(const TexSRT& other) {
            translate = other.translate;
            rotate = other.rotate;
            scale = other.scale;
            return *this;
        }

    } // namespace lyt

    namespace ut {

        // (mDefaultTagProcessor now lives in the patched ut_TextWriterBase.cpp,
        // next to the explicit instantiations that reference it.)

        // Declared in CharWriter.h, never defined by the decomp. Zero-init is
        // safe: the first LoadTexture compares against it and always loads
        // (texture pointers differ), matching the post-Reset() behaviour.
        CharWriter::LoadingTexture CharWriter::mLoadingTexture;

        // Declared in CharWriter.h next to the one-color overload (which the
        // decomp does define); the two-color variant sets the gradation
        // endpoints — gradationMode itself is set via SetGradationMode.
        void CharWriter::SetTextColor(Color start, Color end) {
            mTextColor.start = start;
            mTextColor.end = end;
            UpdateVertexColor();
        }

    } // namespace ut
} // namespace nw4r
