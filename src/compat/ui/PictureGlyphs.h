#pragma once
// =============================================================================
// compat/ui — PictureGlyphs (PC_PORT, title-screen work).
//
// The [A]/[B] icons of "Press both [A] and [B]." are NOT vector art. On the
// console the message carries the 0x1A tag and the tag processor swaps in a
// picture-font glyph that lives in /LayoutData/Font.arc -> /PictureFont.brfnt.
// The port already mounts that font (GameSystemFontHolder::createFontFromFile),
// so the very same texels the console draws are reachable: this module resolves
// a character code against the font and hands the renderer what it needs.
//
// What the shipped font looks like (measured with LUMA_PICFONT_DUMP on the
// user's own asset dump — PictureFontDump.h):
//
//   * one sheet, 128x128 RGB5A3, TGLP cell 34x35, grid 3x3 (nine glyphs);
//   * the ASCII range maps the icons, so the instruction line uses
//     0x0030 = [A] (the disc, 28x28 texels of ink) and 0x0031 = [B] (the
//     portrait tile, 22x28) — the codes sit right where the reference title
//     screen has them;
//   * the CELL is bigger than the art and the art is not square: the icon has
//     to be placed and scaled by its INK box, not by the cell. `solid` is the
//     icon itself (opaque texels) and `outer` adds the antialiased edge; the
//     glyphs carry NO soft drop shadow of their own.
//
// ButtonPrompt draws the `outer` box at ONE texel scale shared by both icons
// (the console draws every glyph of a font at the same scale), bottom-aligned,
// which is the arrangement the reference shows.
//
// With no font installed (assets without LayoutData/, unit tests) everything
// reports invalid and ButtonPrompt falls back to its vector reproduction —
// which is what it drew before this module existed.
// =============================================================================

#include <revolution/types.h>

namespace nw4r {
    namespace ut {
        class Font;
    }
}

namespace compat::ui {

/// Picture-font codes of the instruction line's two icons.
constexpr u16 kPictureCodeA = 0x0030;
constexpr u16 kPictureCodeB = 0x0031;

/// One glyph resolved against the font's texture sheet.
struct PictureGlyph {
    bool valid = false;

    const void* sheetImage = nullptr;  // the sheet's texels, in `sheetFormat`
    u16 sheetWidth = 0;
    u16 sheetHeight = 0;
    u16 sheetFormat = 0;               // GXTexFmt, as stored in the brfnt

    /// Ink boxes, normalised over the sheet (u to the right, v downwards).
    f32 solidU0 = 0.0f, solidV0 = 0.0f, solidU1 = 0.0f, solidV1 = 0.0f;
    f32 outerU0 = 0.0f, outerV0 = 0.0f, outerU1 = 0.0f, outerV1 = 0.0f;

    /// The same two boxes in sheet texels.
    f32 solidW = 0.0f, solidH = 0.0f;
    f32 outerW = 0.0f, outerH = 0.0f;
};

/// Installs the picture font (the ResFont over /PictureFont.brfnt). Passing
/// nullptr — or clearPictureFont() — turns the real glyphs off.
void setPictureFont(const nw4r::ut::Font* font);
void clearPictureFont();

/// Resolves `code`. False when no font is installed, the font has no glyph for
/// the code, or the sheet cannot be decoded.
bool pictureGlyph(u16 code, PictureGlyph& out);

/// Tight ink box of a sub-rectangle of a 32-bit RGBA image, in IMAGE
/// coordinates (the (x, y) origin passed in is included in the result — the
/// caller computes u/v from these straight against the sheet). Pure and
/// unit-tested without any font: a texel counts as `outer` ink when it is not
/// fully transparent and as `solid` ink when its alpha is opaque enough to be
/// the icon rather than a soft edge. When the image carries no alpha at all
/// (an opaque sheet) the test falls back to luminance.
struct InkBox {
    bool any = false;
    int solidX = 0, solidY = 0, solidW = 0, solidH = 0;
    int outerX = 0, outerY = 0, outerW = 0, outerH = 0;
};

InkBox inkBoxOfRgba8(const u8* rgba, int imageWidth, int imageHeight, int x, int y, int w, int h);

}  // namespace compat::ui
