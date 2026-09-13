#!/usr/bin/env python3
"""ppm2png.py — PPM (P6) helper for the --screenshot / F12 frame captures.

The port writes frames as binary PPM (P6: 3-line header + raw RGB) so that no
image library is needed. This script converts those captures to PNG and can
measure them, using only the Python standard library (no PIL, no ImageMagick):

    python3 tools/ppm2png.py luma-frame-00900.ppm title.png
    python3 tools/ppm2png.py title.ppm --stats
    python3 tools/ppm2png.py title.ppm --seams
    python3 tools/ppm2png.py title.ppm --crop 293,158,694,404 logo.png

What the measurements are for (docs/title-widescreen.md): --seams reports the
columns whose left/right neighbourhoods differ most — the vertical-seam metric
for the aspect-adaptive background — and --stats reports the bounding box of
everything that is not the background colour, i.e. where the logo, the
"Press both [A] and [B]." line, the two button icons and the copyright landed.
"""

import argparse
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as fp:
        data = fp.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path}: not a binary PPM (P6) — got {data[:2]!r}")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":  # comment line
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace byte after the maxval
    width, height, maxval = fields
    if maxval != 255:
        raise SystemExit(f"{path}: maxval {maxval} unsupported (expected 255)")
    need = width * height * 3
    raw = data[pos : pos + need]
    if len(raw) != need:
        raise SystemExit(
            f"{path}: truncated — header says {width}x{height} ({need} bytes), got {len(raw)}")
    return width, height, raw


def write_png(path, width, height, rgb):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    rows = bytearray()
    stride = width * 3
    for y in range(height):
        rows.append(0)  # filter type 0 (None)
        rows += rgb[y * stride : (y + 1) * stride]
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as fp:
        fp.write(png)


def crop_png(src, dst, x, y, w, h):
    width, height, rgb = read_ppm(src)
    x = max(0, min(x, width - 1))
    y = max(0, min(y, height - 1))
    w = max(1, min(w, width - x))
    h = max(1, min(h, height - y))
    out = bytearray()
    for row in range(y, y + h):
        start = (row * width + x) * 3
        out += rgb[start : start + w * 3]
    write_png(dst, w, h, bytes(out))
    print(f"{dst}: {w}x{h} cropped from {src} at ({x},{y})")


def stats(path):
    width, height, rgb = read_ppm(path)
    total = width * height
    sums = [0, 0, 0]
    for i in range(0, total * 3, 3):
        sums[0] += rgb[i]
        sums[1] += rgb[i + 1]
        sums[2] += rgb[i + 2]
    avg = tuple(round(s / total) for s in sums)
    bg = rgb[0:3]
    min_x, min_y, max_x, max_y, diff = width, height, -1, -1, 0
    for y in range(height):
        row = y * width * 3
        for x in range(width):
            i = row + x * 3
            if rgb[i : i + 3] != bg:
                diff += 1
                min_x, min_y = min(min_x, x), min(min_y, y)
                max_x, max_y = max(max_x, x), max(max_y, y)
    print(f"{path}: {width}x{height}")
    print(f"  avg RGB            {avg}")
    print(f"  background (0,0)   {tuple(bg)}")
    print(f"  not-background     {diff}/{total} px "
          f"({100.0 * diff / total:.2f}%)")
    if max_x >= 0:
        print(f"  bbox               x=[{min_x},{max_x}] y=[{min_y},{max_y}] "
              f"({max_x - min_x + 1}x{max_y - min_y + 1})")
        print(f"  bbox center        x={(min_x + max_x) / 2:.1f} (frame center "
              f"{width / 2:.1f})  y={(min_y + max_y) / 2:.1f} (frame center "
              f"{height / 2:.1f})")
    else:
        print("  bbox               (uniform frame)")


def seams(path, top=10):
    width, height, rgb = read_ppm(path)
    scores = []
    for x in range(width - 1):
        acc = 0
        a, b = x * 3, (x + 1) * 3
        for y in range(height):
            i = y * width * 3
            acc += (abs(rgb[i + a] - rgb[i + b])
                    + abs(rgb[i + a + 1] - rgb[i + b + 1])
                    + abs(rgb[i + a + 2] - rgb[i + b + 2]))
        scores.append(acc / (height * 3))
    mean = sum(scores) / len(scores)
    print(f"{path}: column discontinuity (mean |p(x+1)-p(x)| over all rows)")
    print(f"  mean over columns  {mean:.3f}")
    worst = sorted(range(len(scores)), key=lambda x: scores[x], reverse=True)[:top]
    for x in worst:
        flag = "  <-- seam candidate" if scores[x] > max(2.0, 4 * mean) else ""
        print(f"  x={x:<6} {scores[x]:.3f}{flag}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="PPM (P6) file")
    ap.add_argument("output", nargs="?", help="PNG to write (default: input with .png)")
    ap.add_argument("--stats", action="store_true", help="print frame/bbox statistics")
    ap.add_argument("--seams", action="store_true", help="print the seam metric per column")
    ap.add_argument("--crop", metavar="X,Y,W,H",
                    help="write a cropped PNG instead of the full frame")
    args = ap.parse_args()

    if args.crop:
        try:
            x, y, w, h = (int(v) for v in args.crop.split(","))
        except ValueError:
            raise SystemExit("--crop wants X,Y,W,H (four integers)")
        out = args.output or (args.input.rsplit(".", 1)[0] + "-crop.png")
        crop_png(args.input, out, x, y, w, h)
    elif not args.stats and not args.seams:
        out = args.output or (args.input.rsplit(".", 1)[0] + ".png")
        width, height, rgb = read_ppm(args.input)
        write_png(out, width, height, rgb)
        print(f"{out}: {width}x{height} written")

    if args.stats:
        stats(args.input)
    if args.seams:
        seams(args.input)


if __name__ == "__main__":
    sys.exit(main())
