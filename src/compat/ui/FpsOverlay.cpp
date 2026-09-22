// =============================================================================
// compat/ui — FpsOverlay implementation (see FpsOverlay.h).
//
// The draw idiom is the one GuidanceBanner established for host text inside
// the EFB pass: pixel ortho + the game's message font through a
// nw4r::ut::WideTextWriter (the font's ink is its alpha, hence the opaque
// white colour mapping), shadowed once so it stays readable over any scene.
// =============================================================================

#include "compat/ui/FpsOverlay.h"

#include "Game/Util/SystemUtil.hpp"

#include "compat/game/UiAnchoring.h"
#include "platform/Log/Log.h"
#include "platform/Timing/Timing.h"

#include <nw4r/ut/Color.h>
#include <nw4r/ut/Font.h>
#include <nw4r/ut/WideTextWriter.h>

#include <revolution/gx.h>
#include <revolution/mtx.h>

#include <cmath>
#include <cstdio>
#include <cwchar>

namespace compat::ui {

namespace {

bool sEnabled = false;

// Frame-rate sampling: count drawn frames per half-second window.
bool sSampling = false;
bool sHaveFps = false;
Platform::Timing::TimePoint sWindowStart;
int sWindowFrames = 0;
int sFps = 0;

bool sWarnedNoFont = false;

/// Endian-safe nw4r colour (same note as GuidanceBanner: the packed-u32 ctor
/// is 0xRRGGBBAA on the console and would swap on a little-endian host).
nw4r::ut::Color makeColor(u8 r, u8 g, u8 b, u8 a) {
    const GXColor gx = {r, g, b, a};
    return nw4r::ut::Color(gx);
}

/// Pixel ortho + identity position matrix (the host 2D convention).
void setupPixelSpace(f32 fbWidth, f32 fbHeight) {
    Mtx mtxImm;
    PSMTXIdentity(mtxImm);
    GXLoadPosMtxImm(mtxImm, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    Mtx44 projMtx;
    C_MTXOrtho(projMtx, 0.0f, fbHeight, 0.0f, fbWidth, -1.0f, 1.0f);
    GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
}

} // namespace

void setFpsOverlayEnabled(bool enabled) {
    if (enabled && !sEnabled) {
        PL_LOG_INFO("ui", "--show-fps: FPS overlay enabled (top-right, game message font)");
    }
    sEnabled = enabled;
}

bool fpsOverlayEnabled() {
    return sEnabled;
}

void drawFpsOverlayIfEnabled() {
    if (!sEnabled) {
        return;
    }

    // --- sample the frame rate ----------------------------------------------
    const Platform::Timing::TimePoint now = Platform::Timing::now();
    if (!sSampling) {
        sSampling = true;
        sWindowStart = now;
        sWindowFrames = 0;
    }
    ++sWindowFrames;
    const double elapsed = Platform::Timing::secondsBetween(sWindowStart, now);
    if (elapsed >= 0.5) {
        sFps = static_cast< int >(std::lround(static_cast< double >(sWindowFrames) / elapsed));
        sWindowStart = now;
        sWindowFrames = 0;
        sHaveFps = true;
    }
    if (!sHaveFps) {
        return; // first half-second: no number yet
    }

    // --- draw it with the game's own message font ----------------------------
    nw4r::ut::Font* pFont = MR::getFontOnCurrentLanguage();
    if (pFont == nullptr) {
        if (!sWarnedNoFont) {
            sWarnedNoFont = true;
            PL_LOG_WARN("ui", "fps overlay: no message font mounted (Font.arc) — the counter stays off");
        }
        return;
    }

    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    framebufferSize(&fbWidth, &fbHeight);
    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return;
    }
    const f32 uiScale = compat::ui::uiScale(fbWidth, fbHeight);

    wchar_t text[32];
    std::swprintf(text, sizeof(text) / sizeof(text[0]), L"%d FPS", sFps);
    const int textLen = static_cast< int >(std::wcslen(text));

    nw4r::ut::WideTextWriter writer;
    writer.SetFont(*pFont);
    const f32 fontSize = 22.0f * uiScale;
    writer.SetFontSize(fontSize, fontSize);

    nw4r::ut::Rect textRect;
    writer.CalcStringRect(&textRect, text, textLen);

    setupPixelSpace(fbWidth, fbHeight);

    const f32 margin = 12.0f * uiScale;
    const f32 left = fbWidth - margin - textRect.GetWidth();
    const f32 top = margin;

    // Same writer settings the layout text boxes use: the message font's ink
    // is its alpha channel, so the colour mapping has to be opaque white. A
    // dark edge first keeps the digits readable over any backdrop.
    writer.SetColorMapping(makeColor(0, 0, 0, 0), makeColor(255, 255, 255, 255));
    writer.SetGradationMode(nw4r::ut::CharWriter::GRADMODE_NONE);

    writer.SetTextColor(makeColor(0, 0, 0, 0xA0));
    writer.SetupGX();
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    writer.SetCursor(left + 1.5f * uiScale, top + 1.5f * uiScale);
    writer.Print(text, textLen);

    writer.SetTextColor(makeColor(255, 255, 255, 255));
    writer.SetupGX();
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    writer.SetCursor(left, top);
    writer.Print(text, textLen);
}

} // namespace compat::ui
