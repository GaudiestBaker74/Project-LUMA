// =============================================================================
// compat/ui — GuidanceBanner implementation (see GuidanceBanner.h).
// =============================================================================

#include "compat/ui/GuidanceBanner.h"

#include "Game/Util/SystemUtil.hpp"

#include "compat/game/UiAnchoring.h"
#include "platform/Log/Log.h"

#include <nw4r/ut/Color.h>
#include <nw4r/ut/Font.h>
#include <nw4r/ut/WideTextWriter.h>

#include <revolution/gx.h>
#include <revolution/mtx.h>

#include <cmath>
#include <cwchar>

namespace compat::ui {

namespace {

// The bar's colours, straight off the reference capture.
constexpr u8 kTopR = 72, kTopG = 158, kTopB = 212;    // the lighter upper band
constexpr u8 kFillR = 53, kFillG = 133, kFillB = 192; // the body
constexpr u8 kBorderR = 0, kBorderG = 73, kBorderB = 122;

/// Endian-safe nw4r colour (ut::Color IS a GXColor; the packed-u32 ctor is
/// 0xRRGGBBAA on the console and would swap on a little-endian host).
nw4r::ut::Color makeColor(u8 r, u8 g, u8 b, u8 a) {
    const GXColor gx = {r, g, b, a};
    return nw4r::ut::Color(gx);
}

/// Immediate colour-only geometry (the idiom of ButtonPrompt/drawCursor).
void beginBarGeometry() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZCompLoc(GX_FALSE);
}

void putVertex(f32 x, f32 y, u8 r, u8 g, u8 b) {
    GXPosition2f32(x, y);
    GXColor4u8(r, g, b, 255);
}

/// The bar's own vertical gradient: lighter at the top edge, the body's fill
/// towards the bottom (the reference capture shows exactly this banding).
void gradientAt(f32 t, u8* r, u8* g, u8* b) {
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }

    *r = static_cast< u8 >(kTopR + (kFillR - kTopR) * t);
    *g = static_cast< u8 >(kTopG + (kFillG - kTopG) * t);
    *b = static_cast< u8 >(kTopB + (kFillB - kTopB) * t);
}

/// A rounded rectangle with the vertical gradient: a middle quad plus one fan
/// per cap. The caps are half-discs (radius = height/2), which is what the
/// bar's fully-rounded ends are.
void drawRoundedBar(const GuidanceBalloonLayout& layout) {
    const f32 left = layout.barLeft;
    const f32 top = layout.barTop;
    const f32 right = left + layout.barWidth;
    const f32 bottom = top + layout.barHeight;
    const f32 radius = layout.cornerRadius;
    const f32 midY = top + layout.barHeight * 0.5f;

    u8 r = 0;
    u8 g = 0;
    u8 b = 0;

    // Middle.
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    gradientAt(0.0f, &r, &g, &b);
    putVertex(left + radius, top, r, g, b);
    putVertex(right - radius, top, r, g, b);
    gradientAt(1.0f, &r, &g, &b);
    putVertex(right - radius, bottom, r, g, b);
    putVertex(left + radius, bottom, r, g, b);
    GXEnd();

    constexpr int kSeg = 12;

    for (int side = 0; side < 2; ++side) {
        const f32 cx = side == 0 ? left + radius : right - radius;
        const f32 dir = side == 0 ? -1.0f : 1.0f;

        GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, static_cast< u16 >(kSeg + 2));
        gradientAt(0.5f, &r, &g, &b);
        putVertex(cx, midY, r, g, b);

        // Sweep the cap from its top (sy = -1) to its bottom (sy = +1).
        for (int i = 0; i <= kSeg; ++i) {
            const f32 ang = -1.5707963f + static_cast< f32 >(i) * (3.14159265f / static_cast< f32 >(kSeg));
            const f32 sy = std::sin(ang);
            gradientAt((sy * 0.5f) + 0.5f, &r, &g, &b);
            putVertex(cx + dir * std::cos(ang) * radius, midY + sy * radius, r, g, b);
        }
        GXEnd();
    }
}

/// Sets the pixel ortho + an identity position matrix (the host 2D convention,
/// same as the file-select cursor pass).
void setupPixelSpace(f32 fbWidth, f32 fbHeight) {
    Mtx mtxImm;
    PSMTXIdentity(mtxImm);
    GXLoadPosMtxImm(mtxImm, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    Mtx44 projMtx;
    C_MTXOrtho(projMtx, 0.0f, fbHeight, 0.0f, fbWidth, -1.0f, 1.0f);
    GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
}

}  // namespace

GuidanceBalloonLayout layoutGuidanceBalloon(f32 textWidthUnits, f32 uiScale, f32 fbWidth, f32 fbHeight) {
    GuidanceBalloonLayout layout;

    if (textWidthUnits < 0.0f) {
        textWidthUnits = 0.0f;
    }

    layout.barWidth = (textWidthUnits + kGuidancePaddingX * 2.0f) * uiScale;
    layout.barHeight = kGuidanceBarHeight * uiScale;
    layout.cornerRadius = kGuidanceCornerRadius * uiScale;
    layout.fontSize = kGuidanceFontSize * uiScale;
    layout.textWidth = textWidthUnits * uiScale;

    layout.barLeft = (fbWidth - layout.barWidth) * 0.5f;

    // The design space is anchored to the framebuffer height (UiAnchoring: 456
    // units span the screen vertically), so a design-unit y maps to
    //     pixelY = fbHeight * 0.5 - y * uiScale
    // and the bar's BOTTOM edge is at kGuidanceBarBottomY.
    const f32 barBottomPx = fbHeight * 0.5f - kGuidanceBarBottomY * uiScale;
    layout.barTop = barBottomPx - layout.barHeight;

    // The text is centred in the bar; the writer's pen is the top-left corner
    // of the text's em box, so the box is centred on both axes.
    layout.textLeft = (fbWidth - layout.textWidth) * 0.5f;
    layout.textTop = layout.barTop + (layout.barHeight - layout.fontSize) * 0.5f;

    return layout;
}

bool drawGuidanceBalloon(const wchar_t* pText) {
    if (pText == nullptr || pText[0] == L'\0') {
        return false;
    }

    nw4r::ut::Font* pFont = MR::getFontOnCurrentLanguage();

    if (pFont == nullptr) {
        PL_LOG_WARN("fileselect", "guidance balloon: no message font mounted — the bar stays off");
        return false;
    }

    f32 fbWidth = 0.0f;
    f32 fbHeight = 0.0f;
    framebufferSize(&fbWidth, &fbHeight);

    if (fbWidth <= 0.0f || fbHeight <= 0.0f) {
        return false;
    }

    const f32 uiScale = compat::ui::uiScale(fbWidth, fbHeight);
    const int textLen = static_cast< int >(std::wcslen(pText));

    // Measure in DESIGN units (the geometry of the balloon is defined there),
    // then lay it out in pixels.
    nw4r::ut::WideTextWriter writer;
    writer.SetFont(*pFont);
    writer.SetFontSize(kGuidanceFontSize, kGuidanceFontSize);

    nw4r::ut::Rect textRect;
    writer.CalcStringRect(&textRect, pText, textLen);

    const GuidanceBalloonLayout layout = layoutGuidanceBalloon(textRect.GetWidth(), uiScale, fbWidth, fbHeight);

    setupPixelSpace(fbWidth, fbHeight);

    // --- the bar: a thin dark border, then the body on top ------------------
    beginBarGeometry();

    GuidanceBalloonLayout border = layout;
    const f32 borderPx = 2.0f * uiScale;
    border.barLeft -= borderPx * 0.5f;
    border.barTop -= borderPx * 0.5f;
    border.barWidth += borderPx;
    border.barHeight += borderPx;
    border.cornerRadius += borderPx * 0.5f;

    {
        const u8 r = kBorderR;
        const u8 g = kBorderG;
        const u8 b = kBorderB;
        const f32 left = border.barLeft;
        const f32 top = border.barTop;
        const f32 right = left + border.barWidth;
        const f32 bottom = top + border.barHeight;
        const f32 radius = border.cornerRadius;
        const f32 midY = top + border.barHeight * 0.5f;

        GXBegin(GX_QUADS, GX_VTXFMT0, 4);
        putVertex(left + radius, top, r, g, b);
        putVertex(right - radius, top, r, g, b);
        putVertex(right - radius, bottom, r, g, b);
        putVertex(left + radius, bottom, r, g, b);
        GXEnd();

        for (int side = 0; side < 2; ++side) {
            const f32 cx = side == 0 ? left + radius : right - radius;
            const f32 dir = side == 0 ? -1.0f : 1.0f;

            GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, 14);
            putVertex(cx, midY, r, g, b);

            for (int i = 0; i <= 12; ++i) {
                const f32 ang = -1.5707963f + static_cast< f32 >(i) * (3.14159265f / 12.0f);
                putVertex(cx + dir * std::cos(ang) * radius, midY + std::sin(ang) * radius, r, g, b);
            }
            GXEnd();
        }
    }

    drawRoundedBar(layout);

    // --- the text -----------------------------------------------------------
    // Same writer settings the layout text boxes use: the message font's ink is
    // its alpha channel, so the colour mapping has to be opaque white.
    writer.SetColorMapping(makeColor(0, 0, 0, 0), makeColor(255, 255, 255, 255));
    writer.SetGradationMode(nw4r::ut::CharWriter::GRADMODE_NONE);
    writer.SetFontSize(layout.fontSize, layout.fontSize);

    // A soft dark edge first (it is what keeps the white readable over the
    // starfield), then the white text on top.
    writer.SetTextColor(makeColor(0, 0, 0, 0x60));
    writer.SetupGX();
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    writer.SetCursor(layout.textLeft + 1.5f * uiScale, layout.textTop + 1.5f * uiScale);
    writer.Print(pText, textLen);

    writer.SetTextColor(makeColor(255, 255, 255, 255));
    writer.SetupGX();
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    writer.SetCursor(layout.textLeft, layout.textTop);
    writer.Print(pText, textLen);

    return true;
}

}  // namespace compat::ui
