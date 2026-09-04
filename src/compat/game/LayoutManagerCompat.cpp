// =============================================================================
// PC_PORT reconstruction of LayoutManager + the layout subset of MR::
// (M9.5.3a — the real layout stack: load & build).
//
// petari does not decompile LayoutManager at all (no .cpp, header members
// 0x64..0x78 unnamed), so this file reconstructs it against the real game
// behavior observed from its callers:
//
//   * LayoutActor::initLayoutManager(name, n) -> LayoutManager(name, true, n, 0x100)
//     mounts /LayoutData/<name>.arc (M9.5.1 archive stack), wraps it in a
//     LayoutHolder (patched copy, this milestone), converts the brlyt with the
//     M9.5.2 big-endian swapper and builds it with nw4r::lyt::Layout::Build.
//     Every brlan in the arc becomes an AnimTransform via
//     Layout::CreateAnimTransform (null until the brlan half lands in
//     M9.5.3b — LayoutAnmPlayer tolerates that).
//   * The animation layers of an actor live in the ROOT LayoutPaneCtrl
//     (MR::isAnimStopped/getAnimCtrl route through getPaneCtrl(nullptr)), so
//     initPaneInfo creates the root controller with the ctor's layer count.
//     Per-pane controllers, group controllers, text-box buffers and drawing
//     are deferred to M9.5.3c and log once when touched.
//   * movement() advances the players' frame controllers; calcAnim() reflects
//     the frames into the transforms and runs Layout::Animate/CalculateMtx.
//
// The MR:: helpers at the bottom are verbatim copies of the decompiled bodies
// in petari's Game/Util/LayoutUtil.cpp / Game/Screen/LayoutCoreUtil.cpp (which
// are not host-compiled: they drag in the message and effect systems). The
// few functions petari left body-less (copyPaneTrans, startAnim) are
// reconstructed from their call sites and documented inline.
//
// Null-safety rule: every method tolerates a null mLayout (missing arc, e.g.
// asset-less test runs) so the boot degrades gracefully instead of crashing.
// =============================================================================
#include "Game/Screen/LayoutManager.hpp"

#include "Game/Animation/LayoutAnmPlayer.hpp"
#include "Game/LiveActor/Spine.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Screen/LayoutPaneCtrl.hpp"
#include "Game/Screen/StarPointerTarget.hpp"
#include "Game/System/LayoutHolder.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"

#include <JSystem/JKernel/JKRArchive.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>
#include <JSystem/JUtility/JUTVideo.hpp>
#include <nw4r/lyt/animation.h>
#include <nw4r/lyt/group.h>
#include <nw4r/lyt/layout.h>
#include <nw4r/lyt/material.h>
#include <nw4r/lyt/pane.h>
#include <nw4r/lyt/texMap.h>

#include "compat/nw4r/LytHost.h"
#include "platform/Log/Log.h"

#include <cstdio>
#include <cstring>

namespace {
    // SMG's layout/screen coordinate space for 4:3 (see ScreenUtil.cpp).
    const f32 cLayoutSpaceHeight = 456.0f;

    // Builds the on-disc archive path for a layout name. convertFilename=true
    // (the usual SimpleLayout path) maps "TitleLogo" to "/LayoutData/TitleLogo.arc";
    // false keeps the caller-provided name as the arc path (that is what
    // initLayoutManagerNoConvertFilename is for).
    void buildLayoutArcPath(char* pOut, size_t outSize, const char* pName, bool convertFilename) {
        if (convertFilename) {
            snprintf(pOut, outSize, "/LayoutData/%s.arc", pName);
        } else {
            snprintf(pOut, outSize, "%s", pName);
        }
    }

    char* copyString(const char* pStr) {
        if (pStr == nullptr) {
            return nullptr;
        }

        size_t len = strlen(pStr) + 1;
        char* pCopy = new char[len];
        memcpy(pCopy, pStr, len);
        return pCopy;
    }

    // Logs "not ported yet" warnings a bounded number of times so per-frame
    // callers cannot flood the log.
    void logOnceUnsupported(const char* pWhat) {
        static int sLoggedNum = 0;

        if (sLoggedNum < 16) {
            sLoggedNum++;
            PL_LOG_WARN("compat.layout", "LayoutManager: %s not ported yet (M9.5.3c+)", pWhat);
        }
    }
};  // namespace

// =============================================================================
// LayoutManager
// =============================================================================

LayoutManager::LayoutManager(const char* pName, bool convertFilename, u32 animLayerNum, u32 textBoxBufferLength)
    : mLayoutHolder(nullptr), mLayout(nullptr), mAnimTransList(nullptr), mDrawInfo(), mIsScreenHidden(false),
      _61(false), mAnimLayerNum(animLayerNum), mTextBoxBufferLength(textBoxBufferLength), mRootPaneCtrl(nullptr),
      mPaneCtrls(nullptr), mPaneCtrlNum(0), mAnimTransNum(0), mPaneMtxRefs(nullptr), mPaneMtxRefNum(0),
      mLayoutName(copyString(pName)) {
    char arcPath[256];
    buildLayoutArcPath(arcPath, sizeof(arcPath), pName, convertFilename);

    initArc(arcPath, pName);
    initDrawInfo();
    initPaneInfo();
    initGroupCtrlList();
}

void LayoutManager::initArc(const char* pArcPath, const char* pLayoutName) {
    JKRMemArchive* pArchive = MR::mountArchive(pArcPath, nullptr);

    if (pArchive == nullptr) {
        PL_LOG_ERROR("compat.layout", "LayoutManager '%s': cannot mount '%s' (layout will stay empty)",
                     mLayoutName != nullptr ? mLayoutName : "?", pArcPath);
        return;
    }

    mLayoutHolder = new LayoutHolder(*pArchive);

    void* pBrlyt = mLayoutHolder->mLayoutRes.getRes(pLayoutName);

    if (pBrlyt == nullptr && mLayoutHolder->mLayoutRes.mCount > 0) {
        // Some arcs name the brlyt differently from the arc itself; fall back
        // to the single layout resource in the arc.
        pBrlyt = mLayoutHolder->mLayoutRes.getRes(0u);
        PL_LOG_WARN("compat.layout", "LayoutManager '%s': brlyt '%s' not found; using '%s' instead",
                    mLayoutName != nullptr ? mLayoutName : "?", pLayoutName,
                    mLayoutHolder->mLayoutRes.getResName(0u));
    }

    if (pBrlyt == nullptr) {
        PL_LOG_ERROR("compat.layout", "LayoutManager '%s': arc '%s' holds no brlyt", mLayoutName != nullptr ? mLayoutName : "?",
                     pArcPath);
        return;
    }

    // The archive hands out the big-endian Wii blob; the M9.5.2 swapper
    // converts it in place (idempotent — already-converted data is a no-op).
    s32 brlytSize = pArchive->getResSize(pBrlyt);

    if (!Platform::CompatLyt::convertBrlyt(pBrlyt, static_cast< u32 >(brlytSize > 0 ? brlytSize : 0))) {
        PL_LOG_ERROR("compat.layout", "LayoutManager '%s': brlyt conversion failed", mLayoutName != nullptr ? mLayoutName : "?");
        return;
    }

    mLayout = nw4r::lyt::Layout::NewObj< nw4r::lyt::Layout >();

    if (!mLayout->Build(pBrlyt, mLayoutHolder)) {
        PL_LOG_ERROR("compat.layout", "LayoutManager '%s': Layout::Build failed", mLayoutName != nullptr ? mLayoutName : "?");
        return;
    }

    mAnimTransNum = mLayoutHolder->mAnimRes.mCount;

    if (mAnimTransNum > 0) {
        mAnimTransList = new nw4r::lyt::AnimTransform*[mAnimTransNum];

        u32 created = 0;

        for (u32 i = 0; i < mAnimTransNum; i++) {
            void* pBrlan = mLayoutHolder->mAnimRes.getRes(i);

            // The archive hands out the big-endian Wii blob; the M9.5.3b
            // swapper converts it in place (idempotent) so AnimResource::Set
            // and the curve readers see host-native data.
            s32 brlanSize = pArchive->getResSize(pBrlan);

            if (!Platform::CompatLyt::convertBrlan(pBrlan, static_cast< u32 >(brlanSize > 0 ? brlanSize : 0))) {
                PL_LOG_WARN("compat.layout", "LayoutManager '%s': brlan #%u conversion failed (its animation stays unbound)",
                            mLayoutName != nullptr ? mLayoutName : "?", static_cast< unsigned >(i));
            }

            mAnimTransList[i] = mLayout->CreateAnimTransform(pBrlan, mLayoutHolder);

            if (mAnimTransList[i] != nullptr) {
                created++;
            }
        }

        // created < mAnimTransNum only happens for corrupt/unconvertible
        // brlans now (M9.5.3b parses them for real).
        PL_LOG_INFO("compat.layout", "LayoutManager '%s': %u/%u anim transforms created from brlans",
                    mLayoutName != nullptr ? mLayoutName : "?", created, mAnimTransNum);
    }

    PL_LOG_INFO("compat.layout", "LayoutManager '%s': built from '%s' (%u textures/fonts in arc)",
                mLayoutName != nullptr ? mLayoutName : "?", pArcPath, mLayoutHolder->getResOtherNum());
}

void LayoutManager::initDrawInfo() {
    // The DrawInfo ctor already sets identity view mtx, alpha 1 and cleared
    // flags. The view rect spans the SMG layout space (608 x 456 for 4:3),
    // centered on the origin, with the SAME sign convention as
    // Layout::GetLayoutRect: top > bottom. That ordering matters —
    // DrawInfo::IsYAxisUp() is `bottom - top < 0`, and Pane::LoadMtx only
    // reverses the Y axis (layout space is Y-down, the screen ortho set by
    // MR::setupDrawForNW4RLayout is Y-up) when the DrawInfo says Y-up.
    mDrawInfo.mViewRect = nw4r::ut::Rect(-304.0f, cLayoutSpaceHeight * 0.5f, 304.0f,
                                         -cLayoutSpaceHeight * 0.5f);
}

void LayoutManager::initPaneInfo() {
    // The root pane controller carries the actor's anim layers: every
    // MR::startAnim/isAnimStopped/... call routes through getPaneCtrl(nullptr),
    // so it must exist even when the arc failed to mount (the players then
    // bind null transforms, which LayoutAnmPlayer tolerates).
    // Per-pane controllers (initPaneInfoRecursive over the whole tree) return
    // with pane-level animation support in M9.5.3c+.
    mRootPaneCtrl = createAndAddRootPaneCtrl(mAnimLayerNum);
}

void LayoutManager::initPaneInfoRecursive(u32&, nw4r::lyt::Pane*) {
    logOnceUnsupported("per-pane controllers (initPaneInfoRecursive)");
}

u32 LayoutManager::countPanes(nw4r::lyt::Pane*) {
    logOnceUnsupported("countPanes");
    return 0;
}

void LayoutManager::initGroupCtrlList() {
    if (mLayout != nullptr && mLayout->GetGroupContainer() != nullptr) {
        logOnceUnsupported("group controllers (initGroupCtrlList)");
    }
}

void LayoutManager::initTextBoxRecursive(nw4r::lyt::Pane*, nw4r::lyt::Pane*, const char*, u32) {
    logOnceUnsupported("text box buffers (initTextBoxRecursive)");
}

void LayoutManager::animateRecursive(u32&, nw4r::lyt::Pane*) {
    logOnceUnsupported("animateRecursive");
}

void LayoutManager::movement() {
    if (mLayout == nullptr) {
        return;
    }

    for (u32 i = 0; i < mPaneCtrlNum; i++) {
        mPaneCtrls[i]->movement();
    }
}

void LayoutManager::calcAnim() {
    if (mLayout == nullptr) {
        return;
    }

    for (u32 i = 0; i < mPaneCtrlNum; i++) {
        mPaneCtrls[i]->calcAnim();
    }

    mLayout->Animate(0);
    mLayout->CalculateMtx(mDrawInfo);
}

void LayoutManager::calcAnimWithoutLocationAdjust(const nw4r::lyt::DrawInfo& rDrawInfo) {
    if (mLayout == nullptr) {
        return;
    }

    for (u32 i = 0; i < mPaneCtrlNum; i++) {
        mPaneCtrls[i]->calcAnim();
    }

    mLayout->Animate(0);
    mLayout->CalculateMtx(rDrawInfo);
}

namespace {
    // ---------------------------------------------------------------------
    // One-shot pane-tree dump (M9.5.3c boot diagnostics). The sandbox has no
    // game assets, so the REAL strap layout's visibility/alpha/positions/
    // textures can only be read from the user's boot.log. Dumps run at the
    // first draw and ~5 s later so animation progression is visible too.
    // ---------------------------------------------------------------------
    void dumpPaneTree(nw4r::lyt::Pane* pPane, int depth) {
        if (pPane == nullptr) {
            return;
        }

        char indent[33];
        int n = depth * 2;

        if (n > 32) {
            n = 32;
        }

        for (int i = 0; i < n; i++) {
            indent[i] = ' ';
        }

        indent[n] = '\0';

        int boundAnims = 0;

        for (auto it = pPane->mAnimList.GetBeginIter(); it != pPane->mAnimList.GetEndIter(); ++it) {
            if (it->GetAnimTransform() != nullptr) {
                boundAnims++;
            }
        }

        const nw4r::math::MTX34& m = pPane->mGlbMtx;
        PL_LOG_INFO("compat.layout",
                    "tree %s'%s' vis=%d alpha=%u/%u glbPos=(%.1f,%.1f) pos=(%.1f,%.1f) basePos=%u "
                    "size=(%.0fx%.0f) scale=(%.2f,%.2f) anm=%d",
                    indent, pPane->mName, static_cast< int >(pPane->IsVisible()),
                    static_cast< unsigned >(pPane->mAlpha), static_cast< unsigned >(pPane->mGlbAlpha),
                    m.m[0][3], m.m[1][3], pPane->mTranslate.x, pPane->mTranslate.y,
                    static_cast< unsigned >(pPane->mBasePosition),
                    pPane->mSize.width, pPane->mSize.height, pPane->mScale.x,
                    pPane->mScale.y, boundAnims);

        const nw4r::lyt::Material* pMat = pPane->mpMaterial;

        if (pMat != nullptr) {
            const int texNum = pMat->GetTextureNum();
            const nw4r::lyt::TexMap* pTex = pMat->GetTexMapAry();

            for (int t = 0; pTex != nullptr && t < texNum; t++) {
                PL_LOG_INFO("compat.layout",
                            "tree %s  mat '%s' tex[%d] %ux%u fmt=%d img=%p palette=%p/%u",
                            indent, pMat->GetName(), t, static_cast< unsigned >(pTex[t].mWidth),
                            static_cast< unsigned >(pTex[t].mHeight),
                            static_cast< int >(pTex[t].GetTexelFormat()), pTex[t].mImage,
                            pTex[t].GetPalette(), static_cast< unsigned >(pTex[t].GetPaletteEntryNum()));
            }
        }

        for (auto it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
            dumpPaneTree(&*it, depth + 1);
        }
    }
}  // namespace

void LayoutManager::draw() const {
    // M9.5.3c: the real layout draw. LayoutActor::draw lands here with no
    // projection set up for the layout pass (LogoScene only configures GX for
    // the ISBN branch), so the draw is self-contained: default viewport/
    // scissor over the framebuffer, then SMG's Y-up layout-space ortho
    // (MR::setupDrawForNW4RLayout, verbatim from DrawUtil.cpp) and the pane
    // tree Draw. Pane matrices come from the frame's calcAnim phase
    // (mLayout->CalculateMtx(mDrawInfo) there); Draw only loads them and
    // submits the quads through the compat GX layer.
    if (mLayout == nullptr || mIsScreenHidden) {
        return;
    }

    static bool sLogged = false;

    if (!sLogged) {
        sLogged = true;
        PL_LOG_INFO("compat.layout", "LayoutManager::draw: first layout draw (M9.5.3c drawing active)");
    }

    // One-shot diagnostics (see dumpPaneTree): draw #1 and #300 (~5 s in).
    static u32 sDrawCount = 0;
    sDrawCount++;

    if (sDrawCount == 1 || sDrawCount == 300) {
        PL_LOG_INFO("compat.layout",
                    "draw #%u: yAxisUp=%d glbAlpha=%.2f viewRect=(%.0f,%.0f)-(%.0f,%.0f)",
                    static_cast< unsigned >(sDrawCount), static_cast< int >(mDrawInfo.IsYAxisUp()),
                    mDrawInfo.GetGlobalAlpha(), mDrawInfo.mViewRect.left, mDrawInfo.mViewRect.top,
                    mDrawInfo.mViewRect.right, mDrawInfo.mViewRect.bottom);
        dumpPaneTree(mLayout->mpRootPane, 0);

        if (mPaneCtrlNum > 0 && mPaneCtrls != nullptr && mPaneCtrls[0] != nullptr) {
            for (u32 layer = 0; layer < mAnimLayerNum; layer++) {
                const J3DFrameCtrl* pFrameCtrl = mPaneCtrls[0]->getFrameCtrl(layer);

                if (pFrameCtrl != nullptr) {
                    PL_LOG_INFO("compat.layout", "draw #%u: anim layer %u frame %.2f/%d mode=%d",
                                static_cast< unsigned >(sDrawCount), static_cast< unsigned >(layer),
                                pFrameCtrl->getFrame(), static_cast< int >(pFrameCtrl->getEnd()),
                                static_cast< int >(pFrameCtrl->getAttribute()));
                }
            }
        }
    }

    MR::setDefaultViewportAndScissor();
    MR::setupDrawForNW4RLayout(1.0f, true);
    mLayout->Draw(mDrawInfo);
}

void LayoutManager::addPaneCtrl(LayoutPaneCtrl* pCtrl) {
    if (pCtrl == nullptr) {
        return;
    }

    LayoutPaneCtrl** pNewList = new LayoutPaneCtrl*[mPaneCtrlNum + 1];

    for (u32 i = 0; i < mPaneCtrlNum; i++) {
        pNewList[i] = mPaneCtrls[i];
    }

    pNewList[mPaneCtrlNum] = pCtrl;

    delete[] mPaneCtrls;
    mPaneCtrls = pNewList;
    mPaneCtrlNum++;
}

LayoutPaneCtrl* LayoutManager::createAndAddRootPaneCtrl(u32 animLayerNum) {
    LayoutPaneCtrl* pCtrl = new LayoutPaneCtrl(this, nullptr, animLayerNum);
    addPaneCtrl(pCtrl);
    return pCtrl;
}

LayoutPaneCtrl* LayoutManager::createAndAddPaneCtrl(const char* pPaneName, u32 animLayerNum) {
    LayoutPaneCtrl* pCtrl = new LayoutPaneCtrl(this, pPaneName, animLayerNum);
    addPaneCtrl(pCtrl);
    return pCtrl;
}

LayoutPaneCtrl* LayoutManager::getPaneCtrl(const char* pPaneName) const {
    if (pPaneName == nullptr) {
        return mRootPaneCtrl;
    }

    for (u32 i = 0; i < mPaneCtrlNum; i++) {
        if (mPaneCtrls[i]->mPane != nullptr && strcmp(mPaneCtrls[i]->mPane->mName, pPaneName) == 0) {
            return mPaneCtrls[i];
        }
    }

    return nullptr;
}

s32 LayoutManager::getIndexOfPane(const char* pPaneName) const {
    for (u32 i = 0; i < mPaneCtrlNum; i++) {
        if (mPaneCtrls[i]->mPane != nullptr && strcmp(mPaneCtrls[i]->mPane->mName, pPaneName) == 0) {
            return static_cast< s32 >(i);
        }
    }

    return -1;
}

bool LayoutManager::isExistPaneCtrl(const char* pPaneName) const {
    return getPaneCtrl(pPaneName) != nullptr;
}

void LayoutManager::addGroupCtrl(LayoutGroupCtrl*) {
    logOnceUnsupported("addGroupCtrl");
}

LayoutPaneCtrl* LayoutManager::createAndAddGroupCtrl(const char*, u32) {
    logOnceUnsupported("createAndAddGroupCtrl");
    return nullptr;
}

s32 LayoutManager::getIndexOfGroupCtrl(const char*) const {
    logOnceUnsupported("getIndexOfGroupCtrl");
    return -1;
}

nw4r::lyt::Group* LayoutManager::getGroup(const char* pGroupName) const {
    if (mLayout == nullptr) {
        return nullptr;
    }

    nw4r::lyt::GroupContainer* pContainer = mLayout->GetGroupContainer();

    if (pContainer == nullptr) {
        return nullptr;
    }

    return pContainer->FindGroupByName(pGroupName);
}

bool LayoutManager::isPointing(const nw4r::lyt::Pane*, const TVec2f&) const {
    return false; // pointer (Wii remote cursor) system not ported yet
}

bool LayoutManager::isPointing(const char*, const TVec2f&) const {
    return false; // pointer (Wii remote cursor) system not ported yet
}

void LayoutManager::createPaneMtxRef(const char* pPaneName) {
    nw4r::lyt::Pane* pPane = getPane(pPaneName);

    if (pPane == nullptr) {
        PL_LOG_WARN("compat.layout", "LayoutManager::createPaneMtxRef: pane '%s' not found",
                    pPaneName != nullptr ? pPaneName : "(root)");
        return;
    }

    if (isExistPaneMtxRef(pPaneName)) {
        return;
    }

    PaneMtxRef* pNewList = new PaneMtxRef[mPaneMtxRefNum + 1];

    for (u32 i = 0; i < mPaneMtxRefNum; i++) {
        pNewList[i] = mPaneMtxRefs[i];
    }

    pNewList[mPaneMtxRefNum].mName = copyString(pPaneName);
    pNewList[mPaneMtxRefNum].mMtx = pPane->mGlbMtx.mtx;

    delete[] mPaneMtxRefs;
    mPaneMtxRefs = pNewList;
    mPaneMtxRefNum++;
}

MtxPtr LayoutManager::getPaneMtxRef(const char* pPaneName) const {
    for (u32 i = 0; i < mPaneMtxRefNum; i++) {
        bool match = (pPaneName == nullptr) ? (mPaneMtxRefs[i].mName == nullptr)
                                            : (mPaneMtxRefs[i].mName != nullptr && strcmp(mPaneMtxRefs[i].mName, pPaneName) == 0);

        if (match) {
            return mPaneMtxRefs[i].mMtx;
        }
    }

    return nullptr;
}

bool LayoutManager::isExistPaneMtxRef(const char* pPaneName) const {
    return getPaneMtxRef(pPaneName) != nullptr;
}

nw4r::lyt::AnimTransform* LayoutManager::getAnimTransform(const char* pAnimName) const {
    if (mLayoutHolder == nullptr || pAnimName == nullptr) {
        return nullptr;
    }

    int idx = mLayoutHolder->mAnimRes.getResIndex(pAnimName);

    if (idx < 0 || static_cast< u32 >(idx) >= mAnimTransNum) {
        return nullptr;
    }

    return mAnimTransList[idx];
}

void LayoutManager::bindPaneCtrlAnim(LayoutPaneCtrl* pCtrl, nw4r::lyt::AnimTransform* pTransform) {
    if (pCtrl == nullptr || pCtrl->mPane == nullptr || pTransform == nullptr) {
        return;
    }

    pCtrl->mPane->UnbindAnimation(pTransform, true);
    pCtrl->mPane->BindAnimation(pTransform, true);

    u32 bookkeeping = 0;
    bindPaneCtrlAnimSub(bookkeeping, pTransform);
}

void LayoutManager::bindPaneCtrlAnimSub(u32&, nw4r::lyt::AnimTransform*) {
    // Console-side bookkeeping for the shared anim-transform list. The
    // functional part on the host is the recursive Pane::BindAnimation above;
    // transforms are registered in mAnimTransList at arc-init time already.
}

void LayoutManager::unbindPaneCtrlAnim(LayoutPaneCtrl* pCtrl, nw4r::lyt::AnimTransform* pTransform) {
    if (pCtrl == nullptr || pCtrl->mPane == nullptr || pTransform == nullptr) {
        return;
    }

    pCtrl->mPane->UnbindAnimation(pTransform, true);

    u32 bookkeeping = 0;
    unbindPaneCtrlAnimSub(bookkeeping, pTransform);
}

void LayoutManager::unbindPaneCtrlAnimSub(u32&, nw4r::lyt::AnimTransform*) {
    // See bindPaneCtrlAnimSub.
}

nw4r::lyt::Pane* LayoutManager::getPane(const char* pPaneName) const {
    if (mLayout == nullptr) {
        return nullptr;
    }

    if (pPaneName == nullptr) {
        return mLayout->mpRootPane;
    }

    return findPaneByName(pPaneName);
}

nw4r::lyt::Pane* LayoutManager::findPaneByName(const char* pPaneName) const {
    if (mLayout == nullptr || mLayout->mpRootPane == nullptr) {
        return nullptr;
    }

    return mLayout->mpRootPane->FindPaneByName(pPaneName, true);
}

void LayoutManager::replaceIndDummyTexture() {
    // Indirect-texture dummy replacement (GX workaround on console); the host
    // renderer path decides this in M9.5.3c. No-op for now.
}

void LayoutManager::removeUnnecessaryPanes(nw4r::lyt::Pane*) {
    // Console memory optimization; keeping every pane is harmless on host.
}

// =============================================================================
// MR:: — layout helpers (verbatim bodies from petari's LayoutUtil.cpp /
// LayoutCoreUtil.cpp, which are not host-compiled).
// =============================================================================
namespace MR {

    bool isDead(const LayoutActor* pActor) {
        return pActor->mFlag.mIsDead;
    }

    bool isHiddenLayout(const LayoutActor* pActor) {
        return pActor->mFlag.mIsHidden;
    }

    void showLayout(LayoutActor* pActor) {
        pActor->mFlag.mIsHidden = false;
        pActor->mFlag.mIsOffCalcAnim = false;
    }

    void hideLayout(LayoutActor* pActor) {
        pActor->mFlag.mIsHidden = true;
        pActor->mFlag.mIsOffCalcAnim = true;
    }

    bool isStopAnimFrame(const LayoutActor* pActor) {
        return pActor->mFlag.mIsStopAnimFrame;
    }

    void stopAnimFrame(LayoutActor* pActor) {
        pActor->mFlag.mIsStopAnimFrame = true;
    }

    void releaseAnimFrame(LayoutActor* pActor) {
        pActor->mFlag.mIsStopAnimFrame = false;
    }

    void onCalcAnim(LayoutActor* pActor) {
        pActor->mFlag.mIsOffCalcAnim = false;
    }

    void offCalcAnim(LayoutActor* pActor) {
        pActor->mFlag.mIsOffCalcAnim = true;
    }

    bool isExecuteCalcAnimLayout(const LayoutActor* pActor) {
        if (pActor->mFlag.mIsDead) {
            return false;
        }

        if (pActor->mLayoutManager == nullptr) {
            return false;
        }

        return !pActor->mFlag.mIsOffCalcAnim;
    }

    bool isExecuteDrawLayout(const LayoutActor* pActor) {
        if (pActor->mFlag.mIsDead) {
            return false;
        }

        if (pActor->mLayoutManager == nullptr) {
            return false;
        }

        return !pActor->mFlag.mIsHidden;
    }

    void showScreen(LayoutActor* pActor) {
        LayoutManager* pLayoutManager = pActor->getLayoutManager();

        pLayoutManager->mIsScreenHidden = false;
    }

    void hideScreen(LayoutActor* pActor) {
        LayoutManager* pLayoutManager = pActor->getLayoutManager();

        pLayoutManager->mIsScreenHidden = true;
    }

    nw4r::lyt::Pane* getPane(const LayoutActor* pActor, const char* pPaneName) {
        return pActor->getLayoutManager()->getPane(pPaneName);
    }

    nw4r::lyt::Pane* getRootPane(const LayoutActor* pActor) {
        return pActor->getLayoutManager()->getPane(nullptr);
    }

    // PC_PORT reconstruction (petari has no body): copies the pane's global
    // translation (layout space) into pTrans. Pane null defaults to the root.
    void copyPaneTrans(TVec2f* pTrans, const LayoutActor* pActor, const char* pPaneName) {
        const nw4r::lyt::Pane* pPane = getPane(pActor, pPaneName);

        if (pPane == nullptr) {
            pTrans->x = 0.0f;
            pTrans->y = 0.0f;
            return;
        }

        pTrans->x = pPane->mGlbMtx._03;
        pTrans->y = pPane->mGlbMtx._13;
    }

    // Verbatim from LayoutCoreUtil.cpp.
    void convertScreenPosToLayoutPos(TVec2f* pLayoutPos, const TVec2f& rScreenPos) {
        pLayoutPos->x = rScreenPos.x * 608.0f / MR::getScreenWidth() - 304.0f;
        pLayoutPos->y = -(rScreenPos.y * MR::getScreenHeight() / MR::getScreenHeight() - (MR::getScreenHeight() * 0.5f));
    }

    // Verbatim from LayoutCoreUtil.cpp.
    void convertLayoutPosToScreenPos(TVec2f* pScreenPos, const TVec2f& rLayoutPos) {
        pScreenPos->x = rLayoutPos.x * MR::getScreenWidth() / 608.0f + MR::getScreenWidth() * 0.5f;
        pScreenPos->y = MR::getScreenHeight() * 0.5f + -rLayoutPos.y * MR::getScreenHeight() / MR::getScreenHeight();
    }

    // Verbatim from ScreenUtil.cpp.
    s32 getScreenWidth() {
        return isScreen16Per9() ? 832 : 608;
    }

    // PC_PORT: petari omits the body. SMG's 4:3 screen space is 608 x 456
    // (the layout space); the 16:9 branch is unreachable while isScreen16Per9
    // is stubbed false.
    s32 getScreenHeight() {
        return 456;
    }

    // PC_PORT (M9.5.3c): petari's ScreenUtil reads the JUTVideo render mode.
    // The manager is guarded because the test binaries never construct one;
    // the fallback is the 4:3 EFB width.
    s32 getFrameBufferWidth() {
        JUTVideo* pVideo = JUTVideo::getManager();

        if (pVideo != nullptr && pVideo->getRenderMode() != nullptr) {
            return pVideo->getRenderMode()->fbWidth;
        }

        return 640;
    }

    void setFollowPos(const TVec2f* pFollowPos, const LayoutActor* pActor, const char* pPaneName) {
        pActor->getLayoutManager()->getPaneCtrl(pPaneName)->mFollowPos = pFollowPos;
    }

    void setFollowTypeReplace(const LayoutActor* pActor, const char* pPaneName) {
        pActor->getLayoutManager()->getPaneCtrl(pPaneName)->mFollowType = 0;
    }

    void setFollowTypeAdd(const LayoutActor* pActor, const char* pPaneName) {
        pActor->getLayoutManager()->getPaneCtrl(pPaneName)->mFollowType = 1;
    }

    // PC_PORT reconstruction (petari has no body): every sibling helper routes
    // through the root pane controller; startPaneAnim shows the pattern.
// Game/Util/LayoutUtil.cpp:194 — no-op until the effect system lands (M10).
// TitleSequenceProduct fires 7 "TitleLogoLight*" emitters per step; the
// PaneEffectKeeper stub already swallows the keeper side.
void emitEffect(LayoutActor*, const char* pEffectName) {
    PL_LOG_DEBUG("game.layout", "emitEffect('%s') — effects stub (M10)", pEffectName);
}

// Game/Util/LayoutUtil.cpp:197 — no-op (same).
void deleteEffectAll(LayoutActor*) {}

    void startAnim(LayoutActor* pActor, const char* pAnimName, u32 animLayer) {
        LayoutPaneCtrl* pPaneCtrl = pActor->getLayoutManager()->getPaneCtrl(nullptr);

        if (pPaneCtrl != nullptr) {
            pPaneCtrl->start(pAnimName, animLayer);
        }
    }

    void startAnimAtFirstStep(LayoutActor* pActor, const char* pAnimName, u32 animLayer) {
        if (isFirstStep(pActor)) {
            startAnim(pActor, pAnimName, animLayer);
        }
    }

    void startAnimAndSetFrameAndStop(LayoutActor* pActor, const char* pAnimName, f32 animFrame, u32 animLayer) {
        startAnim(pActor, pAnimName, animLayer);
        setAnimFrameAndStop(pActor, animFrame, animLayer);
    }

    J3DFrameCtrl* getAnimCtrl(const LayoutActor* pActor, u32 animLayer) {
        return pActor->getLayoutManager()->getPaneCtrl(nullptr)->getFrameCtrl(animLayer);
    }

    void setAnimFrameAndStop(LayoutActor* pActor, f32 animFrame, u32 animLayer) {
        J3DFrameCtrl* pFrameCtrl = getAnimCtrl(pActor, animLayer);

        pFrameCtrl->setFrame(animFrame);
        pFrameCtrl->setRate(0.0f);
    }

    f32 getAnimFrame(const LayoutActor* pActor, u32 animLayer) {
        return getAnimCtrl(pActor, animLayer)->getFrame();
    }

    s16 getAnimFrameMax(const LayoutActor* pActor, u32 animLayer) {
        return getAnimCtrl(pActor, animLayer)->getEnd();
    }

    s16 getAnimFrameMax(const LayoutActor* pActor, const char* pAnimName) {
        return pActor->getLayoutManager()->getAnimTransform(pAnimName)->GetFrameSize();
    }

    void setAnimFrameAndStopAtEnd(LayoutActor* pActor, u32 animLayer) {
        setAnimFrameAndStop(pActor, getAnimFrameMax(pActor, animLayer), animLayer);
    }

    void setAnimFrame(LayoutActor* pActor, f32 animFrame, u32 animLayer) {
        getAnimCtrl(pActor, animLayer)->setFrame(animFrame);
    }

    void setAnimRate(LayoutActor* pActor, f32 animRate, u32 animLayer) {
        getAnimCtrl(pActor, animLayer)->setRate(animRate);
    }

    void stopAnim(LayoutActor* pActor, u32 animLayer) {
        pActor->getLayoutManager()->getPaneCtrl(nullptr)->stop(animLayer);
    }

    bool isAnimStopped(const LayoutActor* pActor, u32 animLayer) {
        return pActor->getLayoutManager()->getPaneCtrl(nullptr)->isAnimStopped(animLayer);
    }

    void invalidateParentAnim(LayoutActor* pActor) {
        LayoutManager* pLayoutManager = pActor->getLayoutManager();

        pLayoutManager->_61 = 0;
    }

    void killAtAnimStopped(LayoutActor* pActor, u32 animLayer) {
        if (isAnimStopped(pActor, animLayer)) {
            pActor->kill();
        }
    }

    void setNerveAtStep(LayoutActor* pActor, const Nerve* pNerve, s32 step) {
        if (pActor->getNerveStep() == step) {
            pActor->setNerve(pNerve);
        }
    }

    void setNerveAtAnimStopped(LayoutActor* pActor, const Nerve* pNerve, u32 animLayer) {
        if (isAnimStopped(pActor, animLayer)) {
            pActor->setNerve(pNerve);
        }
    }

} // namespace MR

// =============================================================================
// StarPointerLayoutTargetKeeper — verbatim ctor from petari's
// StarPointerTarget.cpp (the rest of the pointer system stays out until the
// cursor lands). LayoutActor::initPointingTarget only constructs it.
// =============================================================================
StarPointerLayoutTargetKeeper::StarPointerLayoutTargetKeeper(int maxNumTargets)
    : mNumTargets(0), mMaxNumTargets(maxNumTargets), mTargets(nullptr) {
    mTargets = new StarPointerLayoutTarget*[mMaxNumTargets];
    memset(mTargets, 0, mMaxNumTargets * sizeof(StarPointerLayoutTarget*));
}
