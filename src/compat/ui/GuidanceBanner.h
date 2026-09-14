#pragma once
// =============================================================================
// compat/ui — GuidanceBanner (PC_PORT M10.1): the StarPointer guidance balloon
// — the blue rounded bar with "Please choose a file." that sits under the
// planets on the file-select screen.
//
// -----------------------------------------------------------------------------
// Where this string comes from (and why the screen mixed languages)
// -----------------------------------------------------------------------------
// The bar is NOT a pane of FileSelect.arc: that layout only carries the
// operation buttons and the 2P badge (see the pane tree in docs/fileselect.md).
// On the console the bar is the StarPointer's 1P guidance window, requested
// when the file-select stage comes up:
//
//     Game/Util/StarPointerUtil.cpp:912   request1PGuidance("System_FileSelect008", true)
//
// and the text is the message "System_FileSelect008" of the current language.
// The star-pointer actor (and its layout arc) is not ported, so nothing drew
// the bar: reference capture 1 and 2 show it, the M10.1 build did not, and the
// fallback text of the start button — "Please choose a file." — had been
// guessed onto the wrong panes (the FileSelect button label is "Play This
// File"; see GameTextTable.cpp).
//
// -----------------------------------------------------------------------------
// Geometry: measured off the reference capture, not invented
// -----------------------------------------------------------------------------
// docs/images/fileselect/1_seleccion_save.png is 1920x1080, i.e. uiScale = 1080/456 = 2.3684.
// The bar's fill (53,133,192) was flood-measured on that capture:
//
//     bar box        1165 x 97 px      -> 491.9 x 41.0 design units
//     bar centre     x = screen centre -> 0 units
//     bar bottom     990 px            -> y = -190.0 units
//     text box       607 x 44 px       -> 256.3 x 18.6 units (cap height)
//     text colour    (243,255,255)  -> white; the bar's border is (0,73,122)
//
// A cap height of 18.6 units at the message font's size 26 (its em box) is the
// expected ~0.71 ratio, so the guidance text is simply the message font at its
// own size. The English text is 256.3 units wide, and the bar is exactly
// 492 - 256 = 236 units wider than the text: 118 units of padding per side,
// which is what the geometry below uses (a longer localized string therefore
// widens the bar the same way the console's window does).
// =============================================================================

#include <revolution/types.h>

namespace compat::ui {

// Design units (608 x 456 space) — see the measurements above.
constexpr f32 kGuidanceBarHeight = 41.0f;
constexpr f32 kGuidanceBarBottomY = -190.0f;  // bottom edge, Y up
constexpr f32 kGuidancePaddingX = 118.0f;     // per side
constexpr f32 kGuidanceFontSize = 26.0f;      // the message font's own size
constexpr f32 kGuidanceCornerRadius = kGuidanceBarHeight * 0.5f;

/// The balloon's framebuffer-pixel geometry. Everything is top-left origin,
/// +Y down (the space the host 2D passes draw in).
struct GuidanceBalloonLayout {
    f32 barLeft = 0.0f;
    f32 barTop = 0.0f;
    f32 barWidth = 0.0f;
    f32 barHeight = 0.0f;
    f32 cornerRadius = 0.0f;
    f32 fontSize = 0.0f;    // pixels
    f32 textLeft = 0.0f;    // the writer's cursor origin
    f32 textTop = 0.0f;
    f32 textWidth = 0.0f;   // pixels, as measured by the caller's writer
};

/// Pure layout: `textWidthUnits` is the string's width in design units (the
/// caller measures it with the real font) and `uiScale` maps design units to
/// pixels. At uiScale = 2.3684 and the English text (256.3 units) this returns
/// the bar of the reference capture (1165 x 97 px, bottom edge y = 990).
GuidanceBalloonLayout layoutGuidanceBalloon(f32 textWidthUnits, f32 uiScale, f32 fbWidth, f32 fbHeight);

/// Draws the balloon (rounded blue bar + the centred text) in the CURRENT
/// framebuffer, in pixels. Requires the message font of the current language
/// (MR::getFontOnCurrentLanguage) to be mounted; returns false and draws
/// nothing otherwise. Sets up its own GX state (ortho + identity position
/// matrix + pass-through TEV for the bar, the writer's own state for the text),
/// so it can be called straight after the layout pass.
bool drawGuidanceBalloon(const wchar_t* pText);

}  // namespace compat::ui
