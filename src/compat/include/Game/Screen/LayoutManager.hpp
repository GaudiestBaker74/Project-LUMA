#pragma once

// =============================================================================
// PC_PORT SHADOW of the vendored include/Game/Screen/LayoutManager.hpp (M9.5.3).
//
// The method surface is identical to the decompilation. The member block is a
// host reconstruction: the console layout (offsets 0x64..0x78, all unnamed in
// petari) is replaced by named fields used by the reconstructed implementation
// in src/compat/game/LayoutManagerCompat.cpp. Nothing in the port depends on
// the original object layout, so this is ABI-safe.
// =============================================================================

#include <JSystem/JGeometry/TVec.hpp>
#include <nw4r/lyt/drawInfo.h>

namespace nw4r {
    namespace lyt {
        class AnimTransform;
        class Group;
        class Layout;
        class Pane;
    };  // namespace lyt
};  // namespace nw4r

class LayoutGroupCtrl;
class LayoutHolder;
class LayoutPaneCtrl;

class LayoutManager {
public:
    LayoutManager(const char*, bool, u32, u32);

    void movement();
    void calcAnim();
    void draw() const;
    void addPaneCtrl(LayoutPaneCtrl*);
    LayoutPaneCtrl* createAndAddRootPaneCtrl(u32);
    LayoutPaneCtrl* createAndAddPaneCtrl(const char*, u32);
    LayoutPaneCtrl* getPaneCtrl(const char*) const;
    s32 getIndexOfPane(const char*) const;
    bool isExistPaneCtrl(const char*) const;
    void addGroupCtrl(LayoutGroupCtrl*);
    bool isPointing(const nw4r::lyt::Pane*, const TVec2f&) const;
    LayoutPaneCtrl* createAndAddGroupCtrl(const char*, u32);
    s32 getIndexOfGroupCtrl(const char*) const;
    void createPaneMtxRef(const char*);
    MtxPtr getPaneMtxRef(const char*) const;
    bool isExistPaneMtxRef(const char*) const;
    bool isPointing(const char*, const TVec2f&) const;
    nw4r::lyt::AnimTransform* getAnimTransform(const char*) const;
    void bindPaneCtrlAnim(LayoutPaneCtrl*, nw4r::lyt::AnimTransform*);
    void bindPaneCtrlAnimSub(u32&, nw4r::lyt::AnimTransform*);
    void unbindPaneCtrlAnim(LayoutPaneCtrl*, nw4r::lyt::AnimTransform*);
    void unbindPaneCtrlAnimSub(u32&, nw4r::lyt::AnimTransform*);
    void calcAnimWithoutLocationAdjust(const nw4r::lyt::DrawInfo&);
    nw4r::lyt::Group* getGroup(const char*) const;
    void initArc(const char*, const char*);
    void initDrawInfo();
    void initPaneInfo();
    void initPaneInfoRecursive(u32&, nw4r::lyt::Pane*);
    u32 countPanes(nw4r::lyt::Pane*);
    void initGroupCtrlList();
    void initTextBoxRecursive(nw4r::lyt::Pane*, nw4r::lyt::Pane*, const char*, u32);
    void animateRecursive(u32&, nw4r::lyt::Pane*);
    nw4r::lyt::Pane* getPane(const char*) const;
    nw4r::lyt::Pane* findPaneByName(const char*) const;
    void replaceIndDummyTexture();
    void removeUnnecessaryPanes(nw4r::lyt::Pane*);

    // --- console members (same meaning as the decompilation) ---
    /* 0x00 */ LayoutHolder* mLayoutHolder;
    /* 0x04 */ nw4r::lyt::Layout* mLayout;
    /* 0x08 */ nw4r::lyt::AnimTransform** mAnimTransList;
    /* 0x0C */ nw4r::lyt::DrawInfo mDrawInfo;
    /* 0x60 */ bool mIsScreenHidden;
    /* 0x61 */ bool _61;

    // --- PC_PORT reconstruction (replaces the unnamed 0x64..0x78 block) ---
    u32 mAnimLayerNum;              // anim layers per pane controller (ctor param 3)
    u32 mTextBoxBufferLength;       // text box string buffer length (ctor param 4)
    LayoutPaneCtrl* mRootPaneCtrl;  // controller of the root pane (anim layers 0..N)
    LayoutPaneCtrl** mPaneCtrls;    // every created pane controller (root at [0])
    u32 mPaneCtrlNum;
    u32 mAnimTransNum;              // entries in mAnimTransList (brlans in the arc)
    struct PaneMtxRef {
        const char* mName;
        MtxPtr mMtx;
    };
    PaneMtxRef* mPaneMtxRefs;       // createPaneMtxRef/getPaneMtxRef storage
    u32 mPaneMtxRefNum;
    const char* mLayoutName;        // layout (brlyt) name, host-allocated copy
};
