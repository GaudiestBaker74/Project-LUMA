#pragma once
// =============================================================================
// compat/game — UiAnchoring (PC_PORT, title-screen widescreen work).
//
// How the 608 x 456 layout space (SMG's design space) is mapped onto the host
// framebuffer.
//
// -----------------------------------------------------------------------------
// The console rule (Game/Util/ScreenUtil.cpp)
// -----------------------------------------------------------------------------
//
//      getScreenHeight() == 456                     always
//      getScreenWidth()  == 608 (4:3) / 832 (16:9)
//
// The layout space is scaled UNIFORMLY so that 456 units span the framebuffer
// height; how many layout units fit horizontally is a property of the aspect
// ratio, not something the layout engine stretches. That is what keeps the UI
// identical to the original composition: at 4:3 there are 608 units of width to
// play with, at 16:9 there are 832 — the extra width is *more space*, not a
// wider logo.
//
// -----------------------------------------------------------------------------
// What the host did before this module (the bug)
// -----------------------------------------------------------------------------
//
// LayoutManager::draw() called MR::setupDrawForNW4RLayout(1.0f), whose ortho is
//     C_MTXOrtho(v1, -v1, -304, 304)   with v1 = getScreenHeight() * 0.5
// i.e. 608 layout units were mapped to the WHOLE framebuffer width while 456
// units were mapped to the whole height:
//
//     x pixel scale = W / 608          (1280/608 = 2.105 at 1280x720)
//     y pixel scale = H / 456          ( 720/456 = 1.579 at 1280x720)
//
// so every UI pane was 33% wider than tall — the logo and the "Press both …"
// line were horizontally stretched — and the amount depended on the aspect
// ratio (the composition changed with the resolution/window size).
//
// -----------------------------------------------------------------------------
// The mapping implemented here
// -----------------------------------------------------------------------------
//
//     scale = min(H, W * 456 / 608) / 456        layout units -> pixels
//     screen = (X_center, Y_center) + layout * scale
//
// * 456 units always span the screen height => the UI is anchored to the
//   vertical extent of the viewport, never to its width.
// * The scale never depends on the width unless the framebuffer is NARROWER
//   than 4:3 (then the 608-unit design space would overflow horizontally and
//   the whole composition is shrunk to fit — a phone-shaped window, not a case
//   the console ever had to deal with).
// * Because the scale and the centring are identical for every pane, the
//   whole composition (logo, prompt, copyright) is centred on the screen and
//   keeps its relative offsets at every resolution.
//
// The background/sky must NOT use this mapping: sky actors are world-space 3D.
// On the console the title camera keeps its 60 deg VERTICAL field of view and
// takes the aspect ratio from the framebuffer (CameraContext::getAspect()), so
// a wider window reveals more sky at the sides instead of magnifying the dome —
// see compat/j3d/TitleSky.cpp. Both halves are driven by the same framebuffer
// geometry, which is what framebufferSize() below hands out.
// =============================================================================

#include <revolution/types.h>

namespace compat::ui {

// SMG's design space.
constexpr f32 kDesignHeight = 456.0f;
constexpr f32 kDesignWidth4x3 = 608.0f;
constexpr f32 kDesignWidth16x9 = 832.0f;

// 832 / 456: the widest aspect ratio whose layout space the game itself
// defines (the Wii's 16:9 EFB is 832x456).
constexpr f32 kWideLayoutAspect = kDesignWidth16x9 / kDesignHeight;  // 1.8246

/// Host framebuffer size in PIXELS: the render mode the boot presents with
/// (fbWidth x efbHeight, re-synced to the window every frame by
/// MainLoopFramework::beginRender). Every host path that needs a pixel height
/// must read this and NOT MR::getScreenHeight(): that one returns the 456-unit
/// DESIGN height (Game/Util/ScreenUtil.cpp), which equals the framebuffer height
/// on the console but not on a PC window — feeding 456 into the mapping below at
/// 1920x1080 produced scale 1.0 instead of 2.37, i.e. a logo 2.4x too small and
/// squeezed into the top 456 rows.
/// Falls back to the 4:3 design size when no host render mode exists (headless
/// unit tests).
void framebufferSize(f32* outPixelWidth, f32* outPixelHeight);

/// Framebuffer aspect ratio; <= 0 when the height is not usable yet.
f32 aspectRatio(f32 pixelWidth, f32 pixelHeight);

/// True when 832 layout units fit inside the framebuffer at the uniform scale,
/// i.e. the game's 16:9 layout space is the one to use (the console's
/// MR::isScreen16Per9 branch).
bool usesWideLayoutSpace(f32 pixelWidth, f32 pixelHeight);

/// The layout space width the game would use for this framebuffer: 608 in 4:3,
/// 832 in 16:9 (ScreenUtil::getScreenWidth). Anything wider than 16:9 keeps
/// 832 — the design space does not grow with the window, the viewport does.
f32 layoutSpaceWidth(f32 pixelWidth, f32 pixelHeight);

/// Uniform layout-units -> pixels scale (see the header comment).
f32 uiScale(f32 pixelWidth, f32 pixelHeight);

/// The width of the visible layout space at this aspect ratio, in design units
/// (the scale's inverse: W / uiScale()). 608 at 4:3, 832 near 16:9, more on
/// ultrawide. This is the ortho width a pass needs when it draws in "layout
/// space" and must stay square-pixel (see MR::drawInitFor2DModel).
f32 visibleLayoutWidth(f32 pixelWidth, f32 pixelHeight);

// -----------------------------------------------------------------------------
// Screen-covering panes (PC_PORT: the title intro's flash).
//
// A pane whose size covers the whole 4:3 design area is a SCREEN-COVERING
// EFFECT, not UI: the TitleLogo layout's PicFlash is an 8x8 texture stretched
// over the screen and faded in/out by the logo's "Appear" animation. Drawn at
// the design size it covers 608x456 units = 960x720 px at 1280x720, so on a
// widescreen framebuffer the sides of the frame kept the UNFLASHED scene and
// the boundary showed as a hard vertical cut at the 4:3 edges (the intro-flash
// bug). The game's own 16:9 layout space is 832 units wide, so such a pane has
// to be widened until it covers the framebuffer.
//
// The widening is symmetric about the pane's own centre, which for a pane that
// covers the design area is the screen centre — i.e. the same rule the
// background follows: extend the sides, keep the centre.
//
// Returns the factor to scale the pane's WIDTH by: 1.0f for ordinary panes and
// for any framebuffer that is not wider than the design space.
// -----------------------------------------------------------------------------
f32 screenCoveringPaneScaleXForFramebuffer(f32 paneWidth, f32 paneHeight, f32 pixelWidth,
                                           f32 pixelHeight);

/// Same, for the framebuffer the boot is presenting right now (headless: the
/// design size, so the factor is 1).
f32 screenCoveringPaneScaleX(f32 paneWidth, f32 paneHeight);

/// Layout-space point -> framebuffer pixel (top-left origin, +Y down).
/// +Y in layout space is UP on screen (the converted-position convention of
/// MR::convertLayoutPosToScreenPos); raw pane translations are Y-down.
void layoutToScreen(f32 layoutX, f32 layoutY, f32 pixelWidth, f32 pixelHeight, f32* outPixelX,
                    f32* outPixelY);

/// Fills a host (row-major, translation in [_03/_13/_23]) nw4r MTX34 implementing
/// the mapping above, to be installed as the layout DrawInfo's view matrix.
/// Pane::CalculateMtx composes `mGlbMtx = viewMtx * localMtx`, so this is the
/// single place where the layout space meets the framebuffer.
void fillUiViewMtx(f32 pixelWidth, f32 pixelHeight, f32 outMtx[3][4]);

// -----------------------------------------------------------------------------
// "Press both [A] and [B]." — block layout (pure math, unit-testable).
//
// The instruction line is ONE block: six slots (four words, the two button
// icons and the period) separated by fixed gaps. Individual words are never
// centred on their own; the total width is computed first and then the block is
// centred on `centerX`:
//
//     totalWidth = Σ slot widths + Σ gaps
//     startX     = centerX - totalWidth / 2
// -----------------------------------------------------------------------------
enum class PromptSlot {
    Press = 0,
    Both,
    ButtonA,
    And,
    ButtonB,
    Period,
    Count
};

constexpr int kPromptSlotCount = static_cast<int>(PromptSlot::Count);

struct PromptBlock {
    f32 x[kPromptSlotCount];   // pen x of each slot (layout units)
    f32 totalWidth;
    f32 startX;
    f32 endX;
    f32 baselineY;             // vertical centre of the block (layout units)
};

/// `wordWidths` holds the measured widths of the four words (Press, both, and,
/// plus the period width in `periodWidth`); the button slots are `buttonSize`
/// wide. `wordGap` separates words from words/buttons, `periodGap` is the small
/// advance before the final period.
PromptBlock layoutPromptBlock(f32 centerX, f32 baselineY, const f32 wordWidths[3], f32 buttonSize,
                              f32 periodWidth, f32 wordGap, f32 periodGap);

}  // namespace compat::ui
