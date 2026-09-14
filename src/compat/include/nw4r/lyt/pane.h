#pragma once
// =============================================================================
// PC_PORT PATCH of the vendored libs/nw4r/include/nw4r/lyt/pane.h (see
// src/compat/patches/README.md). This shadow copy wins over the vendored
// header through the compat include path.
//
// Change vs. upstream: the non-Metrowerks LinkList typedef used offset 0
// instead of offsetof(..., mLink). That is only valid when mLink is the
// first member — but Pane derives from detail::PaneBase, whose virtual destructor puts a
// vptr before mLink.
// With offset 0, LinkList::PushBack writes the prev/next pointers over the
// vptr at offset 0 and the first virtual call on the linked object crashes.
// offsetof on a polymorphic type is conditionally-supported and accepted by
// GCC/Clang/MSVC (it may warn), and yields the correct post-vptr offset.
//
// Everything else is identical to upstream.
// =============================================================================

#include "nw4r/lyt/resources.h"
#include "nw4r/lyt/types.h"
#include "nw4r/math/types.h"
#include "nw4r/ut/Color.h"
#include "nw4r/ut/LinkList.h"
#include "nw4r/ut/RuntimeTypeInfo.h"
#include <cstddef>

namespace nw4r {
    namespace ut {
        struct Rect;
    };

    namespace lyt {
        namespace detail {
            class PaneBase {
            public:
                PaneBase();
                virtual ~PaneBase();

                ut::LinkListNode mLink;
            };
        };  // namespace detail

        class AnimTransform;
        class AnimationLink;
        class AnimResource;
        class Material;
        class DrawInfo;
        class Pane;

#ifdef __MWERKS__
        typedef ut::LinkList< Pane, offsetof(detail::PaneBase, mLink) > PaneList;
#else
        typedef ut::LinkList< Pane, offsetof(detail::PaneBase, mLink) > PaneList;
#endif

        class Pane : public detail::PaneBase {
        public:
            NW4R_UT_RUNTIME_TYPEINFO;

            Pane(const res::Pane*);
            virtual ~Pane();
            virtual void CalculateMtx(const DrawInfo&);
            virtual void Draw(const DrawInfo&);
            virtual void DrawSelf(const DrawInfo&);
            virtual void Animate(u32);
            virtual void AnimateSelf(u32);
            virtual const ut::Color GetVtxColor(u32) const;
            virtual void SetVtxColor(u32, ut::Color);
            virtual u8 GetColorElement(u32) const;
            virtual void SetColorElement(u32, u8);
            virtual u8 GetVtxColorElement(u32) const;
            virtual void SetVtxColorElement(u32, u8);
            virtual Pane* FindPaneByName(const char*, bool);
            virtual Material* FindMaterialByName(const char*, bool);
            virtual void BindAnimation(AnimTransform*, bool);
            virtual void UnbindAnimation(AnimTransform*, bool);
            virtual void UnbindAllAnimation(bool);
            virtual void UnbindAnimationSelf(AnimTransform*);
            virtual AnimationLink* FindAnimationLinkSelf(AnimTransform*);
            virtual void SetAnimationEnable(AnimTransform*, bool, bool);
            virtual Material* GetMaterial() const;
            virtual void LoadMtx(const DrawInfo&);

            void Init();
            void SetName(const char*);
            void SetUserData(const char*);

            bool IsUserAllocated() const { return mbUserAllocated; }
            bool IsInfluencedAlpha() const { return detail::TestBit(mFlag, 1); }
            bool IsLocationAdjust() const { return detail::TestBit(mFlag, 2); }

            Pane* GetParent() const { return mpParent; }

            void InsertChild(PaneList::Iterator, Pane*);
            void RemoveChild(Pane*);
            void AppendChild(Pane*);
            void AddAnimationLink(AnimationLink*);
            math::VEC2 GetVtxPos() const;
            void CalculateMtxChild(const DrawInfo& rInfo);

            bool IsVisible() const { return detail::TestBit(mFlag, 0); }

            /// PC_PORT (M10.1): nw4r's Pane::SetVisible. The vendored game code
            /// only ever READS the flag (the console toggles panes through the
            /// animation system), but the host file-select screen has to show
            /// and hide the operation buttons / info bar by hand while the
            /// ButtonPaneController machinery is not ported. Same bit nw4r
            /// uses (mFlag bit 0), so animations that drive visibility keep
            /// working on top of it.
            void SetVisible(bool visible) {
                if (visible) {
                    mFlag |= 1;
                } else {
                    mFlag &= static_cast< u8 >(~1);
                }
            }

            /// PC_PORT (M10.1): the pane's box in layout units, Y-up, with the
            /// origin at its top-left corner (nw4r's convention: the box
            /// extends +width to the right and -height downwards).
            void GetBox(f32* outMinX, f32* outMinY, f32* outMaxX, f32* outMaxY) const {
                if (outMinX != nullptr) {
                    *outMinX = mGlbMtx._03;
                }
                if (outMinY != nullptr) {
                    *outMinY = mGlbMtx._13 - mSize.height;
                }
                if (outMaxX != nullptr) {
                    *outMaxX = mGlbMtx._03 + mSize.width;
                }
                if (outMaxY != nullptr) {
                    *outMaxY = mGlbMtx._13;
                }
            }

            Pane* mpParent;
            PaneList mChildList;
            AnimationList mAnimList;
            Material* mpMaterial;
            math::VEC3 mTranslate;
            math::VEC3 mRotate;
            math::VEC2 mScale;
            Size mSize;
            math::MTX34 mMtx;
            math::MTX34 mGlbMtx;
            u8 mAlpha;
            u8 mGlbAlpha;
            u8 mBasePosition;
            u8 mFlag;
            char mName[17];
            char mUserData[9];
            u8 mbUserAllocated;
            u8 mPadding;
        };
    };  // namespace lyt
};  // namespace nw4r
