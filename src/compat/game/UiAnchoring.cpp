// =============================================================================
// compat/game — UiAnchoring (see UiAnchoring.h).
// =============================================================================

#include "compat/game/UiAnchoring.h"

#include "compat/vi/VICompat.h"

#include <cstring>

namespace compat::ui {

void framebufferSize(f32* outPixelWidth, f32* outPixelHeight) {
    // Platform::CompatVi owns the render mode the boot presents with; it starts
    // at the console geometry and MainLoopFramework::beginRender re-syncs it to
    // the window (setHostFramebufferSize) before anything is drawn.
    const GXRenderModeObj* rmode = Platform::CompatVi::hostRenderMode();
    f32 width = (rmode != nullptr) ? static_cast<f32>(rmode->fbWidth) : 0.0f;
    f32 height = (rmode != nullptr) ? static_cast<f32>(rmode->efbHeight) : 0.0f;
    if (!(width > 0.0f) || !(height > 0.0f)) {
        width = kDesignWidth4x3;
        height = kDesignHeight;
    }
    if (outPixelWidth != nullptr) {
        *outPixelWidth = width;
    }
    if (outPixelHeight != nullptr) {
        *outPixelHeight = height;
    }
}

f32 aspectRatio(f32 pixelWidth, f32 pixelHeight) {
    if (!(pixelHeight > 0.0f) || !(pixelWidth > 0.0f)) {
        return 0.0f;
    }
    return pixelWidth / pixelHeight;
}

bool usesWideLayoutSpace(f32 pixelWidth, f32 pixelHeight) {
    // The console asks the VI whether the current render mode is 16:9. On the
    // host the equivalent question is whether the framebuffer is wide enough to
    // hold the 832-unit layout space at the uniform (456 = height) scale: a
    // 4:3 framebuffer is 1.333, 16:9 is 1.778, both below the 1.8246 the 16:9
    // space needs — the Wii squeezed its 832x456 EFB into 16:9. Use the console
    // threshold (anything wider than 4:3 is "wide"), so 16:9 windows get the
    // game's 16:9 layout space exactly like the console does.
    return aspectRatio(pixelWidth, pixelHeight) > (kDesignWidth4x3 / kDesignHeight);
}

f32 layoutSpaceWidth(f32 pixelWidth, f32 pixelHeight) {
    return usesWideLayoutSpace(pixelWidth, pixelHeight) ? kDesignWidth16x9 : kDesignWidth4x3;
}

f32 uiScale(f32 pixelWidth, f32 pixelHeight) {
    if (!(pixelHeight > 0.0f) || !(pixelWidth > 0.0f)) {
        return 1.0f;
    }

    // 456 layout units span the height...
    f32 scale = pixelHeight / kDesignHeight;

    // ...unless the framebuffer is narrower than 4:3, in which case the 608-unit
    // design space would not fit horizontally. Shrink the whole composition
    // instead of stretching it (keeps the aspect of every pane).
    const f32 fitWidth = pixelWidth * kDesignHeight / kDesignWidth4x3;
    if (fitWidth < pixelHeight) {
        scale = pixelWidth / kDesignWidth4x3;
    }

    return scale;
}

f32 visibleLayoutWidth(f32 pixelWidth, f32 pixelHeight) {
    const f32 scale = uiScale(pixelWidth, pixelHeight);
    return (scale > 0.0f) ? pixelWidth / scale : kDesignWidth4x3;
}

f32 screenCoveringPaneScaleXForFramebuffer(f32 paneWidth, f32 paneHeight, f32 pixelWidth,
                                           f32 pixelHeight) {
    // "Covers the design area" with a texel-level tolerance: the panes are
    // authored in whole design units and the animations may scale them a hair
    // below 608/456 at the extremes of their fade.
    constexpr f32 kEpsilon = 1.0f;

    if (paneWidth < kDesignWidth4x3 - kEpsilon || paneHeight < kDesignHeight - kEpsilon) {
        return 1.0f;
    }

    const f32 visible = visibleLayoutWidth(pixelWidth, pixelHeight);

    if (!(visible > paneWidth)) {
        return 1.0f;
    }

    return visible / paneWidth;
}

f32 screenCoveringPaneScaleX(f32 paneWidth, f32 paneHeight) {
    f32 pixelWidth = 0.0f;
    f32 pixelHeight = 0.0f;
    framebufferSize(&pixelWidth, &pixelHeight);
    return screenCoveringPaneScaleXForFramebuffer(paneWidth, paneHeight, pixelWidth, pixelHeight);
}

void layoutToScreen(f32 layoutX, f32 layoutY, f32 pixelWidth, f32 pixelHeight, f32* outPixelX,
                    f32* outPixelY) {
    const f32 scale = uiScale(pixelWidth, pixelHeight);

    if (outPixelX != nullptr) {
        *outPixelX = pixelWidth * 0.5f + layoutX * scale;
    }
    if (outPixelY != nullptr) {
        *outPixelY = pixelHeight * 0.5f - layoutY * scale;
    }
}

void fillUiViewMtx(f32 pixelWidth, f32 pixelHeight, f32 outMtx[3][4]) {
    const f32 scale = uiScale(pixelWidth, pixelHeight);

    // Eye space (the space the layout ortho expects): the ortho set by
    // MR::setupDrawForNW4RLayout spans x in [-304, 304] and y in [-H/2, H/2],
    // both mapped onto the whole framebuffer. The view matrix therefore has to
    // convert layout units into eye units:
    //   eyeX = layoutX * scale * (608 / W)     (608 eye units == W pixels)
    //   eyeY = layoutY * scale                 (H eye units == H pixels)
    // Pane::LoadMtx applies ReverseYAxis() because the DrawInfo view rect is
    // Y-up, which is what turns the layout space's +Y into screen-up — so the
    // sign convention of the existing (identity) view matrix is kept, only the
    // magnitudes change.
    // BOTH axes are normalised the same way, and that is the whole trick: the
    // ortho above spans 608 eye units over the framebuffer WIDTH and 456 eye
    // units (the design height) over its HEIGHT, so one eye unit is not one
    // pixel in either axis:
    //     px per eye unit X = W / 608      px per eye unit Y = H / 456
    //   => px per layout unit X = eyeScaleX * W / 608
    //      px per layout unit Y = eyeScaleY * H / 456
    // Both must equal `scale` (a layout unit is a layout unit), which gives the
    // normalisation below. Note eyeScaleY == 1 exactly when 456 units span the
    // height — the console rule, and what 1080p/1440p/2160p resolve to; it only
    // drops below 1 on framebuffers narrower than 4:3, where the composition is
    // shrunk to fit instead of overflowing sideways.
    //
    // PC_PORT bug fixed here: eyeScaleY used to be `scale`, i.e. the Y axis was
    // normalised by the DESIGN height instead of the framebuffer height. That
    // was invisible while the caller also passed the design height (scale 1.0,
    // both axes 1:1), and turned into a `scale`x vertical stretch — 2.37x at
    // 1080p — as soon as the mapping was fed the real pixel height.
    const f32 eyeScaleX = (pixelWidth > 0.0f) ? scale * kDesignWidth4x3 / pixelWidth : scale;
    const f32 eyeScaleY = (pixelHeight > 0.0f) ? scale * kDesignHeight / pixelHeight : scale;

    std::memset(outMtx, 0, sizeof(f32) * 12);
    outMtx[0][0] = eyeScaleX;  // layout X -> eye X
    outMtx[1][1] = eyeScaleY;  // layout Y -> eye Y
    outMtx[2][2] = 1.0f;
    outMtx[0][3] = 0.0f;  // anchored on the screen centre: no offset
    outMtx[1][3] = 0.0f;
    outMtx[2][3] = 0.0f;
}

PromptBlock layoutPromptBlock(f32 centerX, f32 baselineY, const f32 wordWidths[3], f32 buttonSize,
                              f32 periodWidth, f32 wordGap, f32 periodGap) {
    PromptBlock block{};
    block.baselineY = baselineY;

    const f32 width[static_cast<int>(PromptSlot::Count)] = {
        wordWidths[0],              // Press
        wordWidths[1],              // both
        buttonSize,                 // [A]
        wordWidths[2],              // and
        buttonSize,                 // [B]
        periodWidth,                // .
    };

    f32 cursor = 0.0f;
    for (int i = 0; i < kPromptSlotCount; ++i) {
        block.x[i] = cursor;

        f32 advance = width[i];
        if (i + 1 < kPromptSlotCount) {
            advance += (i + 1 == static_cast<int>(PromptSlot::Period)) ? periodGap : wordGap;
        }
        cursor += advance;
    }

    block.totalWidth = cursor;
    block.startX = centerX - cursor * 0.5f;
    block.endX = block.startX + cursor;

    for (int i = 0; i < kPromptSlotCount; ++i) {
        block.x[i] += block.startX;
    }

    return block;
}

}  // namespace compat::ui
