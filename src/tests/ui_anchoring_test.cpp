// =============================================================================
// UiAnchoring + ButtonPrompt tests (PC_PORT, title-screen widescreen work).
//
// These pin the two structural properties the title screen has to keep at every
// resolution:
//
//   1. the layout space is mapped UNIFORMLY (no horizontal stretch) and it is
//      anchored to the SCREEN CENTRE with a scale that only depends on the
//      framebuffer height — the UI never changes position or proportions with
//      the aspect ratio;
//   2. the instruction line is laid out as ONE block whose slots keep their
//      relative offsets, centred once (never word by word).
//
// They run headless (pure math, no GX device needed).
// =============================================================================

#include "compat/game/UiAnchoring.h"
#include "compat/ui/ButtonPrompt.h"
#include "compat/ui/PictureGlyphs.h"
#include "compat/vi/VICompat.h"

#include "test_runner.h"

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using compat::ui::kDesignHeight;
using compat::ui::kDesignWidth16x9;
using compat::ui::kDesignWidth4x3;

namespace {

bool nearlyEqual(f32 a, f32 b, f32 epsilon = 1.0e-4f) {
    return std::fabs(a - b) <= epsilon;
}

}  // namespace

TEST_CASE(ui_anchoring_scale_is_uniform_at_every_aspect) {
    struct Resolution {
        f32 w, h;
    };
    // The five aspect ratios the port must support (the console's 4:3 and the
    // widescreen modes players actually use) + a couple of extremes.
    const Resolution resolutions[] = {
        {1280.0f, 720.0f},   // 16:9
        {1920.0f, 1080.0f},  // 16:9
        {2560.0f, 1080.0f},  // 21:9
        {3440.0f, 1440.0f},  // 21:9
        {3840.0f, 2160.0f},  // 16:9 UHD
        {640.0f, 456.0f},    // the console framebuffer (4:3-ish)
        {640.0f, 480.0f},    // 4:3
        {1920.0f, 1200.0f},  // 16:10
        {800.0f, 1080.0f},   // narrower than 4:3 (must shrink, not stretch)
    };

    for (const Resolution& r : resolutions) {
        const f32 scale = compat::ui::uiScale(r.w, r.h);
        CHECK(scale > 0.0f);

        // 456 layout units span the framebuffer height (the console rule)...
        const f32 expectedHeightSpan = r.h;
        const f32 effectiveScale = (r.w * kDesignHeight / kDesignWidth4x3) < r.h
                                       ? r.w / kDesignWidth4x3  // narrow: fit width
                                       : r.h / kDesignHeight;
        CHECK(nearlyEqual(scale, effectiveScale));

        // ...and the scale never stretches the design space horizontally: the
        // 608-unit 4:3 space always fits (that is what the console relies on).
        if (r.w / r.h >= kDesignWidth4x3 / kDesignHeight) {
            CHECK(kDesignWidth4x3 * scale <= r.w + 0.5f);
        }
        // The game's 16:9 space is 832x456 = 1.8246 — slightly wider than a true
        // 16:9 (1.7778), because the Wii squeezed its anamorphic EFB on output.
        // So:
        //   * from 1.8246 up (21:9, 16:8.5, ...) the 832-unit space always fits;
        //   * in the 4:3..1.8246 band (16:10 monitors, the console's own 640x456)
        //     the window is narrower than the design space, and the extra width
        //     may fall outside the framebuffer. That is inherent to the game's
        //     own 16:9 layout space; the title composition is centred, so the
        //     logo/prompt/copyright are unaffected, and the sky/planet fill the
        //     frame regardless (the background never depends on this space).
        const f32 aspect = r.w / r.h;
        if (aspect >= kDesignWidth16x9 / kDesignHeight) {
            CHECK(kDesignWidth16x9 * scale <= r.w + 0.5f);
        } else if (compat::ui::usesWideLayoutSpace(r.w, r.h)) {
            CHECK(kDesignWidth16x9 * scale <= r.w * 1.30f);
        }

        // The centre of the layout space lands on the centre of the screen.
        f32 cx = 0.0f;
        f32 cy = 0.0f;
        compat::ui::layoutToScreen(0.0f, 0.0f, r.w, r.h, &cx, &cy);
        CHECK(nearlyEqual(cx, r.w * 0.5f, 0.01f));
        CHECK(nearlyEqual(cy, r.h * 0.5f, 0.01f));

        // Uniform means: equal layout distances give equal pixel distances in
        // both axes (this is what the old mapping violated: x used W/608 while
        // y used 1, i.e. 1.33x more pixels per unit horizontally at 1280x720).
        f32 ax = 0.0f;
        f32 ay = 0.0f;
        f32 bx = 0.0f;
        f32 by = 0.0f;
        compat::ui::layoutToScreen(100.0f, 0.0f, r.w, r.h, &ax, &ay);
        compat::ui::layoutToScreen(0.0f, 100.0f, r.w, r.h, &bx, &by);
        CHECK(nearlyEqual(std::fabs(ax - cx), std::fabs(by - cy), 0.01f));
        (void)expectedHeightSpan;
    }
}

TEST_CASE(ui_anchoring_wide_layout_space_threshold_and_helpers) {
    // Wider than 4:3 -> the game's 16:9 layout space, like the console's
    // MR::isScreen16Per9 branch.
    CHECK(compat::ui::usesWideLayoutSpace(1280.0f, 720.0f));
    CHECK(compat::ui::usesWideLayoutSpace(2560.0f, 1080.0f));
    CHECK(compat::ui::usesWideLayoutSpace(1920.0f, 1080.0f));
    CHECK(!compat::ui::usesWideLayoutSpace(640.0f, 480.0f));

    CHECK(nearlyEqual(compat::ui::layoutSpaceWidth(1920.0f, 1080.0f), kDesignWidth16x9));
    CHECK(nearlyEqual(compat::ui::layoutSpaceWidth(640.0f, 480.0f), kDesignWidth4x3));

    CHECK(nearlyEqual(compat::ui::aspectRatio(1920.0f, 1080.0f), 16.0f / 9.0f));
    CHECK(nearlyEqual(compat::ui::aspectRatio(0.0f, 0.0f), 0.0f));
}

TEST_CASE(ui_anchoring_view_mtx_places_panes_identically_at_every_resolution) {
    // Two panes of the real title composition (layout units; the layout's +Y is
    // screen-up, so these are the pane translations of TitleLogo/PressStart).
    const f32 logoY = -22.5f;
    const f32 promptY = -134.0f;

    struct Resolution {
        f32 w, h;
    };
    const Resolution resolutions[] = {
        {1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1080.0f}, {3440.0f, 1440.0f}, {3840.0f, 2160.0f},
    };

    for (const Resolution& r : resolutions) {
        f32 mtx[3][4];
        compat::ui::fillUiViewMtx(r.w, r.h, mtx);

        const f32 scale = compat::ui::uiScale(r.w, r.h);

        // The view matrix is applied under MR::setupDrawForNW4RLayout's ortho
        // (608 design units across the framebuffer width, 456 across its
        // height), so the effective layout->pixel scale of each axis is
        //     x: mtx[0][0] * W / 608      y: mtx[1][1] * H / 456
        // and both have to be uiScale — one layout unit, one size, both axes.
        const f32 pixelsPerUnitX = mtx[0][0] * r.w / kDesignWidth4x3;
        const f32 pixelsPerUnitY = mtx[1][1] * r.h / kDesignHeight;
        CHECK_NEAR(pixelsPerUnitX, scale, 0.001f * scale);
        CHECK_NEAR(pixelsPerUnitY, scale, 0.001f * scale);
        CHECK(nearlyEqual(mtx[0][3], 0.0f));  // no horizontal offset: anchored
        CHECK(nearlyEqual(mtx[1][3], 0.0f));  // on the screen centre

        // Both panes keep the same NORMALISED vertical position (relative to the
        // framebuffer height) at every resolution: that is "the composition does
        // not move". Layout +Y is up, so a -22.5 unit pane sits BELOW centre.
        f32 logoPx = 0.0f;
        f32 promptPx = 0.0f;
        f32 sx = 0.0f;
        compat::ui::layoutToScreen(0.0f, logoY, r.w, r.h, &sx, &logoPx);
        compat::ui::layoutToScreen(0.0f, promptY, r.w, r.h, &sx, &promptPx);
        CHECK_NEAR(logoPx, r.h * 0.5f - logoY * scale, 0.01f);
        CHECK_NEAR(promptPx, r.h * 0.5f - promptY * scale, 0.01f);

        const f32 logoNormalised = logoPx / r.h;
        const f32 promptNormalised = promptPx / r.h;
        CHECK_NEAR(logoNormalised, 0.5f - (logoY / kDesignHeight), 1.0e-4f);
        CHECK_NEAR(promptNormalised, 0.5f - (promptY / kDesignHeight), 1.0e-4f);

        // The screen centre maps to the layout origin in both directions (the
        // convert helpers are the inverse of the view matrix).
        f32 screenX = 0.0f;
        f32 screenY = 0.0f;
        compat::ui::layoutToScreen(0.0f, 0.0f, r.w, r.h, &screenX, &screenY);
        CHECK(nearlyEqual(screenX, r.w * 0.5f, 0.01f));
        CHECK(nearlyEqual(screenY, r.h * 0.5f, 0.01f));
    }
}

TEST_CASE(ui_anchoring_never_changes_the_composition_between_16_9_and_21_9) {
    // Same window height, different widths: the UI must not move or scale (the
    // background is the only thing that gains content at the sides).
    const f32 height = 1080.0f;
    const f32 widths[] = {1920.0f, 2560.0f, 3440.0f, 3840.0f};

    f32 referenceScale = 0.0f;
    for (f32 w : widths) {
        const f32 scale = compat::ui::uiScale(w, height);
        if (referenceScale == 0.0f) {
            referenceScale = scale;
        }
        CHECK(nearlyEqual(scale, referenceScale));
    }

    // And between 720p and 4K the scale is exactly 3x (the composition is the
    // same picture at a different size).
    CHECK(nearlyEqual(compat::ui::uiScale(3840.0f, 2160.0f),
                      3.0f * compat::ui::uiScale(1280.0f, 720.0f), 0.01f));
}

TEST_CASE(prompt_block_is_centred_as_a_whole_and_keeps_relative_offsets) {
    // Measured word widths (layout units) — arbitrary but realistic for the
    // title's font size.
    const f32 wordWidths[3] = {52.0f, 30.0f, 26.0f};  // Press / both / and
    const f32 buttonSize = 26.0f;
    const f32 periodWidth = 6.0f;
    const f32 wordGap = 8.0f;
    const f32 periodGap = 2.0f;

    const compat::ui::PromptBlock block = compat::ui::layoutPromptBlock(
        0.0f, -105.0f, wordWidths, buttonSize, periodWidth, wordGap, periodGap);

    const f32 expected =
        wordWidths[0] + wordGap + wordWidths[1] + wordGap + buttonSize + wordGap + wordWidths[2] +
        wordGap + buttonSize + periodGap + periodWidth;
    CHECK(nearlyEqual(block.totalWidth, expected));
    CHECK(nearlyEqual(block.startX, -expected * 0.5f));
    CHECK(nearlyEqual(block.endX, expected * 0.5f));
    CHECK(nearlyEqual(block.x[0], block.startX));

    // Slot order and relative offsets: Press, both, [A], and, [B], .
    CHECK(block.x[0] < block.x[1]);
    CHECK(block.x[1] < block.x[2]);
    CHECK(block.x[2] < block.x[3]);
    CHECK(block.x[3] < block.x[4]);
    CHECK(block.x[4] < block.x[5]);
    CHECK(nearlyEqual(block.x[1] - block.x[0], wordWidths[0] + wordGap));
    CHECK(nearlyEqual(block.x[3] - block.x[2], buttonSize + wordGap));
    CHECK(nearlyEqual(block.x[5] - block.x[4], buttonSize + periodGap));

    // Centring is relative to the caller's centre: shifting the centre shifts
    // the whole block by the same amount (the whole sentence moves as one).
    const compat::ui::PromptBlock shifted = compat::ui::layoutPromptBlock(
        100.0f, -105.0f, wordWidths, buttonSize, periodWidth, wordGap, periodGap);
    for (int i = 0; i < compat::ui::kPromptSlotCount; ++i) {
        CHECK(nearlyEqual(shifted.x[i] - block.x[i], 100.0f));
    }
    CHECK(nearlyEqual(shifted.totalWidth, block.totalWidth));
}

TEST_CASE(button_icon_metrics_scale_with_the_line) {
    const compat::ui::ButtonIconMetrics small = compat::ui::buttonIconMetrics(12.0f);
    const compat::ui::ButtonIconMetrics big = compat::ui::buttonIconMetrics(24.0f);

    // Both icons have the same HEIGHT (on the reference they read as equally
    // tall; the [B] tile is the narrower one) and they grow with the cap
    // height, so the line keeps its look at any resolution.
    CHECK(small.size > 0.0f);
    CHECK(nearlyEqual(big.size, small.size * 2.0f, 0.01f));
    CHECK(nearlyEqual(big.wordGap, small.wordGap * 2.0f, 0.01f));
    CHECK(small.cornerRatio > 0.0f && small.cornerRatio < 0.5f);
}

// PC_PORT (title widescreen): the icon geometry is a set of MEASUREMENTS off
// the original title screen (1280x720; the line's cap height there is 24 px),
// not a re-design. These numbers are the reproduction contract.
TEST_CASE(button_icons_match_the_original_title_screen) {
    const compat::ui::ButtonIconMetrics m = compat::ui::buttonIconMetrics(24.0f);

    CHECK(nearlyEqual(m.size, 38.9f, 0.6f));      // [A] outer disc: 39 px
    CHECK(nearlyEqual(m.faceSize, 31.9f, 0.6f));  // ... its face: 32 px
    CHECK(nearlyEqual(m.capHeight, 24.0f, 0.01f)); // the line's cap height
    CHECK(nearlyEqual(m.bWidth, 33.0f, 0.8f));    // [B] tile: 33 px wide
    CHECK(m.bWidth < m.size);                     // ... and therefore narrower
    CHECK(nearlyEqual(m.ringWidth, 3.5f, 0.3f));   // [A] dark ring ...
    CHECK(nearlyEqual(m.bRingWidth, 4.5f, 0.3f));  // ... [B] frame is thicker
    CHECK(nearlyEqual(m.letterA, 21.0f, 0.8f));   // letter caps: 21 px (A) ...
    CHECK(nearlyEqual(m.letterB, 19.0f, 0.8f));   // ... 19 px (B)
    CHECK(nearlyEqual(m.wordGap, 14.4f, 1.2f));   // gaps of the original line
    CHECK(nearlyEqual(m.dropY, 1.2f, 0.3f));      // icon centre sits that low

    // Same proportions at double the cap height (1080p+ / any resolution).
    const compat::ui::ButtonIconMetrics hd = compat::ui::buttonIconMetrics(48.0f);
    CHECK(nearlyEqual(hd.size, m.size * 2.0f, 0.01f));
    CHECK(nearlyEqual(hd.bWidth, m.bWidth * 2.0f, 0.01f));
    CHECK(nearlyEqual(hd.letterB, m.letterB * 2.0f, 0.01f));
    CHECK(nearlyEqual(hd.faceSize, m.faceSize * 2.0f, 0.01f));
}

TEST_CASE(prompt_message_is_split_into_words_and_button_slots) {
    compat::ui::PromptItem items[compat::ui::kMaxPromptItems];

    // The message our LayoutManager fallback installs for PressStart.
    const wchar_t longForm[] = L"Press both [A] and [B].";
    int count = compat::ui::splitPromptMessage(longForm, 23, items, compat::ui::kMaxPromptItems);
    REQUIRE(count == 6);
    CHECK(items[0].kind == compat::ui::PromptItem::Text);
    CHECK_EQ(items[0].len, 5);  // "Press"
    CHECK(items[1].kind == compat::ui::PromptItem::Text);
    CHECK_EQ(items[1].len, 4);  // "both"
    CHECK(items[2].kind == compat::ui::PromptItem::ButtonA);
    CHECK(items[3].kind == compat::ui::PromptItem::Text);
    CHECK_EQ(items[3].len, 3);  // "and"
    CHECK(items[4].kind == compat::ui::PromptItem::ButtonB);
    CHECK(items[5].kind == compat::ui::PromptItem::Text);
    CHECK_EQ(items[5].len, 1);  // "."

    // The short form (no "both") works too, and the trailing blanks around a
    // token are gaps, never part of a word.
    const wchar_t shortForm[] = L"Press [A] and [B].";
    count = compat::ui::splitPromptMessage(shortForm, 18, items, compat::ui::kMaxPromptItems);
    REQUIRE(count == 5);
    CHECK_EQ(items[0].len, 5);
    CHECK(items[1].kind == compat::ui::PromptItem::ButtonA);
    CHECK_EQ(items[2].len, 3);
    CHECK(items[3].kind == compat::ui::PromptItem::ButtonB);
    CHECK_EQ(items[4].len, 1);

    // No button slot -> handled by the plain text path.
    const wchar_t plain[] = L"Hello world";
    CHECK_EQ(compat::ui::splitPromptMessage(plain, 11, items, compat::ui::kMaxPromptItems), 0);

    // Degenerate inputs must not loop or dereference.
    CHECK_EQ(compat::ui::splitPromptMessage(nullptr, 5, items, compat::ui::kMaxPromptItems), 0);
    CHECK_EQ(compat::ui::splitPromptMessage(plain, 0, items, compat::ui::kMaxPromptItems), 0);
}

// -----------------------------------------------------------------------------
// PC_PORT (title widescreen): the mapping has to be fed FRAMEBUFFER PIXELS.
//
// Every host call site used to pass MR::getScreenHeight(), which is the 456-unit
// DESIGN height (Game/Util/ScreenUtil.cpp). That is right on the console — its
// EFB really is 456 rows high — but wrong on a PC window: at 1920x1080 it gave
// uiScale 1.0 (instead of 2.37) and a 456-row viewport, i.e. the whole UI at 1:1
// squeezed into the top of the frame. compat::ui::framebufferSize() is the
// single accessor for the real geometry (the render mode the EFB is created
// with), and these checks pin it.
// -----------------------------------------------------------------------------
TEST_CASE(ui_mapping_is_fed_framebuffer_pixels_not_the_design_height) {
    // The render mode object the compat VI layer owns is what the EFB is created
    // with, so the test drives it directly. (Platform::CompatVi::
    // setHostFramebufferSize() also mirrors the geometry into whatever object
    // VIConfigure cached — in the boot that is this same mutable object, and the
    // suite's synthetic mode tables are const, so they are not written here.)
    GXRenderModeObj* hostMode = Platform::CompatVi::hostRenderMode();
    REQUIRE(hostMode != nullptr);
    const u16 savedW = hostMode->fbWidth;
    const u16 savedH = hostMode->efbHeight;

    f32 w = 0.0f;
    f32 h = 0.0f;
    compat::ui::framebufferSize(&w, &h);

    // Headless (before the window exists): the console geometry the boot starts
    // from — and only there does the design height equal the pixel height.
    CHECK(nearlyEqual(h, 456.0f));
    if (nearlyEqual(w, 640.0f) && nearlyEqual(h, 456.0f)) {
        CHECK(nearlyEqual(compat::ui::uiScale(w, h), 1.0f));  // 456 units == 456 rows
    }

    // A 1080p window: MainLoopFramework::beginRender re-syncs the render mode to
    // the window before the first draw, and the UI mapping has to follow it.
    hostMode->fbWidth = 1920;
    hostMode->efbHeight = 1080;
    hostMode->xfbHeight = 1080;
    compat::ui::framebufferSize(&w, &h);
    CHECK(nearlyEqual(w, 1920.0f));
    CHECK(nearlyEqual(h, 1080.0f));
    CHECK(nearlyEqual(compat::ui::uiScale(w, h), 1080.0f / kDesignHeight, 0.001f));
    // The bug, stated as a check: the design height is NOT the pixel height.
    CHECK(nearlyEqual(compat::ui::uiScale(w, kDesignHeight), 1.0f));
    CHECK(compat::ui::uiScale(w, h) > 2.3f);

    // 16:9 takes the game's 832-unit layout space...
    CHECK(nearlyEqual(compat::ui::layoutSpaceWidth(w, h), kDesignWidth16x9));

    // ...and the composition is centred on the framebuffer: the origin maps to
    // the screen centre, and a pane offset of -22.5 layout units (the logo's)
    // moves by -22.5 * scale — never by a percentage of the width.
    f32 sx = 0.0f;
    f32 sy = 0.0f;
    compat::ui::layoutToScreen(0.0f, 0.0f, w, h, &sx, &sy);
    CHECK(nearlyEqual(sx, 960.0f));
    CHECK(nearlyEqual(sy, 540.0f));
    compat::ui::layoutToScreen(0.0f, -22.5f, w, h, &sx, &sy);
    CHECK(nearlyEqual(sx, 960.0f));
    CHECK(nearlyEqual(sy, 540.0f + 22.5f * (1080.0f / kDesignHeight)));

    // The view matrix keeps the anchor (no offset terms) at this size, and its
    // Y scale is 1.0: the ortho already spans the 456 design units over the
    // framebuffer height, so the view matrix must NOT scale Y again (doing so
    // was the 2.37x vertical stretch this test now pins down).
    f32 mtx[3][4];
    compat::ui::fillUiViewMtx(w, h, mtx);
    CHECK(nearlyEqual(mtx[0][3], 0.0f));
    CHECK(nearlyEqual(mtx[1][3], 0.0f));
    CHECK_NEAR(mtx[1][1], 1.0f, 0.001f);

    // Restore the boot default (the rest of the suite expects the console mode).
    hostMode->fbWidth = savedW;
    hostMode->efbHeight = savedH;
    hostMode->xfbHeight = savedH;
}

// -----------------------------------------------------------------------------
// The layout pass, end to end: a layout unit must land as the SAME number of
// pixels in X and in Y ("one composition", no vertical stretch), and that number
// must be uiScale for the framebuffer.
//
// This is the property the title's logo/prompt/copyright composition lives or
// dies on, and it is decided by two things working together:
//   * MR::setupDrawForNW4RLayout's ortho, which spans 608 DESIGN units in x and
//     456 in y over the whole framebuffer,
//   * the DrawInfo view matrix from fillUiViewMtx.
// Feeding the design height in as if it were the pixel height (and normalising
// y by it) is what produced the 2.37x vertical stretch at 1080p.
// -----------------------------------------------------------------------------
TEST_CASE(layout_pass_maps_layout_units_uniformly_to_pixels) {
    struct Resolution {
        f32 w, h;
    };
    const Resolution resolutions[] = {
        {1280.0f, 720.0f},  {1920.0f, 1080.0f}, {2560.0f, 1080.0f},
        {3440.0f, 1440.0f}, {3840.0f, 2160.0f}, {640.0f, 456.0f},
        {640.0f, 480.0f},   {800.0f, 1080.0f},  // narrower than 4:3
    };

    for (const Resolution& r : resolutions) {
        // The ortho setupDrawForNW4RLayout sets: ±(456/2) tall, ±(608/2) wide.
        const f32 eyeHalfX = kDesignWidth4x3 * 0.5f;
        const f32 eyeHalfY = kDesignHeight * 0.5f;
        const f32 pixelsPerEyeX = r.w / (2.0f * eyeHalfX);
        const f32 pixelsPerEyeY = r.h / (2.0f * eyeHalfY);

        f32 mtx[3][4];
        compat::ui::fillUiViewMtx(r.w, r.h, mtx);

        const f32 sxPerUnit = mtx[0][0];
        const f32 syPerUnit = mtx[1][1];
        const f32 pixelsPerUnitX = sxPerUnit * pixelsPerEyeX;
        const f32 pixelsPerUnitY = syPerUnit * pixelsPerEyeY;
        const f32 scale = compat::ui::uiScale(r.w, r.h);

        // Uniform: a square layout area is a square on screen, at every aspect.
        CHECK_NEAR(pixelsPerUnitX, pixelsPerUnitY, 0.001f * scale);
        // 456 layout units span the framebuffer height (the console rule).
        CHECK_NEAR(pixelsPerUnitY, scale, 0.001f * scale);
        // Anchored on the screen centre: no translation terms.
        CHECK(nearlyEqual(mtx[0][3], 0.0f));
        CHECK(nearlyEqual(mtx[1][3], 0.0f));
        CHECK(nearlyEqual(mtx[2][2], 1.0f));

        // Sanity: the composition keeps its proportions relative to the frame.
        // (At 16:9 the logo's -22.5 layout-unit offset is 22.5 * scale pixels
        // above the centre, never a fraction of the width.)
        f32 px = 0.0f;
        f32 py = 0.0f;
        compat::ui::layoutToScreen(0.0f, -22.5f, r.w, r.h, &px, &py);
        CHECK_NEAR(py, r.h * 0.5f + 22.5f * scale, 0.01f);
    }
}

// PC_PORT (title widescreen): the [A]/[B] icons are drawn with the ORIGINAL
// picture-font glyphs when the font is mounted (compat/ui/PictureGlyphs.h). The
// ink boxes those quads are built from separate the icon from the soft shadow
// the glyphs carry — that is what keeps the icon the size the reference
// measures and lets the shadow hang below it.
TEST_CASE(picture_glyph_ink_box_separates_the_icon_from_its_shadow) {
    // 12x12 RGBA: the "icon" is an opaque 4x4 block at (3,3) and below it sit
    // two low-alpha shadow rows. A bright decoy sits outside the queried cell.
    //
    // The boxes come back in IMAGE coordinates (origin included). That is the
    // whole point: the u/v of the icon quad are computed straight from them, so
    // a cell-relative box made the quad sample the sheet's top-left corner —
    // glyph 0, the blank space — and the icons came out invisible.
    std::vector<u8> px(12 * 12 * 4, 0);
    const auto set = [&px](int x, int y, u8 v, u8 a) {
        u8* p = &px[(static_cast<size_t>(y) * 12 + x) * 4];
        p[0] = p[1] = p[2] = v;
        p[3] = a;
    };
    for (int y = 3; y < 7; ++y) {
        for (int x = 3; x < 7; ++x) {
            set(x, y, 250, 255);
        }
    }
    for (int x = 3; x < 7; ++x) {
        set(x, 7, 40, 60);  // shadow, row 1
        set(x, 8, 40, 20);  // shadow, row 2
    }
    set(11, 0, 255, 255);   // decoy, outside the cell

    const compat::ui::InkBox box = compat::ui::inkBoxOfRgba8(px.data(), 12, 12, 1, 1, 8, 9);

    CHECK(box.any);
    // The icon: only the opaque block, in IMAGE coordinates (origin (1,1)).
    CHECK(box.solidX == 3);
    CHECK(box.solidY == 3);
    CHECK(box.solidW == 4);
    CHECK(box.solidH == 4);
    // The full ink: the icon plus its two shadow rows, no wider than the icon.
    CHECK(box.outerX == 3);
    CHECK(box.outerY == 3);
    CHECK(box.outerW == 4);
    CHECK(box.outerH == 6);
}

TEST_CASE(picture_glyph_ink_box_falls_back_to_luminance_without_alpha) {
    // An opaque sheet (alpha zero everywhere) has to be measured by luminance,
    // which is how some picture fonts store the art.
    std::vector<u8> px(8 * 8 * 4, 0);
    for (int y = 2; y < 5; ++y) {
        for (int x = 2; x < 5; ++x) {
            u8* p = &px[(static_cast<size_t>(y) * 8 + x) * 4];
            p[0] = p[1] = p[2] = 200;
        }
    }

    const compat::ui::InkBox box = compat::ui::inkBoxOfRgba8(px.data(), 8, 8, 0, 0, 8, 8);
    CHECK(box.any);
    CHECK(box.solidX == 2);
    CHECK(box.solidY == 2);
    CHECK(box.solidW == 3);
    CHECK(box.solidH == 3);
    CHECK(box.outerW == 3);
    CHECK(box.outerH == 3);
}

TEST_CASE(picture_glyph_ink_box_rejects_out_of_range_cells) {
    std::vector<u8> px(8 * 8 * 4, 255);
    CHECK(!compat::ui::inkBoxOfRgba8(px.data(), 8, 8, 6, 6, 4, 4).any);
    CHECK(!compat::ui::inkBoxOfRgba8(nullptr, 8, 8, 0, 0, 8, 8).any);
}

TEST_CASE(picture_glyph_requires_an_installed_font) {
    // Without Font.arc there is no glyph source, so ButtonPrompt has to fall
    // back to the vector reproduction (what the sandbox and the tests run).
    compat::ui::PictureGlyph glyph;
    compat::ui::clearPictureFont();
    CHECK(!compat::ui::pictureGlyph(compat::ui::kPictureCodeA, glyph));
    CHECK(!glyph.valid);
    CHECK(glyph.sheetImage == nullptr);
    CHECK(glyph.solidH == 0.0f);

    compat::ui::setPictureFont(nullptr);
    CHECK(!compat::ui::pictureGlyph(compat::ui::kPictureCodeB, glyph));
}

// PC_PORT (title widescreen): a pane that covers the whole design area is a
// screen-covering EFFECT (the title intro's flash, TitleLogo's PicFlash) and
// has to cover the whole framebuffer — on a widescreen frame it was only
// covering the 4:3 middle, which left the sides unflashed and showed a hard
// vertical cut at the design-area edges.
TEST_CASE(screen_covering_panes_reach_the_full_framebuffer_width) {
    // The flash pane: authored over the whole 4:3 design area.
    const f32 flashW = compat::ui::kDesignWidth4x3;
    const f32 flashH = compat::ui::kDesignHeight;

    // 1280x720: at the uniform scale the frame shows 810.7 units of layout
    // space (456 * 16/9), so the 608-unit design area has to grow by 810.7/608.
    const f32 k720 = compat::ui::screenCoveringPaneScaleXForFramebuffer(flashW, flashH, 1280.0f, 720.0f);
    CHECK_NEAR(k720, compat::ui::visibleLayoutWidth(1280.0f, 720.0f) / 608.0f, 0.001f);
    CHECK_NEAR(k720, 1.333f, 0.01f);

    // The widened pane covers the framebuffer exactly.
    CHECK_NEAR(flashW * k720, compat::ui::visibleLayoutWidth(1280.0f, 720.0f), 0.01f);

    // Bigger/other 16:9 sizes: the same layout-space coverage every time.
    for (const auto& r : {std::pair<f32, f32>{1920.0f, 1080.0f}, {3840.0f, 2160.0f}}) {
        const f32 k = compat::ui::screenCoveringPaneScaleXForFramebuffer(flashW, flashH, r.first, r.second);
        CHECK_NEAR(k, 1.333f, 0.01f);
    }

    // Ultrawide 2560x1080 / 3440x1440: MORE than the 832-unit layout space is
    // visible, and the flash has to cover that too (no seams on the sides).
    for (const auto& r : {std::pair<f32, f32>{2560.0f, 1080.0f}, {3440.0f, 1440.0f}}) {
        const f32 k = compat::ui::screenCoveringPaneScaleXForFramebuffer(flashW, flashH, r.first, r.second);
        CHECK(k > 1.333f);
        CHECK_NEAR(flashW * k, compat::ui::visibleLayoutWidth(r.first, r.second), 0.01f);
    }

    // 4:3 (and narrower): nothing to extend.
    CHECK_NEAR(compat::ui::screenCoveringPaneScaleXForFramebuffer(flashW, flashH, 960.0f, 720.0f), 1.0f, 0.001f);

    // Ordinary panes are never touched: the logo, a button, a text box.
    CHECK_NEAR(compat::ui::screenCoveringPaneScaleXForFramebuffer(440.0f, 256.0f, 1920.0f, 1080.0f), 1.0f, 0.001f);
    CHECK_NEAR(compat::ui::screenCoveringPaneScaleXForFramebuffer(288.0f, 176.0f, 3440.0f, 1440.0f), 1.0f, 0.001f);
    // Covering only ONE axis is not a screen-covering pane either.
    CHECK_NEAR(compat::ui::screenCoveringPaneScaleXForFramebuffer(608.0f, 100.0f, 1920.0f, 1080.0f), 1.0f, 0.001f);
    CHECK_NEAR(compat::ui::screenCoveringPaneScaleXForFramebuffer(100.0f, 456.0f, 1920.0f, 1080.0f), 1.0f, 0.001f);
}
