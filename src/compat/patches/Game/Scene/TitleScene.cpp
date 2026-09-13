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
#include "Game/Util/ScreenUtil.hpp"
#include "compat/game/UiAnchoring.h"
#include "compat/j3d/TitleSky.h"
#include "platform/Log/Log.h"
#include "platform/Timing/Timing.h"

#include <revolution/gx.h>
#include <revolution/mtx.h>

#include <cmath>
#include <new>

namespace {
    NEW_NERVE(TitleSceneTitle, TitleScene, Title);
    NEW_NERVE(TitleSceneEnd, TitleScene, End);
};  // namespace

TitleScene::TitleScene() : Scene("TitleScene"), mTitle(nullptr), mEnded(false), mSky() {
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

    // PC_PORT (M9.5.4 v8): the real backdrop. FileSelector::createSky makes
    // the FileSelectSky actor on the console; here the compat J3D stand-in
    // loads /ObjectData/CometNearOrbitSky.arc. Missing/unparseable archive →
    // mSky stays null and draw() keeps the v7 starfield.
    std::unique_ptr< compat::j3d::TitleSky > sky(new compat::j3d::TitleSky());
    if (sky->init()) {
        mSky = std::move(sky);
        PL_LOG_INFO("boot", "TitleScene::init: CometNearOrbitSky backdrop ready");
    }
}

void TitleScene::exeTitle() {
    // PC_PORT (M9.5.4): belt-and-braces for the crash reported on Windows
    // (TitleSequenceProduct::appear on a null `this` from here). The root
    // cause was the scene controller running update() while init() was still
    // executing on the async worker (see MR::tryEndFunctionAsyncExecute in
    // compat/game/GameBoot.cpp); that is fixed at the source, but a scene
    // update must never dereference a product that does not exist yet.
    if (mTitle == nullptr) {
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            PL_LOG_WARN("boot", "TitleScene::exeTitle: update before init finished (mTitle null) — skipped");
        }
        return;
    }

    if (MR::isFirstStep(this)) {
        mTitle->appear();
    }

    // PC_PORT (M9.5.4): TitleSequenceProduct is a plain NerveExecutor (not a
    // NameObj), so nothing else steps its spine — without this the sequence
    // stayed parked in BgmPrepare forever (black screen + heartbeat only).
    // Steps the real chain: BgmPrepare -> LogoFadein -> LogoWait ->
    // LogoDisplay (A+B) -> Decide -> Dead.
    mTitle->updateNerve();

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
    // PC_PORT (M9.5.4 v8): FileSelectSky::exeWait + calcAnim (the actor is
    // not in the NameObj lists on the host, so the scene steps it).
    if (mSky && !mEnded) {
        mSky->update();
    }
}

namespace {
    // PC_PORT (M9.5.4 v6/v7): stand-in for the console's FileSelectSky — the
    // "CometNearOrbitSky" J3D model that fills the title background (a slowly
    // rotating star dome with nebula layers). J3D is not ported yet
    // (M9.5.4), so the scene draws a host backdrop through the same untextured
    // GX immediate path MR::fillScreen uses (vertex colours, PASSCLR):
    //   * a breathing deep-space gradient (v7: slow colour drift),
    //   * three parallax star layers that drift across the screen and wrap
    //     (v7: far layer slowest — the dome's rotation as seen from inside),
    //   * per-star twinkle and a few "comet dust" sparkles (v7).
    // Everything is a pure function of time (Platform::Timing::nowSeconds) so
    // it is frame-rate independent, and the star scatter keeps the v6 seed so
    // screenshots stay comparable. Replace with FileSelectSky once J3D lands.
    struct BackdropStar {
        f32 x, y;      // 0..1 normalised
        f32 size;
        f32 phase;     // twinkle phase
        f32 rate;      // twinkle speed
        u8 layer;      // 0 far, 1 mid, 2 near
        u8 lum;
    };

    const int cBackdropStarNum = 200;
    BackdropStar sBackdropStars[cBackdropStarNum];
    bool sBackdropInit = false;

    void initBackdropStars() {
        u32 seed = 0x2545F491u;
        auto next = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return (seed >> 8) & 0xFFFFFFu;
        };

        for (int i = 0; i < cBackdropStarNum; i++) {
            BackdropStar& star = sBackdropStars[i];
            star.x = static_cast< f32 >(next() % 4096u) / 4096.0f;
            star.y = static_cast< f32 >(next() % 4096u) / 4096.0f;
            const u32 tier = next() % 3u;
            star.layer = static_cast< u8 >(tier);
            star.size = tier == 2 ? 2.0f : (tier == 1 ? 1.5f : 1.0f);
            star.lum = tier == 0 ? 90 : (tier == 1 ? 150 : 230);
            star.phase = static_cast< f32 >(next() % 1024u) * (6.2831853f / 1024.0f);
            star.rate = 0.6f + static_cast< f32 >(next() % 1024u) / 1024.0f * 2.4f;
        }

        sBackdropInit = true;
    }

    f32 wrap01(f32 v) {
        v = v - static_cast< f32 >(static_cast< int >(v));
        return v < 0.0f ? v + 1.0f : v;
    }

    u8 toByte(f32 v) {
        return static_cast< u8 >(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    }

    void drawSpaceBackdrop() {
        if (!sBackdropInit) {
            initBackdropStars();
        }

        // PC_PORT: this fallback backdrop (no CometNearOrbitSky.arc) draws in
        // PIXELS through the ortho set below, so both extents come from the
        // framebuffer — using the 456-unit design height here squashed the
        // gradient and the star field vertically on any PC window.
        f32 width = 0.0f;
        f32 height = 0.0f;
        compat::ui::framebufferSize(&width, &height);
        const f32 t = static_cast< f32 >(Platform::Timing::nowSeconds());

        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

        Mtx mtxImm;
        PSMTXIdentity(mtxImm);
        GXLoadPosMtxImm(mtxImm, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);

        Mtx44 projMtx;
        C_MTXOrtho(projMtx, 0.0f, height, 0.0f, width, -1.0f, 1.0f);
        GXSetProjection(projMtx, GX_ORTHOGRAPHIC);

        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GXSetCullMode(GX_CULL_NONE);

        // Gradient: near-black violet at the top, deep blue at the bottom,
        // with a slow "breathing" of the blue and a faint horizontal band that
        // sweeps up (the nebula glow of the real sky dome, very toned down).
        const f32 breathe = 0.5f + 0.5f * sinf(t * 0.35f);
        const f32 bandY = wrap01(t * 0.02f) * height;
        const f32 bandHalf = height * 0.25f;

        const u8 topR = 6, topG = 4, topB = toByte(18.0f + 6.0f * breathe);
        const u8 botR = toByte(14.0f + 6.0f * breathe), botG = toByte(22.0f + 8.0f * breathe), botB = toByte(64.0f + 18.0f * breathe);

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        GXPosition2f32(0.0f, 0.0f);
        GXColor4u8(topR, topG, topB, 255);
        GXPosition2f32(width, 0.0f);
        GXColor4u8(topR, topG, topB, 255);
        GXPosition2f32(width, height);
        GXColor4u8(botR, botG, botB, 255);
        GXPosition2f32(0.0f, height);
        GXColor4u8(botR, botG, botB, 255);
        GXEnd();

        // Nebula band: additive-free approximation — a slightly lighter strip
        // drawn as two quads fading to the gradient colour at its edges.
        {
            const f32 y0 = bandY - bandHalf;
            const f32 y1 = bandY;
            const f32 y2 = bandY + bandHalf;
            const u8 midR = toByte(22.0f + 6.0f * breathe), midG = toByte(20.0f + 6.0f * breathe), midB = toByte(70.0f + 20.0f * breathe);

            // Colour at an arbitrary height of the base gradient (for the edges).
            auto gradAt = [&](f32 y, u8& r, u8& g, u8& b) {
                const f32 k = y <= 0.0f ? 0.0f : (y >= height ? 1.0f : y / height);
                r = toByte(topR + (botR - topR) * k);
                g = toByte(topG + (botG - topG) * k);
                b = toByte(topB + (botB - topB) * k);
            };

            u8 r0, g0, b0, r2, g2, b2;
            gradAt(y0, r0, g0, b0);
            gradAt(y2, r2, g2, b2);

            GXBegin(GX_QUADS, GX_VTXFMT0, 8);
            GXPosition2f32(0.0f, y0);
            GXColor4u8(r0, g0, b0, 255);
            GXPosition2f32(width, y0);
            GXColor4u8(r0, g0, b0, 255);
            GXPosition2f32(width, y1);
            GXColor4u8(midR, midG, midB, 255);
            GXPosition2f32(0.0f, y1);
            GXColor4u8(midR, midG, midB, 255);

            GXPosition2f32(0.0f, y1);
            GXColor4u8(midR, midG, midB, 255);
            GXPosition2f32(width, y1);
            GXColor4u8(midR, midG, midB, 255);
            GXPosition2f32(width, y2);
            GXColor4u8(r2, g2, b2, 255);
            GXPosition2f32(0.0f, y2);
            GXColor4u8(r2, g2, b2, 255);
            GXEnd();
        }

        // Star field: three parallax layers drifting right-to-left (the
        // dome's rotation), wrapping at the edges, each star twinkling.
        static const f32 cLayerSpeed[3] = {0.004f, 0.009f, 0.016f};  // screen widths per second
        static const f32 cLayerDrop[3] = {0.0010f, 0.0020f, 0.0035f}; // slight diagonal

        GXBegin(GX_QUADS, GX_VTXFMT0, static_cast< u16 >(cBackdropStarNum * 4));
        for (int i = 0; i < cBackdropStarNum; i++) {
            const BackdropStar& star = sBackdropStars[i];
            const f32 x = wrap01(star.x - t * cLayerSpeed[star.layer]) * width;
            const f32 y = wrap01(star.y + t * cLayerDrop[star.layer]) * height;

            const f32 twinkle = 0.75f + 0.25f * sinf(t * star.rate + star.phase);
            const u8 lum = toByte(static_cast< f32 >(star.lum) * twinkle);
            const u8 warm = static_cast< u8 >(lum > 200 ? lum : (lum > 20 ? lum - 20 : lum));
            const f32 size = star.size + (star.layer == 2 ? 0.5f * twinkle : 0.0f);

            GXPosition2f32(x, y);
            GXColor4u8(warm, lum, lum, 255);
            GXPosition2f32(x + size, y);
            GXColor4u8(warm, lum, lum, 255);
            GXPosition2f32(x + size, y + size);
            GXColor4u8(warm, lum, lum, 255);
            GXPosition2f32(x, y + size);
            GXColor4u8(warm, lum, lum, 255);
        }
        GXEnd();

        // Comet dust: a handful of bright specks that streak diagonally and
        // respawn (the real dome has a comet trail sweeping the sky).
        const int cDustNum = 6;
        GXBegin(GX_QUADS, GX_VTXFMT0, static_cast< u16 >(cDustNum * 4));
        for (int i = 0; i < cDustNum; i++) {
            const f32 period = 7.0f + static_cast< f32 >(i) * 1.7f;
            const f32 phase = wrap01(t / period + static_cast< f32 >(i) * 0.37f);
            const f32 startX = wrap01(static_cast< f32 >(i) * 0.61803f + 0.1f);
            const f32 x = (startX + phase * 0.35f) * width;
            const f32 y = (0.05f + phase * 0.9f) * height;
            const f32 fade = phase < 0.1f ? phase / 0.1f : (phase > 0.8f ? (1.0f - phase) / 0.2f : 1.0f);
            const u8 lum = toByte(200.0f * fade);
            const u8 blue = toByte(255.0f * fade);

            GXPosition2f32(x, y);
            GXColor4u8(lum, lum, blue, 255);
            GXPosition2f32(x + 2.0f, y);
            GXColor4u8(lum, lum, blue, 255);
            GXPosition2f32(x + 2.0f, y + 2.0f);
            GXColor4u8(lum, lum, blue, 255);
            GXPosition2f32(x, y + 2.0f);
            GXColor4u8(lum, lum, blue, 255);
        }
        GXEnd();
    }
};  // namespace

void TitleScene::draw() const {
    MR::drawInit();

    GXColor fillColor;
    fillColor.r = 0;
    fillColor.g = 0;
    fillColor.b = 0;
    fillColor.a = 255;

    if (!mEnded) {
        // PC_PORT (M9.5.4 v8): the real CometNearOrbitSky J3D dome
        // (compat/j3d TitleSky, MR::DrawBufferType_Sky pass of the console
        // scene). Falls back to the v6/v7 host starfield when the archive
        // is not available (see drawSpaceBackdrop).
        MR::fillScreen(fillColor);
        if (mSky) {
            mSky->draw();
        } else {
            drawSpaceBackdrop();
        }
        MR::clearZBuffer();
        MR::drawInitFor2DModel();
        CategoryList::execute(MR::DrawType_Layout);
    } else {
        // Parked: keep presenting pure black.
        MR::fillScreen(fillColor);
    }
}
