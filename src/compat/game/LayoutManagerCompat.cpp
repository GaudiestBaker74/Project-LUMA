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
#include <nw4r/lyt/textBox.h>
#include <nw4r/ut/Font.h>

#include "compat/game/GameTextTable.h"
#include "compat/game/LanguageCompat.h"
#include "compat/nw4r/LytHost.h"
#include "platform/Log/Log.h"
#include "compat/game/UiAnchoring.h"

#include <cstdio>
#include <cstring>
#include <vector>

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
    // Case-insensitive compare for resource names (MSVC has no strcasecmp).
    bool resNameEquals(const char* pA, const char* pB) {
        while (*pA != '\0' && *pB != '\0') {
            char a = *pA;
            char b = *pB;

            if (a >= 'A' && a <= 'Z') {
                a = static_cast< char >(a - 'A' + 'a');
            }

            if (b >= 'A' && b <= 'Z') {
                b = static_cast< char >(b - 'A' + 'a');
            }

            if (a != b) {
                return false;
            }

            ++pA;
            ++pB;
        }

        return *pA == *pB;
    }

    // True when the arc's resource name denotes the layout the caller asked
    // for. Beside the exact (case-insensitive) name this accepts the
    // "…replace" variant the SMG arcs ship: WiiRemoteStrap.arc contains
    // 'wiiremotestrapreplace.brlyt', which IS the strap layout the actor asks
    // for by its name "WiiRemoteStrap" (the arc has no plain
    // 'wiiremotestrap.brlyt'), so this is a match, not a wrong-layout
    // fallback.
    bool resNameMatches(const char* pResName, const char* pWanted) {
        if (resNameEquals(pResName, pWanted)) {
            return true;
        }

        const size_t wantedLen = std::strlen(pWanted);
        char head[128];

        if (wantedLen >= sizeof(head) || std::strlen(pResName) != wantedLen + 7 /* "replace" */) {
            return false;
        }

        std::memcpy(head, pResName, wantedLen);
        head[wantedLen] = '\0';

        return resNameEquals(head, pWanted) && resNameEquals(pResName + wantedLen, "replace");
    }

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
      mLayoutName(copyString(pName)), mDrawCount(0) {
    char arcPath[256];
    buildLayoutArcPath(arcPath, sizeof(arcPath), pName, convertFilename);

    initArc(arcPath, pName);
    initDrawInfo();
    initPaneInfo();
    initGroupCtrlList();

    // M9.5.4 v7: string buffers + fonts + text for every TextBox (console:
    // LayoutManager::initTextBoxRecursive, called from the ctor with the
    // layout name for the message lookup).
    if (mLayout != nullptr && mLayout->mpRootPane != nullptr && mTextBoxBufferLength > 0) {
        initTextBoxRecursive(mLayout->mpRootPane, mLayout->mpRootPane, pName, mTextBoxBufferLength);
    }
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
        // The arc may name the brlyt differently from the name the actor asked
        // for: a case difference, or the '…replace' variant (see
        // resNameMatches). Look for a real match before falling back — the
        // old code always took whichever layout came first and warned, which
        // is how 'WiiRemoteStrap' ended up logging a warning on every boot.
        for (u32 i = 0; i < mLayoutHolder->mLayoutRes.mCount; i++) {
            const char* pResName = mLayoutHolder->mLayoutRes.getResName(i);

            if (pResName != nullptr && resNameMatches(pResName, pLayoutName)) {
                pBrlyt = mLayoutHolder->mLayoutRes.getRes(i);
                break;
            }
        }
    }

    if (pBrlyt == nullptr && mLayoutHolder->mLayoutRes.mCount > 0) {
        // Nothing matched: the arc really holds a different layout. Use it and
        // say so — this is the case that deserves a warning.
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

    // Drop the panes of the other languages before anything binds to them
    // (see removeUnnecessaryPanes).
    removeUnnecessaryPanes(mLayout->mpRootPane);

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
    // flags. The view rect spans the SMG layout space, centered on the origin,
    // with the SAME sign convention as Layout::GetLayoutRect: top > bottom.
    // That ordering matters — DrawInfo::IsYAxisUp() is `bottom - top < 0`, and
    // Pane::LoadMtx only reverses the Y axis (layout space is Y-down, the
    // screen ortho set by MR::setupDrawForNW4RLayout is Y-up) when the DrawInfo
    // says Y-up.
    //
    // PC_PORT (title widescreen): the rect follows the ASPECT RATIO, like the
    // console's getScreenWidth() (608 in 4:3, 832 in 16:9). The rect is only
    // half the story though — pane positions come from the view MATRIX below,
    // which is what actually places the composition on screen.
    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);
    const f32 spaceWidth = compat::ui::layoutSpaceWidth(fbWidth, fbHeight);
    mDrawInfo.mViewRect = nw4r::ut::Rect(-spaceWidth * 0.5f, cLayoutSpaceHeight * 0.5f,
                                         spaceWidth * 0.5f, -cLayoutSpaceHeight * 0.5f);
    updateUiAnchoring();
}

// PC_PORT (title widescreen): the single place where the layout space meets the
// framebuffer — see compat/game/UiAnchoring.h for the mapping and for what the
// old (stretching) behaviour was.
void LayoutManager::updateUiAnchoring() {
    // PC_PORT: PIXELS, not the 456-unit design height — see
    // compat::ui::framebufferSize(). Passing 456 here at 1920x1080 gave
    // uiScale 1.0, i.e. the whole UI at 1 layout unit = 1 pixel.
    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    compat::ui::framebufferSize(&fbWidth, &fbHeight);

    f32 mtx[3][4];
    compat::ui::fillUiViewMtx(fbWidth, fbHeight, mtx);

    mDrawInfo.mViewMtx._00 = mtx[0][0];
    mDrawInfo.mViewMtx._01 = mtx[0][1];
    mDrawInfo.mViewMtx._02 = mtx[0][2];
    mDrawInfo.mViewMtx._03 = mtx[0][3];
    mDrawInfo.mViewMtx._10 = mtx[1][0];
    mDrawInfo.mViewMtx._11 = mtx[1][1];
    mDrawInfo.mViewMtx._12 = mtx[1][2];
    mDrawInfo.mViewMtx._13 = mtx[1][3];
    mDrawInfo.mViewMtx._20 = mtx[2][0];
    mDrawInfo.mViewMtx._21 = mtx[2][1];
    mDrawInfo.mViewMtx._22 = mtx[2][2];
    mDrawInfo.mViewMtx._23 = mtx[2][3];
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
    // A "group" is nw4r lyt's way of tagging panes inside a layout (the 'grp1'
    // blocks the SMG arcs carry, e.g. StarPointerLayout's "GroupRing").
    // Animations bound to a group are applied by Layout::BindAnimationAuto
    // (patches/nw4r/lyt/lyt_layout.cpp) — that part IS ported.
    //
    // What is not ported is the by-name group *controller* API
    // (addGroupCtrl / createAndAddGroupCtrl / getIndexOfGroupCtrl below): game
    // code asks for one with MR::createAndAddGroupCtrl and drives an animation
    // through it. The only caller in the game is StarPointerLayout.cpp:98
    // ("GroupRing"); the title/logo/strap layouts never ask for one. Those
    // entry points warn when actually called, so a layout that merely
    // CONTAINS groups does not need a warning of its own.
    if (mLayout == nullptr || mLayout->GetGroupContainer() == nullptr) {
        return;
    }

    nw4r::lyt::GroupContainer* pContainer = mLayout->GetGroupContainer();
    u32 groupNum = 0;

    for (nw4r::lyt::GroupList::Iterator it = pContainer->mGroupList.GetBeginIter();
         it != pContainer->mGroupList.GetEndIter(); ++it) {
        ++groupNum;
    }

    PL_LOG_INFO("compat.layout",
                "LayoutManager '%s': %u group(s) in the layout (group-bound animations bind normally; "
                "the by-name group controller API is not ported yet)",
                mLayoutName != nullptr ? mLayoutName : "?", groupNum);
}

namespace {
    // M9.5.4 v7: the layout message for a text box.
    //
    // Console flow (LayoutCoreUtil.cpp initTextBoxPane): the id is
    // "Layout_<layoutName><paneName>" (e.g. Layout_PressStartTxtStart, minus
    // the language suffix) and the UTF-16 text comes from
    // /MessageData/Message.arc (bmg + MessageId.tbl BCSV). The host does not
    // have the message system yet (MessageHolder/JMapInfo/bmg — all
    // big-endian binary tables, tracked as an open gap in docs), so the known
    // ids get an embedded English fallback and everything else keeps the text
    // baked in the brlyt (txt1 payload) or a visible placeholder.
    //
    // "Press both [A] and [B]." — the real message renders the A/B icons via
    // PictureFont tags (0x1A tag, group 3); without a tag processor and the
    // picture font wiring they are written as words.
    struct LayoutMessageFallback {
        const char* layoutName;
        const char* panePrefix;  // pane name without the language suffix
        const wchar_t* text;
    };

    const LayoutMessageFallback sLayoutMessageFallbacks[] = {
        {"PressStart", "TxtStart", L"Press both [A] and [B]."},
        {"PressStart", "ShaStart", L"Press both [A] and [B]."},
    };

    // `pPaneName` may carry a language suffix ("TxtStartUsEn"): match on the
    // prefix so every language pane gets the same fallback.
    //
    // PC_PORT (M10.1): the table in compat/game/GameTextTable resolves the REAL
    // message id first — "Layout_" + layoutName + paneName(minus the language
    // suffix), which is the id LayoutCoreUtil::initTextBoxPane asks the message
    // system for. That is what makes the FileSelect / BackButton / PressStart
    // text follow --language instead of showing the English text baked in the
    // brlyt (the mixed-language bug of M10.1). Only ids the table does not know
    // fall through to the embedded fallbacks below.
    const wchar_t* findLayoutMessageFallback(const char* pLayoutName, const char* pPaneName) {
        if (pLayoutName == nullptr || pPaneName == nullptr) {
            return nullptr;
        }

        char messageId[128];
        compat::game::buildLayoutMessageId(messageId, sizeof(messageId), pLayoutName, pPaneName);

        if (const wchar_t* pLocalized = compat::game::gameTextForMessageId(messageId)) {
            return pLocalized;
        }

        for (const LayoutMessageFallback& entry : sLayoutMessageFallbacks) {
            if (std::strcmp(entry.layoutName, pLayoutName) != 0) {
                continue;
            }

            const size_t prefixLen = std::strlen(entry.panePrefix);

            if (std::strncmp(entry.panePrefix, pPaneName, prefixLen) == 0) {
                return entry.text;
            }
        }

        return nullptr;
    }

}  // namespace

void LayoutManager::initTextBoxRecursive(nw4r::lyt::Pane* pRoot, nw4r::lyt::Pane* pPane, const char* pLayoutName,
                                         u32 bufferLength) {
    if (pPane == nullptr) {
        return;
    }

    if (pPane->GetRuntimeTypeInfo()->IsDerivedFrom(&nw4r::lyt::TextBox::typeInfo)) {
        nw4r::lyt::TextBox* pTextBox = static_cast< nw4r::lyt::TextBox* >(pPane);

        // Console: AllocStringBuffer(len) where the ctor parameter is the
        // buffer length in characters (PressStart passes 0x100). Growing the
        // buffer drops the txt1 text the ctor copied from the brlyt, so keep
        // a copy: it is what the pane shows when no message is known.
        std::vector< wchar_t > designText;

        if (pTextBox->mTextBuf != nullptr && pTextBox->mTextLen > 0) {
            designText.assign(pTextBox->mTextBuf, pTextBox->mTextBuf + pTextBox->mTextLen);
        }

        const u16 chars = static_cast< u16 >(bufferLength > 0xFFFF ? 0xFFFF : bufferLength);
        pTextBox->AllocStringBuffer(chars);

        // Font: the brlyt names the font (fnl1); LayoutHolder::GetFont resolves
        // it from the GameSystem font holder (Font.arc). A layout built before
        // the fonts were loaded (or a name the holder does not have) keeps a
        // null font and would never draw — fall back to the message font.
        if (pTextBox->mpFont == nullptr) {
            const nw4r::ut::Font* pFont = MR::getFontOnCurrentLanguage();

            if (pFont != nullptr) {
                pTextBox->SetFont(pFont);
            } else {
                static bool sWarned = false;

                if (!sWarned) {
                    sWarned = true;
                    PL_LOG_WARN("compat.layout",
                                "LayoutManager '%s': text box '%s' has no font (Font.arc not loaded?) — text stays hidden",
                                pLayoutName != nullptr ? pLayoutName : "?", pPane->mName);
                }
            }
        }

        const wchar_t* pFallback = findLayoutMessageFallback(pLayoutName, pPane->mName);

        if (pFallback != nullptr) {
            pTextBox->SetString(pFallback, 0);
        } else if (!designText.empty()) {
            pTextBox->SetString(designText.data(), 0, static_cast< u16 >(designText.size()));
        }
        // else: no message and no design text — the box stays empty and
        // DrawSelf draws nothing, like a layout whose message id is missing.

        PL_LOG_INFO("compat.layout", "LayoutManager '%s': text box '%s' font=%p len=%u%s",
                    pLayoutName != nullptr ? pLayoutName : "?", pPane->mName,
                    static_cast< const void* >(pTextBox->mpFont), static_cast< unsigned >(pTextBox->mTextLen),
                    pFallback != nullptr ? " (embedded fallback message)" : "");
    }

    for (auto it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
        initTextBoxRecursive(pRoot, &*it, pLayoutName, bufferLength);
    }
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

    // The window/EFB size can change at any time (resize, fullscreen): refresh
    // the mapping before the pane matrices are composed, so a layout that was
    // already built still lands in the right place after a resolution change.
    updateUiAnchoring();

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

    // Diagnostic sampling (see dumpPaneTree below).
    mDrawCount++;

    // PC_PORT (title widescreen): the pane-tree dump is what makes a capture
    // log self-explanatory, so it samples EVERY layout (a global counter left
    // the title's own layout undumped — only the first one to draw got a
    // tree). Draws are per layout, i.e. frames for that layout: the schedule
    // below tracks the title's appear/flash sequence (~4 s) densely and then
    // thins out.
    static const u32 kDumpDraws[] = {1, 2, 3, 4, 6, 9, 13, 18, 25, 35, 50, 70, 100, 140, 190, 250, 300};
    bool dumpNow = false;
    for (u32 d : kDumpDraws) {
        if (mDrawCount == d) {
            dumpNow = true;
            break;
        }
    }

    if (dumpNow) {
        const u32 sDrawCount = mDrawCount;
        // PC_PORT (title widescreen): the framebuffer geometry and the resolved
        // layout->pixel scale go in the same line, so a capture log shows at a
        // glance whether the UI is anchored to the window (fb=1920x1080 =>
        // scale=2.37, i.e. 456 layout units = 1080 px) or to the design height.
        f32 dbgFbW = 0.0f;
        f32 dbgFbH = 0.0f;
        compat::ui::framebufferSize(&dbgFbW, &dbgFbH);
        PL_LOG_INFO("compat.layout",
                    "draw #%u: yAxisUp=%d glbAlpha=%.2f viewRect=(%.0f,%.0f)-(%.0f,%.0f) "
                    "fb=%.0fx%.0f uiScale=%.3f space=%.0f",
                    static_cast< unsigned >(sDrawCount), static_cast< int >(mDrawInfo.IsYAxisUp()),
                    mDrawInfo.GetGlobalAlpha(), mDrawInfo.mViewRect.left, mDrawInfo.mViewRect.top,
                    mDrawInfo.mViewRect.right, mDrawInfo.mViewRect.bottom, dbgFbW, dbgFbH,
                    compat::ui::uiScale(dbgFbW, dbgFbH), static_cast<f32>(MR::getScreenWidth()));
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

// PC_PORT (M9.5.4 v6): language-variant pane filter. petari does not
// decompile this method; the behaviour is reconstructed from the layout data
// and the documented game feature (Luma's Workshop, "Layouts"): a pane may
// come with per-language siblings whose name ends with the first four letters
// of a language folder ("PicTitleLogoJpJa", "TxtStartUsEn"). When the current
// language has such a variant, the plain pane and every other variant are
// dropped; otherwise only the foreign variants are dropped and the plain pane
// stays. Dropped panes are unlinked from the tree and destroyed (nothing else
// references them yet: this runs right after Layout::Build, before the brlan
// transforms are bound and before any pane controller is created).
namespace {
    const char* const cLanguagePaneSuffixes[] = {"JpJa", "UsEn", "UsSp", "UsFr", "EuEn", "EuSp",
                                                 "EuFr", "EuGe", "EuIt", "EuDu", "CnSi", "KrKo"};

    // Returns the language suffix `pName` ends with (nullptr when none).
    const char* findLanguageSuffix(const char* pName) {
        const size_t len = strlen(pName);

        if (len <= 4) {
            return nullptr;
        }

        for (const char* pSuffix : cLanguagePaneSuffixes) {
            if (strcmp(pName + len - 4, pSuffix) == 0) {
                return pSuffix;
            }
        }

        return nullptr;
    }

    // True when `pParent` holds a pane named <base><suffix>.
    bool hasSibling(nw4r::lyt::Pane* pParent, const char* pBase, size_t baseLen, const char* pSuffix) {
        for (auto it = pParent->mChildList.GetBeginIter(); it != pParent->mChildList.GetEndIter(); ++it) {
            const char* pName = it->mName;

            if (strncmp(pName, pBase, baseLen) == 0 && strcmp(pName + baseLen, pSuffix) == 0) {
                return true;
            }
        }

        return false;
    }

    // Collects `pPane` and its whole subtree (Pane::~Pane destroys the
    // children too, and the groups may reference any of them).
    void collectSubtree(nw4r::lyt::Pane* pPane, std::vector< nw4r::lyt::Pane* >& rOut) {
        rOut.push_back(pPane);

        for (auto it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter(); ++it) {
            collectSubtree(&*it, rOut);
        }
    }

    bool contains(const std::vector< nw4r::lyt::Pane* >& rPanes, const nw4r::lyt::Pane* pPane) {
        for (const nw4r::lyt::Pane* p : rPanes) {
            if (p == pPane) {
                return true;
            }
        }

        return false;
    }

    // Drops every group link that targets one of `rPanes` (the brlan group
    // binding walks those links; a dangling target would be a use-after-free).
    void purgeGroupLinks(nw4r::lyt::GroupContainer* pContainer, const std::vector< nw4r::lyt::Pane* >& rPanes) {
        if (pContainer == nullptr) {
            return;
        }

        for (auto grp = pContainer->mGroupList.GetBeginIter(); grp != pContainer->mGroupList.GetEndIter(); ++grp) {
            nw4r::lyt::PaneLinkList& links = grp->GetPaneList();

            for (auto it = links.GetBeginIter(); it != links.GetEndIter();) {
                auto cur = it++;

                if (contains(rPanes, cur->mTarget)) {
                    links.Erase(cur);
                    nw4r::lyt::Layout::FreeMemory(&*cur);
                }
            }
        }
    }

    void destroyPane(nw4r::lyt::Pane* pPane, nw4r::lyt::GroupContainer* pGroups) {
        std::vector< nw4r::lyt::Pane* > subtree;
        collectSubtree(pPane, subtree);
        purgeGroupLinks(pGroups, subtree);

        if (pPane->IsUserAllocated()) {
            return;
        }

        pPane->~Pane();
        nw4r::lyt::Layout::FreeMemory(pPane);
    }
};  // namespace

void LayoutManager::removeUnnecessaryPanes(nw4r::lyt::Pane* pPane) {
    if (pPane == nullptr) {
        return;
    }

    const char* pCurrentSuffix = compat::getLanguagePaneSuffix();
    u32 removedNum = 0;

    for (auto it = pPane->mChildList.GetBeginIter(); it != pPane->mChildList.GetEndIter();) {
        nw4r::lyt::Pane* pChild = &*it;
        ++it;

        const char* pSuffix = findLanguageSuffix(pChild->mName);
        bool remove = false;

        if (pSuffix != nullptr) {
            // A language variant: keep only the current language's one.
            remove = strcmp(pSuffix, pCurrentSuffix) != 0;
        } else if (hasSibling(pPane, pChild->mName, strlen(pChild->mName), pCurrentSuffix)) {
            // A plain pane shadowed by a variant for the current language.
            remove = true;
        }

        if (remove) {
            pPane->RemoveChild(pChild);
            destroyPane(pChild, mLayout != nullptr ? mLayout->GetGroupContainer() : nullptr);
            removedNum++;
        } else {
            removeUnnecessaryPanes(pChild);
        }
    }

    if (removedNum > 0 && pPane == (mLayout != nullptr ? mLayout->mpRootPane : nullptr)) {
        PL_LOG_INFO("compat.layout", "LayoutManager '%s': removed language panes under root (current %s)",
                    mLayoutName != nullptr ? mLayoutName : "?", pCurrentSuffix);
    }
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

    // PC_PORT (title widescreen): the exact inverse of the UI anchoring — see
    // compat/game/UiAnchoring.h. The vendored bodies assumed the 608-wide
    // layout space was stretched across the framebuffer (x factor
    // 608/getScreenWidth with a 1:1 y factor); that mapping no longer exists,
    // so these have to follow the uniform scale the DrawInfo view matrix uses.
    void convertScreenPosToLayoutPos(TVec2f* pLayoutPos, const TVec2f& rScreenPos) {
        f32 w = 0.0f;
        f32 h = 0.0f;
        compat::ui::framebufferSize(&w, &h);
        const f32 scale = compat::ui::uiScale(w, h);

        pLayoutPos->x = (rScreenPos.x - w * 0.5f) / scale;
        pLayoutPos->y = (h * 0.5f - rScreenPos.y) / scale;
    }

    // Verbatim from LayoutCoreUtil.cpp, with the uniform scale above.
    void convertLayoutPosToScreenPos(TVec2f* pScreenPos, const TVec2f& rLayoutPos) {
        f32 w = 0.0f;
        f32 h = 0.0f;
        compat::ui::framebufferSize(&w, &h);
        compat::ui::layoutToScreen(rLayoutPos.x, rLayoutPos.y, w, h, &pScreenPos->x,
                                   &pScreenPos->y);
    }

    // PC_PORT (title widescreen): ScreenUtil.cpp returns 832 in 16:9 and 608 in
    // 4:3. On the host the render mode is whatever the window is, so the answer
    // comes from the framebuffer's aspect ratio (the 16:9 branch used to be
    // unreachable because isScreen16Per9() was a stub that returned false).
    s32 getScreenWidth() {
        f32 w = 0.0f;
        f32 h = 0.0f;
        compat::ui::framebufferSize(&w, &h);
        return static_cast<s32>(compat::ui::layoutSpaceWidth(w, h));
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

    // PC_PORT (M9.5.4): the console never misses a pane controller (every pane
    // of the brlyt gets one in initPaneInfoRecursive). On the host only the
    // root controller exists, and a layout whose arc failed to mount has no
    // panes at all — so every helper that used to dereference
    // getPaneCtrl(name) unconditionally now logs once and returns instead of
    // faulting (the previous behaviour was a null deref inside the scene-init
    // worker thread, i.e. a silent process crash while the Title loads).
    static LayoutPaneCtrl* paneCtrlOrWarn(const LayoutActor* pActor, const char* pPaneName, const char* pWhat) {
        LayoutManager* pManager = pActor != nullptr ? pActor->getLayoutManager() : nullptr;
        LayoutPaneCtrl* pCtrl = pManager != nullptr ? pManager->getPaneCtrl(pPaneName) : nullptr;

        if (pCtrl == nullptr) {
            PL_LOG_WARN("game.layout", "%s: no pane controller for '%s' in layout '%s' (ignored)", pWhat,
                        pPaneName != nullptr ? pPaneName : "<root>",
                        pManager != nullptr && pManager->mLayoutName != nullptr ? pManager->mLayoutName : "?");
        }

        return pCtrl;
    }

    void setFollowPos(const TVec2f* pFollowPos, const LayoutActor* pActor, const char* pPaneName) {
        LayoutPaneCtrl* pCtrl = paneCtrlOrWarn(pActor, pPaneName, "setFollowPos");

        if (pCtrl != nullptr) {
            pCtrl->mFollowPos = pFollowPos;
        }
    }

    void setFollowTypeReplace(const LayoutActor* pActor, const char* pPaneName) {
        LayoutPaneCtrl* pCtrl = paneCtrlOrWarn(pActor, pPaneName, "setFollowTypeReplace");

        if (pCtrl != nullptr) {
            pCtrl->mFollowType = 0;
        }
    }

    void setFollowTypeAdd(const LayoutActor* pActor, const char* pPaneName) {
        LayoutPaneCtrl* pCtrl = paneCtrlOrWarn(pActor, pPaneName, "setFollowTypeAdd");

        if (pCtrl != nullptr) {
            pCtrl->mFollowType = 1;
        }
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

    static LayoutPaneCtrl* rootCtrlWithLayerOrWarn(const LayoutActor* pActor, u32 animLayer, const char* pWhat);

    void startAnim(LayoutActor* pActor, const char* pAnimName, u32 animLayer) {
        LayoutPaneCtrl* pPaneCtrl = rootCtrlWithLayerOrWarn(pActor, animLayer, "startAnim");

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

    // PC_PORT (M9.5.4): the vendored LayoutUtil helpers dereference
    // getPaneCtrl(nullptr) and index the layer array unchecked. The root
    // controller always exists on the host (initPaneInfo), but a layer index
    // beyond the actor's animLayerNum (LogoLayout has 2 layers, SimpleLayout
    // 1) would read past mAnmPlayerArray — a silent crash inside the
    // scene-init worker while the Title screen loads. Validate both and fall
    // back to a process-wide dummy frame controller so callers that only
    // poke frame/rate keep working; the WARN in the log says what happened.
    static LayoutPaneCtrl* rootCtrlWithLayerOrWarn(const LayoutActor* pActor, u32 animLayer, const char* pWhat) {
        LayoutPaneCtrl* pCtrl = paneCtrlOrWarn(pActor, nullptr, pWhat);

        if (pCtrl == nullptr) {
            return nullptr;
        }

        if (animLayer >= static_cast< u32 >(pCtrl->mAnmPlayerArray.size())) {
            LayoutManager* pManager = pActor != nullptr ? pActor->getLayoutManager() : nullptr;
            PL_LOG_WARN("game.layout", "%s: anim layer %u out of range (layout '%s' has %d layer(s)) (ignored)", pWhat,
                        static_cast< unsigned >(animLayer),
                        pManager != nullptr && pManager->mLayoutName != nullptr ? pManager->mLayoutName : "?",
                        pCtrl->mAnmPlayerArray.size());
            return nullptr;
        }

        return pCtrl;
    }

    J3DFrameCtrl* getAnimCtrl(const LayoutActor* pActor, u32 animLayer) {
        LayoutPaneCtrl* pCtrl = rootCtrlWithLayerOrWarn(pActor, animLayer, "getAnimCtrl");

        if (pCtrl == nullptr) {
            // Never hand out null: vendored callers (setAnimFrameAndStop,
            // setAnimRate, ...) dereference the result unconditionally.
            static J3DFrameCtrl sDummyFrameCtrl(0);
            return &sDummyFrameCtrl;
        }

        return pCtrl->getFrameCtrl(animLayer);
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
        // PC_PORT (M9.5.4): mirrors vendored LayoutUtil.cpp:304 but tolerates
        // a missing brlan (getAnimTransform returns null on the host when the
        // arc lacks the animation) instead of dereferencing null.
        nw4r::lyt::AnimTransform* pTransform = pActor->getLayoutManager()->getAnimTransform(pAnimName);

        if (pTransform == nullptr) {
            PL_LOG_WARN("game.layout", "getAnimFrameMax: no animation '%s' in layout '%s' (returning 0)",
                        pAnimName != nullptr ? pAnimName : "?",
                        pActor->getLayoutManager()->mLayoutName != nullptr ? pActor->getLayoutManager()->mLayoutName : "?");
            return 0;
        }

        return static_cast< s16 >(pTransform->GetFrameSize());
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
        LayoutPaneCtrl* pCtrl = rootCtrlWithLayerOrWarn(pActor, animLayer, "stopAnim");

        if (pCtrl != nullptr) {
            pCtrl->stop(animLayer);
        }
    }

    bool isAnimStopped(const LayoutActor* pActor, u32 animLayer) {
        LayoutPaneCtrl* pCtrl = rootCtrlWithLayerOrWarn(pActor, animLayer, "isAnimStopped");

        // PC_PORT: nothing can be playing on a missing controller/layer —
        // report "stopped" so nerves waiting on the animation move on
        // instead of hanging (the vendored code would have crashed here).
        return pCtrl == nullptr || pCtrl->isAnimStopped(animLayer);
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
