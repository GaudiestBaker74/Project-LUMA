// =============================================================================
// compat/ui — ButtonPrompt (see ButtonPrompt.h).
// =============================================================================
#include "compat/ui/ButtonPrompt.h"

#include "compat/game/UiAnchoring.h"
#include "compat/ui/PictureGlyphs.h"
#include "platform/Log/Log.h"

#include <nw4r/ut/CharWriter.h>
#include <nw4r/ut/Color.h>
#include <nw4r/ut/Font.h>
#include <nw4r/ut/Rect.h>
#include <nw4r/ut/TextWriterBase.h>
#include <nw4r/ut/WideTextWriter.h>

#include <revolution/gx.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace compat::ui {

namespace {

// -----------------------------------------------------------------------------
// Item list: the message is split into text runs and button slots.
// -----------------------------------------------------------------------------
namespace {
bool isTokenA(const wchar_t* p) {
    return p[0] == L'[' && p[1] == L'A' && p[2] == L']';
}

bool isTokenB(const wchar_t* p) {
    return p[0] == L'[' && p[1] == L'B' && p[2] == L']';
}
}  // namespace

// -----------------------------------------------------------------------------
// Measuring / drawing through the layout's own text writer.
// -----------------------------------------------------------------------------
f32 measureRun(nw4r::ut::WideTextWriter* writer, const wchar_t* text, int len) {
    nw4r::ut::Rect rect;
    writer->CalcStringRect(&rect, text, len);
    return rect.GetWidth();
}

/// Prints one run with its pen at (x, y); the writer's own font/colour/space
/// settings are already the layout's (SetTextColor was chosen by the caller).
void printRun(nw4r::ut::WideTextWriter* writer, const wchar_t* text, int len, f32 x, f32 y) {
    writer->SetCursor(x, y);
    writer->Print(text, len);
}

// -----------------------------------------------------------------------------
// The [A]/[B] icons.
//
// PREFERRED: the picture-font glyphs the console itself draws (compat/ui/
// PictureGlyphs.h — /LayoutData/Font.arc → /PictureFont.brfnt). Codes 0x0030
// ([A]) and 0x0031 ([B]) were identified from the user's own dump of the font.
//
// FALLBACK (no font mounted: assets without LayoutData/, unit tests): vector
// icons that reproduce the same art. Every number below is a MEASUREMENT off
// the original title screen (ref/thumb.png, 1280x720, line cap height 24 px),
// never a re-design:
//
//   [A]  outer disc 39 px = 1.62 x cap   bright face 32 px = 1.33 x cap
//        dark ring ~3.5 px (0.09 x diameter); face white, lit from the top
//        (~252 -> ~208); soft shadow below
//   [B]  portrait tile 33 x 39 px (0.85 x height); face 24 x 32 px
//        dark frame ~4 px (0.12 x width); corner radius ~6 px (0.18 x width)
//   both letters mid grey (~122) 21 px (A) / 19 px (B) — never black
//        icon centre ~1.2 px (0.05 cap) below the text centre
//
// Whatever the path, the geometry is emitted as immediate geometry through the
// compat GX layer, so it rides the position matrix the pane already loaded
// (LoadMtx) and lands in exactly the same space as the text.
// -----------------------------------------------------------------------------
enum class Shape { Disc, RoundedSquare };

void beginGlyphGeometry() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                  GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    // The icons have to composite over the sky/planet band exactly like the
    // text does: straight alpha.
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZCompLoc(GX_FALSE);
}

/// Vertical interpolation between two colours, pre-multiplied by the pane
/// alpha (the icons composite exactly like the text).
GXColor lerpColor(const GXColor& top, const GXColor& bottom, f32 t, u8 paneAlpha) {
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    const f32 a = 1.0f - t;
    const f32 alpha = (static_cast<f32>(top.a) * a + static_cast<f32>(bottom.a) * t) *
                      (static_cast<f32>(paneAlpha) / 255.0f);
    GXColor out;
    out.r = static_cast<u8>(static_cast<f32>(top.r) * a + static_cast<f32>(bottom.r) * t);
    out.g = static_cast<u8>(static_cast<f32>(top.g) * a + static_cast<f32>(bottom.g) * t);
    out.b = static_cast<u8>(static_cast<f32>(top.b) * a + static_cast<f32>(bottom.b) * t);
    out.a = static_cast<u8>(alpha < 0.0f ? 0.0f : (alpha > 255.0f ? 255.0f : alpha));
    return out;
}

/// Perimeter of a disc / rounded rectangle as ONE triangle fan.
///
/// The tile is traced as FOUR CORNER ARCS JOINED BY STRAIGHT EDGES. (Sampling
/// the support function radially does NOT give the boundary of an elongated
/// rounded rectangle: it pinches the corners and the tile comes out as a
/// four-lobed clover. That was visible against the reference — see
/// tools/prompt_icons.py, which mirrors this code.)
///
/// `top`/`bottom` are interpolated vertically, which is what gives the
/// original faces their soft top-lit bevel.
void emitShape(Shape shape, f32 cx, f32 cy, f32 halfW, f32 halfH, f32 corner,
               const GXColor& top, const GXColor& bottom, u8 paneAlpha) {
    constexpr int kCircleSegments = 56;
    constexpr int kArcSegments = 14;  // per corner -> 60 perimeter points
    const int perimeter = (shape == Shape::Disc) ? kCircleSegments : 4 * (kArcSegments + 1);
    const f32 radius = (shape == Shape::Disc) ? halfW : corner;
    const f32 scaleY = halfH > 0.0f ? halfH : 1.0f;

    GXBegin(GX_TRIANGLEFAN, GX_VTXFMT0, static_cast<u16>(perimeter + 2));
    GXPosition2f32(cx, cy);
    const GXColor center = lerpColor(top, bottom, 0.5f, paneAlpha);
    GXColor4u8(center.r, center.g, center.b, center.a);

    for (int i = 0; i <= perimeter; ++i) {
        const int idx = (i == perimeter) ? 0 : i;
        f32 x;
        f32 y;
        if (shape == Shape::Disc) {
            // Start at +Y and go clockwise so the winding matches the layout
            // quads (the GX layer culls nothing here, but keep it consistent).
            const f32 a =
                static_cast<f32>(idx) * (2.0f * 3.14159265f / static_cast<f32>(kCircleSegments));
            x = cx + std::sin(a) * halfW;
            y = cy + std::cos(a) * halfW;
        } else {
            const int quadrant = idx / (kArcSegments + 1);
            const int step = idx % (kArcSegments + 1);
            const f32 deg = static_cast<f32>(quadrant) * 90.0f +
                            static_cast<f32>(step) * (90.0f / static_cast<f32>(kArcSegments));
            const f32 a = deg * 3.14159265f / 180.0f;
            const f32 ccx = cx + ((quadrant == 0 || quadrant == 3) ? (halfW - radius)
                                                                   : -(halfW - radius));
            const f32 ccy = cy + ((quadrant == 0 || quadrant == 1) ? (halfH - radius)
                                                                   : -(halfH - radius));
            x = ccx + std::cos(a) * radius;
            y = ccy + std::sin(a) * radius;
        }
        // +Y is down in this space, so a point ABOVE the centre (y < cy) has to
        // take the `top` colour: t = 0 at the top edge.
        const GXColor c = lerpColor(top, bottom, 0.5f + 0.5f * ((y - cy) / scaleY), paneAlpha);
        GXPosition2f32(x, y);
        GXColor4u8(c.r, c.g, c.b, c.a);
    }
    GXEnd();
}

nw4r::ut::Color makeColor(const GXColor& c) {
    return nw4r::ut::Color(c);
}

/// One icon: soft shadow -> dark ring/frame -> light face. `width`/`height` are
/// the OUTER size, `ring` the thickness of the dark ring ([A]) / frame ([B]).
void drawIcon(Shape shape, f32 centerX, f32 centerY, f32 width, f32 height, f32 ring,
              f32 corner, u8 paneAlpha) {
    const f32 halfW = width * 0.5f;
    const f32 halfH = height * 0.5f;
    const f32 radius = shape == Shape::Disc ? halfW : corner;

    // Soft shadow, four passes (tight -> wide): the original drops a diffuse
    // dark halo below the button — the sea right under it falls from ~110 to
    // ~45 luminance and the halo fades out over ~15 px. Tuned against the
    // reference with tools/prompt_icons.py.
    // Four passes of the SAME size sliding downwards: that builds a vertical
    // falloff instead of concentric rings (rings band visibly against the sea).
    const f32 shadowOffset[4] = {0.010f, 0.040f, 0.075f, 0.115f};
    const u8 shadowAlpha[4] = {90, 52, 30, 15};
    for (int pass = 0; pass < 4; ++pass) {
        const f32 off = shadowOffset[pass];
        const GXColor ink{0, 0, 0, shadowAlpha[pass]};
        emitShape(shape, centerX + off * height * 0.35f, centerY + off * height, halfW * 1.06f,
                  halfH * 1.12f, radius, ink, ink, paneAlpha);
    }

    // The ring/frame itself: near black, very slightly cool, and with a faint
    // bleed so its outer contour fades into the sea like the original's.
    const GXColor ringInk{9, 20, 26, 255};
    const GXColor ringBleed{9, 20, 26, 130};
    emitShape(shape, centerX, centerY, halfW * 1.012f, halfH * 1.012f, radius, ringBleed,
              ringBleed, paneAlpha);
    emitShape(shape, centerX, centerY, halfW, halfH, radius, ringInk, ringInk, paneAlpha);

    // Face: white, top-lit (the original face reads ~252 top-left -> ~208
    // bottom-right), inset by the ring thickness.
    const GXColor faceTop{252, 252, 252, 255};
    const GXColor faceBottom{206, 212, 216, 255};
    const f32 faceCorner = shape == Shape::Disc ? halfW - ring : corner - ring * 0.5f;
    emitShape(shape, centerX, centerY, halfW - ring, halfH - ring, faceCorner, faceTop,
              faceBottom, paneAlpha);
}

/// The texel -> layout scale the picture font is drawn at.
///
/// IMPORTANT: the console draws EVERY glyph of a font at ONE scale (the size
/// the layout asks for). The icons only differ because their art differs, so
/// the scale is derived ONCE from the [A] glyph and shared — normalising each
/// glyph on its own would break the proportion between the two icons.
///
/// `faceHeight` is the height the icon's FACE must have (the white disc on the
/// original: 1.33 cap heights = 32 px), and the face is exactly the glyph's
/// SOLID ink box -- the `outer` box around it is the art's own thin edge, not
/// growth. Scaling by `solidH` is therefore what puts the icon at the measured
/// size; scaling by the outer box made it ~22% too big (49 px instead of the
/// reference's 32 px, which is what the captures showed).
f32 pictureGlyphScale(f32 faceHeight) {
    PictureGlyph glyph;

    if (pictureGlyph(kPictureCodeA, glyph) && glyph.solidH > 0.0f) {
        return faceHeight / glyph.solidH;
    }
    if (pictureGlyph(kPictureCodeB, glyph) && glyph.solidH > 0.0f) {
        return faceHeight / glyph.solidH;
    }
    return 0.0f;
}

/// The console's own art: draws the picture-font glyph `code` (see
/// compat/ui/PictureGlyphs.h) as one textured quad.
///
/// The quad covers the glyph's whole ink box (`outer` — the art has no soft
/// shadow of its own: the [A] glyph closes with an antialiased edge) and is
/// centred on the slot. `referenceFaceHeight` is the height the line's faces
/// must have (the [A] disc's); the other icon is BOTTOM-ALIGNED to it instead
/// of centred, which is what the reference shows -- both buttons rest on the
/// same line. The letter is part of the art, so nothing is printed on top.
///
/// Returns false when no picture font is installed or it has no glyph for the
/// code; the caller then draws the vector fallback.
bool drawPictureGlyph(u16 code, f32 centerX, f32 centerY, f32 scale, f32 referenceFaceHeight,
                      u8 paneAlpha) {
    PictureGlyph glyph;

    if (!pictureGlyph(code, glyph) || glyph.outerH <= 0.0f || scale <= 0.0f) {
        return false;
    }

    const f32 quadW = glyph.outerW * scale;
    const f32 quadH = glyph.outerH * scale;

    // Distance between the centres of the two ink boxes: the ICON box has to sit
    // on the slot centre, not the box of the art around it.
    const f32 shiftU = ((glyph.outerU0 + glyph.outerU1) - (glyph.solidU0 + glyph.solidU1)) * 0.5f;
    const f32 shiftV = ((glyph.outerV0 + glyph.outerV1) - (glyph.solidV0 + glyph.solidV1)) * 0.5f;
    const f32 cx = centerX - shiftU * static_cast<f32>(glyph.sheetWidth) * scale;
    // v grows downwards in the sheet, and so does y in this space (the same one
    // the words are printed in), so the sheet offset carries over as-is. The
    // bottom alignment rests the shorter icon's FACE on the taller one's: on the
    // reference both buttons share their lower edge.
    const f32 cy = centerY + shiftV * static_cast<f32>(glyph.sheetHeight) * scale +
                   (referenceFaceHeight - glyph.solidH * scale) * 0.5f;

    GXTexObj texObj;
    GXInitTexObj(&texObj, const_cast<void*>(glyph.sheetImage), glyph.sheetWidth, glyph.sheetHeight,
                 static_cast<GXTexFmt>(glyph.sheetFormat), GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&texObj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);

    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE,
                  GX_AF_NONE);

    GXLoadTexObj(&texObj, GX_TEXMAP0);

    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);

    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    // texel * vertex colour (white, pane alpha) — the same modulate the layout's
    // own textured quads use.
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);

    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZCompLoc(GX_FALSE);

    const f32 x0 = cx - quadW * 0.5f;
    const f32 x1 = cx + quadW * 0.5f;
    // +Y grows DOWNWARDS in this space (the same space the words are printed
    // in: printRun places the pen at the TOP of the line and the glyphs extend
    // to larger y). The sheet's v axis also grows downwards, so v0 (the top row
    // of the glyph) belongs to the SMALLER y. Getting this backwards draws the
    // icons upside down.
    const f32 yTop = cy - quadH * 0.5f;     // sheet v0
    const f32 yBottom = cy + quadH * 0.5f;  // sheet v1

    // Attributes in ascending GX_VA_* order (POS, CLR0, TEX0): the order the
    // vertex descriptor is walked in. Clockwise from the top-left, like the
    // layout's own quads.
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    {
        GXPosition2f32(x0, yTop);
        GXColor4u8(255, 255, 255, paneAlpha);
        GXTexCoord2f32(glyph.outerU0, glyph.outerV0);

        GXPosition2f32(x1, yTop);
        GXColor4u8(255, 255, 255, paneAlpha);
        GXTexCoord2f32(glyph.outerU1, glyph.outerV0);

        GXPosition2f32(x1, yBottom);
        GXColor4u8(255, 255, 255, paneAlpha);
        GXTexCoord2f32(glyph.outerU1, glyph.outerV1);

        GXPosition2f32(x0, yBottom);
        GXColor4u8(255, 255, 255, paneAlpha);
        GXTexCoord2f32(glyph.outerU0, glyph.outerV1);
    }
    GXEnd();

    return true;
}

/// Ratio between the height of the capitals the line draws and the ASCENT the
/// text writer reports for the same font.
///
/// The reference measurements (39 px outer disc / 32 px face / 3.5 px ring,
/// 21 px letters, ~14 px gaps — all against a 24 px cap) are in CAP HEIGHTS,
/// while the writer only offers the ASCENT, which on this font is taller:
/// measured on the title at 720p the capitals are 26 px while the writer
/// reports ~30 px, i.e. 26/30 = 0.87. Everything the prompt derives from
/// `ctx.capHeight` — the icon size, its slots, the gaps and dropY — goes
/// through this factor.
constexpr f32 kFontAscentToCapHeight = 0.87f;

// Tunables (env-overridable for A/B tuning against the reference capture).
// The defaults are the reference measurements in cap-height units: on the
// original the line's cap height is 24 px, so 1.62 * 24 = 38.9 px ~ the 39 px
// outer disc that was measured, and so on.
struct PromptTuning {
    f32 iconScale = 1.62f;   // [A] outer diameter / cap height   (39 px)
    f32 bWidthRatio = 0.85f; // [B] outer width / icon height     (33 px)
    f32 ringRatio = 0.09f;   // [A] ring thickness / icon height  (3.5 px)
    f32 ringRatioB = 0.115f; // [B] frame thickness / icon height (4.5 px)
    f32 cornerRatio = 0.18f; // [B] corner radius / [B] width     (6 px)
    f32 letterA = 0.54f;     // [A] letter height / icon height   (21 px)
    f32 letterB = 0.49f;     // [B] letter height / icon height   (19 px)
    f32 wordGap = 0.60f;     // gap / cap height                  (~14 px)
    f32 periodGap = 0.10f;
    f32 dropY = 0.05f;       // icon centre below the text centre (~1.2 px)
};

const PromptTuning& tuning() {
    static PromptTuning t = [] {
        PromptTuning out;
        const auto readEnv = [](const char* name, f32 def) {
            const char* v = std::getenv(name);
            if (v == nullptr || v[0] == '\0') {
                return def;
            }
            const f32 parsed = static_cast<f32>(std::atof(v));
            return parsed > 0.0f ? parsed : def;
        };
        out.iconScale = readEnv("LUMA_PROMPT_ICON_SCALE", out.iconScale);
        out.bWidthRatio = readEnv("LUMA_PROMPT_B_WIDTH", out.bWidthRatio);
        out.ringRatio = readEnv("LUMA_PROMPT_RING", out.ringRatio);
        out.ringRatioB = readEnv("LUMA_PROMPT_RING_B", out.ringRatioB);
        out.wordGap = readEnv("LUMA_PROMPT_WORD_GAP", out.wordGap);
        out.periodGap = readEnv("LUMA_PROMPT_PERIOD_GAP", out.periodGap);
        out.cornerRatio = readEnv("LUMA_PROMPT_CORNER", out.cornerRatio);
        out.letterA = readEnv("LUMA_PROMPT_LETTER_A", out.letterA);
        out.letterB = readEnv("LUMA_PROMPT_LETTER_B", out.letterB);
        return out;
    }();
    return t;
}

}  // namespace

ButtonIconMetrics buttonIconMetrics(f32 capHeight) {
    const PromptTuning& t = tuning();
    ButtonIconMetrics m{};
    m.capHeight = capHeight;
    m.size = capHeight * t.iconScale;
    // `size` is the icon's OUTER box (ring included); the picture-font glyphs
    // carry the ring in their own art, so what has to be scaled to that box is
    // its SOLID ink — the face — not the box around it.
    m.faceSize = m.size * (1.0f - 2.0f * t.ringRatio);
    m.bWidth = m.size * t.bWidthRatio;
    m.ringWidth = m.size * t.ringRatio;
    m.bRingWidth = m.size * t.ringRatioB;
    m.wordGap = capHeight * t.wordGap;
    m.periodGap = capHeight * t.periodGap;
    m.cornerRatio = t.cornerRatio;
    m.letterA = m.size * t.letterA;
    m.letterB = m.size * t.letterB;
    m.dropY = capHeight * t.dropY;
    return m;
}

bool drawButtonPrompt(const PromptDrawContext& ctx) {
    if (ctx.writer == nullptr || ctx.text == nullptr || ctx.textLen == 0 || ctx.capHeight <= 0.0f) {
        return false;
    }

    PromptItem items[kMaxPromptItems];
    const int itemCount =
        splitPromptMessage(ctx.text, static_cast<int>(ctx.textLen), items, kMaxPromptItems);

    if (itemCount <= 0) {
        return false;
    }

    // `ctx.capHeight` is what the text writer reports: the font's ASCENT. The
    // metrics above are expressed in CAP HEIGHTS — the height of the capitals
    // the line actually draws — and on this font the ascent is the taller of
    // the two (measured on the title at 720p: the capitals are 26 px while the
    // writer reports 30 px). Feeding the ascent straight in made the icons ~15%
    // too big for the words (their slots too, which also widened the line).
    const ButtonIconMetrics metrics = buttonIconMetrics(ctx.capHeight * kFontAscentToCapHeight);

    // --- measure -------------------------------------------------------------
    f32 textWidths[kPromptSlotCount] = {0.0f, 0.0f, 0.0f};
    int textRun = 0;
    for (int i = 0; i < itemCount; ++i) {
        if (items[i].kind == PromptItem::Text) {
            items[i].width = measureRun(ctx.writer, items[i].text, items[i].len);
            // The measured runs are: the words before [A] (one or two words),
            // the word between the icons, and the trailing period run.
        }
    }

    // --- LAYOUT-SPACE BLOCK --------------------------------------------------
    // The instruction line is ONE block. Its slots carry mixed kinds (text runs
    // and icons), so walk them in order and place each at the running cursor
    // that starts at `block.startX`.
    //
    // Slot widths: text runs use the font advance; an icon slot is as wide as
    // the icon actually gets drawn — the picture-font glyph's ink box at the
    // shared texel scale. Without the font it is the measured vector width
    // ([A] the disc diameter, [B] the narrower tile).
    const f32 glyphScale = pictureGlyphScale(metrics.faceSize);

    const auto slotWidth = [&metrics, glyphScale](const PromptItem& item) {
        if (item.kind == PromptItem::Text) {
            return item.width;
        }

        if (glyphScale > 0.0f) {
            PictureGlyph glyph;
            const u16 code = item.kind == PromptItem::ButtonA ? kPictureCodeA : kPictureCodeB;

            if (pictureGlyph(code, glyph) && glyph.outerW > 0.0f) {
                return glyph.outerW * glyphScale;
            }
        }

        return item.kind == PromptItem::ButtonA ? metrics.size : metrics.bWidth;
    };

    f32 total = 0.0f;
    for (int i = 0; i < itemCount; ++i) {
        total += slotWidth(items[i]);
        if (i + 1 < itemCount) {
            total += metrics.wordGap;
        }
    }
    const f32 startX = ctx.blockCenterX - total * 0.5f;

    // --- draw ----------------------------------------------------------------
    f32 cursor = startX;
    f32 iconCenter[kPromptSlotCount] = {0.0f, 0.0f};
    bool iconFromFont[kPromptSlotCount] = {false, false};
    int iconCount = 0;
    // The icon is centred on the TEXT's vertical centre, a touch below it
    // (dropY, measured on the reference). In this space +Y is down and
    // `ctx.textTop` is the text writer's top line, i.e. the ASCENT line: the
    // capitals hang `ascent - capHeight` below it, so the centre the icons sit
    // on is that much lower than the line's top.
    const f32 capsTop = ctx.textTop + (ctx.capHeight - metrics.capHeight);
    const f32 iconCenterY = capsTop + metrics.capHeight * 0.5f + metrics.dropY;

    for (int i = 0; i < itemCount; ++i) {
        if (items[i].kind == PromptItem::Text) {
            cursor += items[i].width;
        } else {
            const bool isA = items[i].kind == PromptItem::ButtonA;
            const Shape shape = isA ? Shape::Disc : Shape::RoundedSquare;
            const f32 slot = slotWidth(items[i]);
            const f32 centerX = cursor + slot * 0.5f;

            // The original art first: the glyph the console draws, letters
            // included (that is why they are not printed on top of it).
            const bool real = drawPictureGlyph(isA ? kPictureCodeA : kPictureCodeB, centerX,
                                               iconCenterY, glyphScale, metrics.faceSize,
                                               ctx.globalAlpha);

            if (!real) {
                const f32 ring = isA ? metrics.ringWidth : metrics.bRingWidth;
                beginGlyphGeometry();
                drawIcon(shape, centerX, iconCenterY, slot, metrics.size, ring,
                         metrics.cornerRatio * metrics.bWidth, ctx.globalAlpha);
            }

            if (iconCount < kPromptSlotCount) {
                iconCenter[iconCount] = centerX;
                iconFromFont[iconCount] = real;
            }
            ++iconCount;
            cursor += slot;
        }
        if (i + 1 < itemCount) {
            cursor += metrics.wordGap;
        }
    }

    // --- words (+ letters of the vector fallback) on top of the icons --------
    GXSetNumTexGens(1);
    GXSetNumTevStages(1);

    const nw4r::ut::Color topColor = makeColor(ctx.colorTop);
    const nw4r::ut::Color bottomColor = makeColor(ctx.colorBottom);
    ctx.writer->SetTextColor(topColor, bottomColor);

    // Everything the TextBox does before printing, minus the parts the caller
    // already did (font/size/space/colour mapping).
    ctx.writer->SetupGX();

    cursor = startX;
    int icon = 0;
    for (int i = 0; i < itemCount; ++i) {
        if (items[i].kind == PromptItem::Text) {
            printRun(ctx.writer, items[i].text, items[i].len, cursor, ctx.textTop);
            cursor += items[i].width;
        } else {
            // With the real glyph the letter is part of the art, so only the
            // vector fallback prints one.
            if (!iconFromFont[icon]) {
                // Letter centred on the face, in the layout's font. On the
                // original the letters are mid grey (~122) — never black — and
                // measure 21 px (A) / 19 px (B) against the 24 px cap height.
                const bool isA = items[i].kind == PromptItem::ButtonA;
                const wchar_t letter = isA ? L'A' : L'B';
                const f32 letterTarget = isA ? metrics.letterA : metrics.letterB;
                // The words are printed at scale 1 with the layout's own font
                // and come out `metrics.capHeight` tall, so a letter printed at
                // scale s is s * capHeight tall: the measured target (21 px for
                // [A], 19 px for [B] against a 24 px cap) sets s directly.
                const f32 scale = letterTarget / metrics.capHeight;

                // Centre on the glyph's own advance, not on the font cell.
                nw4r::ut::Rect letterRect;
                ctx.writer->CalcStringRect(&letterRect, &letter, 1);
                const f32 letterWidth = letterRect.GetWidth() * scale;

                const GXColor inkRaw{124, 126, 126, ctx.globalAlpha};
                const nw4r::ut::Color ink = makeColor(inkRaw);
                ctx.writer->SetTextColor(ink);
                ctx.writer->SetScale(scale, scale);

                const f32 letterX = iconCenter[icon] - letterWidth * 0.5f;
                // printRun puts the pen on the ASCENT line (+Y down) and the
                // letter hangs (ascent - cap) below it, so its centre lands on
                // the icon's centre.
                const f32 letterY = iconCenterY - (ctx.capHeight * scale - letterTarget) -
                                    letterTarget * 0.5f;
                printRun(ctx.writer, &letter, 1, letterX, letterY);

                ctx.writer->SetScale(1.0f, 1.0f);
                ctx.writer->SetTextColor(topColor, bottomColor);
            }

            cursor += slotWidth(items[i]);
            ++icon;
        }
        if (i + 1 < itemCount) {
            cursor += metrics.wordGap;
        }
    }

    return true;
}

// The tokenizer lives outside the file-local helpers: it is part of the
// module surface so the pure split logic can be unit-tested without GX/fonts.
int splitPromptMessage(const wchar_t* text, int len, PromptItem* items, int maxItems) {
    if (text == nullptr || items == nullptr || len <= 0 || maxItems <= 0) {
        return 0;
    }

    int count = 0;
    int runStart = -1;
    int buttons = 0;

    const auto flushRun = [&](int end) {
        if (runStart >= 0 && end > runStart && count < maxItems) {
            PromptItem& item = items[count++];
            item.kind = PromptItem::Text;
            item.text = text + runStart;
            item.len = end - runStart;
            item.width = 0.0f;
        }
        runStart = -1;
    };

    int i = 0;
    while (i < len) {
        const wchar_t* p = text + i;
        const bool tokenA = (i + 2 < len) && isTokenA(p);
        const bool tokenB = (i + 2 < len) && isTokenB(p);

        if (tokenA || tokenB) {
            // Whitespace *around* a token belongs to the gaps between slots, not
            // to a word: the run being collected (if any) ends here, and a run
            // that would start with blanks is dropped.
            flushRun(i);
            if (count < maxItems) {
                PromptItem& item = items[count++];
                item.kind = tokenA ? PromptItem::ButtonA : PromptItem::ButtonB;
                item.text = nullptr;
                item.len = 0;
                item.width = 0.0f;
            }
            ++buttons;
            i += 3;
            while (i < len && (text[i] == L' ' || text[i] == L'\t')) {
                ++i;
            }
            continue;
        }

        if (text[i] == L' ' || text[i] == L'\t' || text[i] == L'\n') {
            flushRun(i);
        } else if (runStart < 0) {
            runStart = i;
        }
        ++i;
    }
    flushRun(len);

    return buttons > 0 ? count : 0;
}

}  // namespace compat::ui
