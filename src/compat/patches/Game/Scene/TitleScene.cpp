// =============================================================================
// PC_PORT (M9.5.3d) — TitleScene (see TitleScene.hpp for the scope notes).
//
// Structure mirrors the vendored LogoScene patch: the scene runs a small
// nerve machine (Title → End), the vendored TitleSequenceProduct drives the
// actual sequence (BgmPrepare → LogoFadein → LogoWait → LogoDisplay → Decide
// → Dead) and its SimpleLayouts draw through the M9.5.3 layout stack.
// =============================================================================

#include "Game/Scene/TitleScene.hpp"

#include "Game/LiveActor/Nerve.hpp"
#include "Game/Screen/TitleSequenceProduct.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/NerveUtil.hpp"
#include "platform/Log/Log.h"

#include <new>

namespace {
    NEW_NERVE(TitleSceneTitle, TitleScene, Title);
    NEW_NERVE(TitleSceneEnd, TitleScene, End);
};  // namespace

TitleScene::TitleScene() : Scene("TitleScene"), mTitle(nullptr), mEnded(false) {
}

TitleScene::~TitleScene() {
}

void TitleScene::init() {
    PL_LOG_INFO("boot", "TitleScene::init: begin");
    initNerve(&TitleSceneTitle::sInstance);

    SceneFunction::createHioBasicNode(this);
    SceneFunction::initForNameObj();
    MR::createSceneObj(SceneObj_CameraContext);
    MR::createSceneObj(SceneObj_NameObjGroup);

    // The product's ctor creates its layouts (LogoLayout "TitleLogo" +
    // PressStart); missing arcs degrade through the M9.5.3a null-layout path.
    mTitle = new TitleSequenceProduct();
    PL_LOG_INFO("boot", "TitleScene::init: TitleSequenceProduct created");
}

void TitleScene::exeTitle() {
    if (MR::isFirstStep(this)) {
        mTitle->appear();
    }

    if (!mTitle->isActive()) {
        setNerve(&TitleSceneEnd::sInstance);
    }
}

void TitleScene::exeEnd() {
    // PC_PORT: parked. The console re-requests the Title (endScene →
    // requestChangeSceneTitle); without FileSelector (M10) that would loop
    // the whole intro forever, so we hold the last frame instead.
    if (MR::isFirstStep(this)) {
        mEnded = true;
        PL_LOG_INFO("boot",
                    "TitleScene: sequence ended (Decide) — FileSelector is M10, "
                    "the title parks here");
    }
}

void TitleScene::update() {
    static int sUpdCount = 0;
    ++sUpdCount;
    if (sUpdCount % 600 == 0) {
        PL_LOG_INFO("boot", "TitleScene: alive, %d updates (~%d s), ended=%d", sUpdCount,
                    sUpdCount / 60, static_cast< int >(mEnded));
    }

    updateNerve();
    SceneFunction::executeMovementList();
}

void TitleScene::calcAnim() {
    SceneFunction::executeCalcAnimList();
    SceneFunction::executeCalcViewAndEntryList2D();
}

void TitleScene::draw() const {
    MR::drawInit();

    // Black backdrop (M9.5.3d scope — the console draws FileSelectSky here).
    GXColor fillColor;
    fillColor.r = 0;
    fillColor.g = 0;
    fillColor.b = 0;
    fillColor.a = 255;

    if (!mEnded) {
        MR::fillScreen(fillColor);
        MR::clearZBuffer();
        MR::drawInitFor2DModel();
        CategoryList::execute(MR::DrawType_Layout);
    } else {
        // Parked: keep presenting pure black.
        MR::fillScreen(fillColor);
    }
}
