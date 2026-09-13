// =============================================================================
// Host presentation policy tests. These stay headless: the pure helpers are
// the contract used by the live ScreenUtil snapshot.
// =============================================================================

#include "tests/test_runner.h"

#include "compat/ScreenMetrics.h"

#include <cmath>

TEST_CASE(screen_metrics_logical_width_stays_wii_compatible) {
    using Platform::CompatScreen::logicalWidthForAspect;

    // Resizing changes only the presentation rectangle. The BRLYT coordinate
    // space remains the Wii 608 x 456 space, otherwise the final EFB blit
    // stretches the title logo on wide and portrait windows.
    CHECK_EQ(logicalWidthForAspect(4.0f / 3.0f), 608u);
    CHECK_EQ(logicalWidthForAspect(16.0f / 9.0f), 608u);
    CHECK_EQ(logicalWidthForAspect(21.0f / 9.0f), 608u);
    CHECK_EQ(logicalWidthForAspect(9.0f / 16.0f), 608u);
}

TEST_CASE(screen_metrics_widescreen_mode_uses_live_aspect) {
    using Platform::CompatScreen::isWidescreenAspect;

    CHECK(!isWidescreenAspect(4.0f / 3.0f));
    CHECK(isWidescreenAspect(16.0f / 9.0f));
    CHECK(isWidescreenAspect(32.0f / 9.0f));
    CHECK(!isWidescreenAspect(9.0f / 16.0f));

    // Minimized/invalid extents fall back to the 4:3 Wii coordinate policy.
    CHECK_EQ(Platform::CompatScreen::logicalWidthForAspect(0.0f), 608u);
    CHECK(!isWidescreenAspect(0.0f));
    CHECK(std::isfinite(static_cast<float>(Platform::CompatScreen::logicalWidthForAspect(-1.0f))));
}
