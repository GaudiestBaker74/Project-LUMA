// =============================================================================
// compat/ui — PictureGlyphs (see the header for the why).
// =============================================================================
#include "compat/ui/PictureGlyphs.h"

#include "compat/gx/Bti.h"
#include "platform/Log/Log.h"

#include <nw4r/ut/Font.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace compat::ui {

namespace {

/// Alpha at or above which a texel is the icon itself rather than its soft
/// shadow. RGB5A3 stores three bits of alpha, so the shadow sits low.
constexpr u8 kSolidAlpha = 160;

/// The font currently installed, and the glyphs already resolved from it.
const nw4r::ut::Font* gFont = nullptr;
std::unordered_map<u16, PictureGlyph> gCache;

/// Fills the normalised rectangles + texel sizes of `out` from `box`.
void fillRects(PictureGlyph& out, const InkBox& box, int sheetW, int sheetH) {
    const f32 invW = 1.0f / static_cast<f32>(sheetW);
    const f32 invH = 1.0f / static_cast<f32>(sheetH);

    out.solidU0 = static_cast<f32>(box.solidX) * invW;
    out.solidV0 = static_cast<f32>(box.solidY) * invH;
    out.solidU1 = static_cast<f32>(box.solidX + box.solidW) * invW;
    out.solidV1 = static_cast<f32>(box.solidY + box.solidH) * invH;

    out.outerU0 = static_cast<f32>(box.outerX) * invW;
    out.outerV0 = static_cast<f32>(box.outerY) * invH;
    out.outerU1 = static_cast<f32>(box.outerX + box.outerW) * invW;
    out.outerV1 = static_cast<f32>(box.outerY + box.outerH) * invH;

    out.solidW = static_cast<f32>(box.solidW);
    out.solidH = static_cast<f32>(box.solidH);
    out.outerW = static_cast<f32>(box.outerW);
    out.outerH = static_cast<f32>(box.outerH);
}

/// Resolves one code against the installed font: glyph index + cell + sheet, a
/// host decode of the sheet, and the ink boxes inside the cell.
bool resolveGlyph(u16 code, PictureGlyph& out) {
    nw4r::ut::Glyph glyph;
    gFont->GetGlyph(&glyph, code);

    const u32 sheetW = glyph.texWidth;
    const u32 sheetH = glyph.texHeight;

    if (glyph.pTexture == nullptr || sheetW == 0 || sheetH == 0) {
        PL_LOG_WARN("compat.font", "picture font: code 0x%04X has no glyph", code);
        return false;
    }

    const int cellW = gFont->GetCellWidth();
    const int cellH = gFont->GetCellHeight();

    if (cellW <= 0 || cellH <= 0 ||
        static_cast<u32>(glyph.cellX) + static_cast<u32>(cellW) > sheetW ||
        static_cast<u32>(glyph.cellY) + static_cast<u32>(cellH) > sheetH) {
        PL_LOG_WARN("compat.font", "picture font: code 0x%04X has an out-of-sheet cell (%d,%d %dx%d of %ux%u)",
                    code, glyph.cellX, glyph.cellY, cellW, cellH, sheetW, sheetH);
        return false;
    }

    // Decode the sheet once (the sheets are tiny: 128x128 = 64 KiB here) and
    // measure the ink boxes inside this glyph's cell.
    const size_t sheetBytes =
        Platform::CompatGx::btiImageSize(sheetW, sheetH, static_cast<u8>(glyph.texFormat));
    std::vector<u8> rgba(static_cast<size_t>(sheetW) * sheetH * 4);

    if (!Platform::CompatGx::btiDecodeToRgba8(static_cast<const u8*>(glyph.pTexture), sheetBytes, sheetW,
                                              sheetH, static_cast<u8>(glyph.texFormat), nullptr, 0, 0,
                                              rgba.data())) {
        PL_LOG_WARN("compat.font", "picture font: cannot decode the %ux%u sheet (format %d) for code 0x%04X",
                    sheetW, sheetH, static_cast<int>(glyph.texFormat), code);
        return false;
    }

    const InkBox box = inkBoxOfRgba8(rgba.data(), static_cast<int>(sheetW), static_cast<int>(sheetH),
                                     glyph.cellX, glyph.cellY, cellW, cellH);

    if (!box.any) {
        PL_LOG_WARN("compat.font", "picture font: code 0x%04X resolves to an empty cell", code);
        return false;
    }

    out.valid = true;
    out.sheetImage = glyph.pTexture;
    out.sheetWidth = static_cast<u16>(sheetW);
    out.sheetHeight = static_cast<u16>(sheetH);
    out.sheetFormat = static_cast<u16>(glyph.texFormat);
    fillRects(out, box, static_cast<int>(sheetW), static_cast<int>(sheetH));

    PL_LOG_INFO("compat.font",
                "picture font: code 0x%04X -> cell (%u,%u) %dx%d of %ux%u, CWDH left=%d glyph=%d advance=%d, "
                "icon %dx%d texels, with edge %dx%d",
                code, glyph.cellX, glyph.cellY, cellW, cellH, sheetW, sheetH,
                static_cast<int>(glyph.widths.left), static_cast<int>(glyph.widths.glyphWidth),
                static_cast<int>(glyph.widths.charWidth), box.solidW, box.solidH, box.outerW, box.outerH);

    return true;
}

}  // namespace

void setPictureFont(const nw4r::ut::Font* font) {
    if (gFont == font) {
        return;
    }

    gFont = font;
    gCache.clear();

    if (gFont == nullptr) {
        return;
    }

    PL_LOG_INFO("compat.font", "picture font installed for the [A]/[B] icons (cell %dx%d)",
                gFont->GetCellWidth(), gFont->GetCellHeight());

    // Resolve the two codes the instruction line uses right away: the log then
    // says whether the exact glyph art is in use or the vector fallback.
    PictureGlyph probe;
    const bool haveA = pictureGlyph(kPictureCodeA, probe);
    const bool haveB = pictureGlyph(kPictureCodeB, probe);

    PL_LOG_INFO("compat.font", "picture font: [A]=%s [B]=%s", haveA ? "real glyph" : "MISSING",
                haveB ? "real glyph" : "MISSING");
}

void clearPictureFont() {
    setPictureFont(nullptr);
}

bool pictureGlyph(u16 code, PictureGlyph& out) {
    out = PictureGlyph();

    if (gFont == nullptr) {
        return false;
    }

    const auto it = gCache.find(code);

    if (it != gCache.end()) {
        out = it->second;
        return out.valid;
    }

    PictureGlyph glyph;
    glyph.valid = resolveGlyph(code, glyph);
    gCache.emplace(code, glyph);

    out = glyph;
    return glyph.valid;
}

InkBox inkBoxOfRgba8(const u8* rgba, int imageWidth, int imageHeight, int x, int y, int w, int h) {
    InkBox box;

    if (rgba == nullptr || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > imageWidth ||
        y + h > imageHeight) {
        return box;
    }

    // Does the sheet carry alpha at all? An opaque sheet (some fonts store the
    // background as black opaque) has to be measured by luminance instead.
    bool hasAlpha = false;

    for (int ty = 0; ty < h && !hasAlpha; ++ty) {
        for (int tx = 0; tx < w; ++tx) {
            if (rgba[(static_cast<size_t>(y + ty) * imageWidth + (x + tx)) * 4 + 3] != 0) {
                hasAlpha = true;
                break;
            }
        }
    }

    int solidX0 = w, solidY0 = h, solidX1 = -1, solidY1 = -1;
    int outerX0 = w, outerY0 = h, outerX1 = -1, outerY1 = -1;

    for (int ty = 0; ty < h; ++ty) {
        for (int tx = 0; tx < w; ++tx) {
            const u8* p = &rgba[(static_cast<size_t>(y + ty) * imageWidth + (x + tx)) * 4];
            const u8 alpha = p[3];
            const int lum = (static_cast<int>(p[0]) * 30 + static_cast<int>(p[1]) * 59 +
                             static_cast<int>(p[2]) * 11) /
                            100;

            const bool isOuter = hasAlpha ? (alpha > 0) : (lum > 16);
            const bool isSolid = hasAlpha ? (alpha >= kSolidAlpha) : (lum > 64);

            if (isOuter) {
                if (tx < outerX0) outerX0 = tx;
                if (tx > outerX1) outerX1 = tx;
                if (ty < outerY0) outerY0 = ty;
                if (ty > outerY1) outerY1 = ty;
            }
            if (isSolid) {
                if (tx < solidX0) solidX0 = tx;
                if (tx > solidX1) solidX1 = tx;
                if (ty < solidY0) solidY0 = ty;
                if (ty > solidY1) solidY1 = ty;
            }
        }
    }

    if (outerX1 < outerX0 || outerY1 < outerY0) {
        return box;  // nothing painted in this cell
    }

    // A glyph with no opaque texels (all of it soft) still has an icon box.
    if (solidX1 < solidX0 || solidY1 < solidY0) {
        solidX0 = outerX0;
        solidY0 = outerY0;
        solidX1 = outerX1;
        solidY1 = outerY1;
    }

    box.any = true;
    // Image-space coordinates: the caller passes the cell's origin (x, y), and
    // the box has to come back in the SAME space the sheet is indexed in — this
    // is what the sampler's u/v are computed from. Returning them relative to
    // the cell would make the quad sample the top-left corner of the sheet: for
    // the picture font that is glyph 0, the blank space, so the icons came out
    // invisible.
    box.solidX = x + solidX0;
    box.solidY = y + solidY0;
    box.solidW = solidX1 - solidX0 + 1;
    box.solidH = solidY1 - solidY0 + 1;
    box.outerX = x + outerX0;
    box.outerY = y + outerY0;
    box.outerW = outerX1 - outerX0 + 1;
    box.outerH = outerY1 - outerY0 + 1;
    return box;
}

}  // namespace compat::ui
