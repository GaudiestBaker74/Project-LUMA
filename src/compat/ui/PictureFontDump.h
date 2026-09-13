#pragma once
// =============================================================================
// compat/ui — PictureFontDump (PC_PORT, title-screen widescreen work).
//
// The console draws the [A]/[B] symbols of "Press both [A] and [B]." with
// PICTURE-FONT glyphs (the 0x1A tag, group 3), so the exact pixels are in the
// player's own /LayoutData/Font.arc -> /PictureFont.brfnt. This diagnostic
// dumps that font's code map and glyph sheets so the two glyph codes can be
// identified and wired into compat/ui/ButtonPrompt.
//
// Enable with:  LUMA_PICFONT_DUMP=<dir>      (default: current directory)
//
// Writes, for the mounted picture font:
//   picfont_sheet<N>.ppm   each glyph sheet, 4x, alpha flattened on gray
//   picfont_glyphs.ppm     ONE CELL PER GLYPH in code order (contact sheet)
//   picfont_codes.txt      code -> glyph index -> sheet cell, same order
//
// The contact sheet is the one to look at: whichever cell shows the round [A]
// button, its code is the line with the same position in picfont_codes.txt.
// =============================================================================

#include <revolution/types.h>

namespace compat::ui {

/// `brfntData` is the mounted /PictureFont.brfnt image (may be null when
/// /LayoutData/Font.arc is missing — that case is logged too). Does nothing
/// when LUMA_PICFONT_DUMP is unset, so it is free in normal runs.
bool dumpPictureFontIfRequested(const void* brfntData);

}  // namespace compat::ui
