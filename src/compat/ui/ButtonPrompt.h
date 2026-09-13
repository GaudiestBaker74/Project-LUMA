#pragma once
// =============================================================================
// compat/ui — ButtonPrompt (PC_PORT, title-screen widescreen work).
//
// "Press both [A] and [B]." with REAL button icons.
//
// -----------------------------------------------------------------------------
// Why this exists (root cause of the "white blobs")
// -----------------------------------------------------------------------------
//
// On the console the instruction line is a message (Layout_PressStartTxtStart)
// whose A/B symbols are **picture-font glyphs**: the text carries the 0x1A tag,
// the tag processor swaps in a glyph from the picture-font groups
// (/MessageData/... packed in the message archive). The host
// (compat/game/LayoutManagerCompat.cpp) has no message system and no picture
// font, so the fallback message wrote the symbols as *words* and — worse — the
// placeholder glyphs the layout's font does provide for that slot rendered as
// plain filled circles/dots. That is the "manchas/círculos blancos": no texture
// atlas is involved at all, the symbols simply never had a glyph.
//
// This module draws the whole line itself, through the same GX path the layout
// engine uses (so it inherits the pane matrix, drawing order and alpha):
//
//   * the six slots of the sentence are measured and the WHOLE block is centred
//     once (compat::ui::layoutPromptBlock) — individual words are never
//     centred on their own;
//   * the [A]/[B] slots are drawn with the ORIGINAL art: the picture-font
//     glyphs of /LayoutData/Font.arc -> /PictureFont.brfnt (codes 0x0030 and
//     0x0031), resolved through compat/ui/PictureGlyphs.h. Without that font
//     they fall back to vector icons that reproduce those glyphs — the disc
//     with its dark ring and the portrait tile with its frame, with sizes,
//     ring thickness, letter grey, shadow and offsets measured off the
//     reference title screen in cap-height units, so the icons scale with the
//     line at every resolution;
//   * the words keep the layout's own font, colours and gradient.
//
// Vertical placement, slot order and gaps follow the reference title screen.
// =============================================================================

#include <revolution/types.h>

// WideTextWriter is a typedef of TextWriterBase<wchar_t>, not a class: include
// the real header instead of forward-declaring it.
#include <nw4r/ut/WideTextWriter.h>

namespace compat::ui {

/// Everything the layout's TextBox knows about itself when it draws.
struct PromptDrawContext {
    nw4r::ut::WideTextWriter* writer;
    const wchar_t* text;
    u16 textLen;

    /// Origin of the text draw rect (layout units, +Y is up on screen).
    f32 textLeft;
    f32 textTop;

    /// Cap height of the line in layout units (font ascent at the used size).
    f32 capHeight;

    /// Layout-space x the block is centred on (0 = the screen centre: the
    /// layout space is centred on the origin by construction).
    f32 blockCenterX;

    GXColor colorTop;
    GXColor colorBottom;

    /// The writer's colour mapping (the material's TEV colours 0/1, see
    /// nw4r::ut::CharWriter::SetupGXWithColorMapping): every printed texel is
    /// mix(min, max, texel) BEFORE the vertex colour multiplies it. On the
    /// title's shadow pane (ShaStart) the dark ink can live in the mapping's
    /// max instead of the vertex colour, so the icons fold `mapMax` into their
    /// tint (running the lerp as a TEV stage over the texel washes the art's
    /// own colours to white on alpha-only mappings). Defaults are identity.
    GXColor mapMin = GXColor{0, 0, 0, 0};
    GXColor mapMax = GXColor{255, 255, 255, 255};

    /// Pane/global alpha (0-255) applied to every emitted vertex.
    u8 globalAlpha = 255;
};

// -----------------------------------------------------------------------------
// Message tokenization (pure, unit-testable): the line is split into text runs
// and A/B button slots. The `[A]`/`[B]` tokens are where the console's
// picture-font glyphs live in our fallback message.
// -----------------------------------------------------------------------------
constexpr int kMaxPromptItems = 12;

struct PromptItem {
    enum Kind { Text, ButtonA, ButtonB } kind;
    const wchar_t* text;  // Text runs only
    int len;              // Text runs only
    f32 width;            // filled in by the measuring pass
};

/// Returns the number of items (>= 1) or 0 when the message has no button slot
/// (the caller then falls back to the plain text path).
int splitPromptMessage(const wchar_t* text, int len, PromptItem* items, int maxItems);

/// Icon metrics, sized from the cap height (exposed for tests).
///
/// Every field is a measurement off the ORIGINAL title screen (the reference
/// frame is 1280x720 and its cap height is 24 px). The [A] disc and the [B]
/// tile are the same HEIGHT and the tile is narrower, which is what the
/// original shows — nothing here is a re-design:
///
///   [A] outer disc 39 px (1.62 cap), face 32 px (1.33 cap), ring ~3.5 px
///   [B] outer tile 33 x 39 px (0.85 x height), face 24 x 30 px, frame ~4.5 px
///   letters mid grey, 21 px (A) / 19 px (B); icon centre 1.2 px below text
struct ButtonIconMetrics {
    f32 capHeight;   // effective cap height of the line (layout units)
    f32 size;        // [A] outer diameter == [B] outer height
    f32 faceSize;    // [A] face (the white disc) == [B] face height: 0.82 * size
    f32 bWidth;      // [B] outer width (portrait tile)
    f32 ringWidth;   // [A] dark ring thickness (3.5 px)
    f32 bRingWidth;  // [B] dark frame thickness (4.5 px)
    f32 wordGap;     // gap around the icons and between words
    f32 periodGap;   // gap before the final period
    f32 cornerRatio; // [B] corner radius / bWidth
    f32 letterA;     // [A] letter target height
    f32 letterB;     // [B] letter target height
    f32 dropY;       // icon centre below the text centre, / cap height
};

ButtonIconMetrics buttonIconMetrics(f32 capHeight);

/// Draws the whole instruction line when `text` is a "Press both [A] and [B]."
/// style message (tokens `[A]`/`[B]`, also accepted: the short "Press [A] and
/// [B]." form). Returns false when the text is not such a message, in which
/// case the caller falls back to the normal TextBox print.
bool drawButtonPrompt(const PromptDrawContext& ctx);

}  // namespace compat::ui
