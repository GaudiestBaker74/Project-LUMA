#!/usr/bin/env python3
"""Offline simulator + verifier of the ButtonPrompt icons (PC_PORT).

Mirrors the geometry ButtonPrompt.cpp emits through GX at the reference frame's
own scale (cap height = 24 px at 1280x720) so the reproduction can be compared
with ref/thumb.png pixel for pixel, and re-measures its own output with the very
same detector used on the original.

    python3 tools/prompt_icons.py            # render the icons -> /tmp/icons_sim.png
    python3 tools/prompt_icons.py medir      # + measure sim vs reference

Every number in TUNABLES is a measurement off the original title screen (see the
comment block in ButtonPrompt.cpp); nothing here is a re-design.
"""
import math
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# --- tuning, mirrored from ButtonPrompt.cpp (cap-height units) --------------
CAP = 24.0             # the line's cap height on the reference
ICON_SCALE = 1.62      # [A] outer diameter / cap        (39 px)
B_WIDTH_RATIO = 0.85   # [B] outer width / icon height   (33 px)
RING_RATIO_A = 0.09    # [A] ring thickness / height     (3.5 px)
RING_RATIO_B = 0.115   # [B] frame thickness / height    (4.5 px)
CORNER_RATIO = 0.18    # [B] corner radius / [B] width   (6 px)
LETTER_A = 0.54        # [A] letter height / icon height (21 px)
LETTER_B = 0.49        # [B] letter height / icon height (19 px)
DROP_Y = 0.05          # icon centre below the text centre (1.2 px)
WORD_GAP = 0.60        # gap / cap height                (14.4 px)

SEA = (52, 129, 139)
RING_INK = (9, 20, 26)
BLEED_INK = (9, 20, 26)
FACE_TOP = (252, 252, 252)
FACE_BOTTOM = (206, 212, 216)
LETTER_INK = (124, 126, 126)
ARC_SEGMENTS = 14
Z = 8  # simulator pixels per reference pixel


def circle_points(cx, cy, radius, segments=56):
    return [(cx + math.sin(a) * radius, cy + math.cos(a) * radius)
            for a in (2 * math.pi * i / segments for i in range(segments + 1))]


def tile_points(cx, cy, half_w, half_h, corner, arc_segments=ARC_SEGMENTS):
    """Same tracing as emitShape: four corner arcs joined by straight edges.

    (Sampling a support function radially would pinch the corners and turn the
    tile into a four-lobed clover -- that is exactly the bug this checks for.)
    """
    r = corner
    pts = []
    for q in range(4):
        ccx = cx + ((half_w - r) if q in (0, 3) else -(half_w - r))
        ccy = cy + ((half_h - r) if q in (0, 1) else -(half_h - r))
        for s in range(arc_segments + 1):
            a = math.radians(q * 90.0 + s * 90.0 / arc_segments)
            pts.append((ccx + math.cos(a) * r, ccy + math.sin(a) * r))
    return pts


def shape_points(disc, cx, cy, half_w, half_h, corner):
    if disc:
        return circle_points(cx, cy, half_w)
    return tile_points(cx, cy, half_w, half_h, corner)


def paint(img, disc, cx, cy, width, height, corner, top, bottom, alpha):
    """One shape, composited through a mask (Pillow's ImageDraw with mode
    'RGBA' REPLACES pixels on an RGBA image, it does not blend them -- so the
    shape always goes through a layer + alpha_composite)."""
    half_w, half_h = width * 0.5, height * 0.5
    pts = [(x * Z, y * Z) for x, y in shape_points(disc, cx, cy, half_w, half_h, corner)]
    mask = Image.new('L', img.size, 0)
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    if alpha < 255:
        mask = mask.point(lambda p: p * alpha // 255)
    layer = Image.new('RGBA', img.size, (0, 0, 0, 0))
    if top == bottom:
        layer.paste(top + (255,), (0, 0, img.width, img.height))
    else:
        rd = ImageDraw.Draw(layer)
        y0, y1 = (cy - half_h) * Z, (cy + half_h) * Z
        for py in range(max(0, int(y0)), min(img.height, int(y1) + 1)):
            t = (py - y0) / max(1.0, (y1 - y0))
            col = tuple(int(top[k] + (bottom[k] - top[k]) * t) for k in range(3))
            rd.line([(0, py), (img.width, py)], fill=col + (255,))
    layer.putalpha(mask)
    img.alpha_composite(layer)


def draw_button(img, disc, cx, cy, width, height, ring, corner, with_letter):
    half_w, half_h = width * 0.5, height * 0.5
    radius = half_w if disc else corner
    # soft shadow -> ring bleed -> ring -> top-lit face
    # four passes at the SAME size, sliding down: a vertical falloff instead of
    # concentric rings (which band visibly against the sea).
    for off, alpha in ((0.010, 90), (0.040, 52), (0.075, 30), (0.115, 15)):
        paint(img, disc, cx + off * height * 0.35, cy - off * height,
              half_w * 2 * 1.06, half_h * 2 * 1.12, radius, (0, 0, 0), (0, 0, 0), alpha)
    paint(img, disc, cx, cy, half_w * 2 * 1.012, half_h * 2 * 1.012, radius,
          BLEED_INK, BLEED_INK, 130)
    paint(img, disc, cx, cy, half_w * 2, half_h * 2, radius, RING_INK, RING_INK, 255)
    face_corner = (half_w - ring) if disc else corner - ring * 0.5
    paint(img, disc, cx, cy, (half_w - ring) * 2, (half_h - ring) * 2, face_corner,
          FACE_TOP, FACE_BOTTOM, 255)
    if not with_letter:
        return
    try:
        letter = 'A' if disc else 'B'
        target = CAP * (0.875 if disc else 0.79)  # letter cap = fraction of the line's cap
        font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
                                  int(target * Z))
        d = ImageDraw.Draw(img, 'RGBA')
        box = d.textbbox((0, 0), letter, font=font)
        d.text(((cx * Z) - (box[2] - box[0]) * 0.5 - box[0],
                (cy * Z) - (box[3] - box[1]) * 0.5 - box[1]), letter, font=font,
               fill=LETTER_INK + (255,))
    except Exception as exc:  # pragma: no cover
        print('fuente no disponible:', exc)


def render():
    size = CAP * ICON_SCALE
    b_w = size * B_WIDTH_RATIO
    corner = CORNER_RATIO * b_w
    pad = 30.0
    img = Image.new('RGBA', (int(230 * Z), int(150 * Z)), SEA + (255,))
    ax, ay = pad + size * 0.5, pad + size * 0.5 + CAP * DROP_Y
    bx = ax + size + 40.0
    draw_button(img, True, ax, ay, size, size, size * RING_RATIO_A, size * 0.5, True)
    draw_button(img, False, bx, ay, b_w, size, size * RING_RATIO_B, corner, True)
    return img, ax, ay, bx


def measure(arr, cx, cy, width, label):
    """Detector identical to the one used on the reference: scan the centre row
    and column out of the icon and read the bright face + the dark ring/frame."""
    L = arr.mean(2)
    half = int(round(width * 0.8))
    row = L[int(round(cy)), int(round(cx)) - half:int(round(cx)) + half + 1]
    col = L[int(round(cy)) - half:int(round(cy)) + half + 1, int(round(cx))]
    def face_and_ring(v, name):
        idx = np.where(v > 168)[0]
        if len(idx) == 0:
            return 0, 0, 0, 0
        i0, i1 = idx.min(), idx.max()
        def dark_out(i):
            n = 0
            i -= 1
            while i >= 0 and v[i] < 72:
                n += 1
                i -= 1
            return n
        def dark_in(i):
            n = 0
            i += 1
            while i < len(v) and v[i] < 72:
                n += 1
                i += 1
            return n
        return i1 - i0 + 1, dark_out(i0), dark_in(i1), name
    fw, rl, rr, _ = face_and_ring(row, 'x')
    fh, rt, rb, _ = face_and_ring(col, 'y')
    print('%-14s cara %2dx%-2d  anillo izq/der %d/%d  arriba/abajo %d/%d' % (
        label, fw, fh, rl, rr, rt, rb))


if __name__ == '__main__':
    img, ax, ay, bx = render()
    out = '/tmp/icons_sim.png'
    img.convert('RGB').save(out)
    print('guardado', out, img.size)

    if len(sys.argv) > 1 and sys.argv[1] == 'medir':
        small = img.convert('RGB').resize((img.width // Z, img.height // Z), Image.LANCZOS)
        sim = np.asarray(small, float)  # now 1 px == 1 reference px
        ref = np.asarray(Image.open('/home/user/ref/thumb.png').convert('RGB'), float)
        print('\n--- [A] disco (referencia cap 24 px) ---')
        measure(ref, 671.5, 563.5, 39, 'ORIGINAL')
        measure(sim, ax, ay, 39, 'REPRODUCCION')
        print('\n--- [B] teja ---')
        measure(ref, 804.0, 564.0, 33, 'ORIGINAL')
        measure(sim, bx, ay, 33, 'REPRODUCCION')
        # el mar bajo la teja: el original lo oscurece con la sombra
        print('\n--- sombra (luminancia media 8 px bajo el borde inferior) ---')
        print('  ORIGINAL    %.0f' % ref[586:596, 795:815].mean())
        print('  REPRODUCCION %.0f' % sim[int(ay + CAP * ICON_SCALE * 0.5) + 8:
                                        int(ay + CAP * ICON_SCALE * 0.5) + 18,
                                        int(bx - 10):int(bx + 10)].mean())
