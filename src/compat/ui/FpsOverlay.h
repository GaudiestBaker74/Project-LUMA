#pragma once
// =============================================================================
// compat/ui — FpsOverlay (--show-fps): a "NN FPS" counter drawn at the top
// right of the frame, on top of whatever the game rendered.
//
// The glyphs come from the game's own message font (Font.arc, the same brfnt
// the layout text boxes use) — no host-authored art is added. Call
// drawFpsOverlayIfEnabled() from INSIDE an open EFB render pass, after the
// scene has drawn (MainLoopFramework::endRender on the --boot path, the demo
// loop in main.cpp otherwise); it is a no-op unless --show-fps was given.
// =============================================================================

namespace compat::ui {

/// --show-fps wiring (main.cpp). Logs once when turned on.
void setFpsOverlayEnabled(bool enabled);

bool fpsOverlayEnabled();

/// Draws "NN FPS" right-aligned at the top-right corner of the current
/// framebuffer. Needs the message font (Font.arc) mounted; without it the
/// overlay stays off (warned once). Measures presented frames per 0.5 s.
void drawFpsOverlayIfEnabled();

} // namespace compat::ui
