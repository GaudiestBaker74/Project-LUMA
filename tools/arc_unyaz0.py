#!/usr/bin/env python3
"""Yaz0 -> RARC: decompress a Super Mario Galaxy `.arc` (stdlib only).

    python3 tools/arc_unyaz0.py ObjectData/CometNearOrbitSky.arc out.rarc

The archives shipped in the game are Yaz0-compressed (magic "Yaz0", the
decompressed size big-endian at +0x04, the stream starting at +0x10). The
result is a plain RARC, which

  * `build/src/tools/sky-probe` reads to list the model's draw items / tex-matrix
    modes and to dump every texture as PPM, and
  * `galaxy-pc --assets-dir <tree>` mounts as a normal asset (copy it back as
    ObjectData/<Name>.arc, still Yaz0-compressed — the DVD layer decompresses on
    the fly).
"""

import struct
import sys


def yaz0_decode(src: bytes) -> bytes:
    if src[:4] != b"Yaz0":
        raise SystemExit("not a Yaz0 stream (magic %r)" % src[:4])
    size = struct.unpack(">I", src[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = src[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(src[i])
                i += 1
            else:
                b1, b2 = src[i], src[i + 1]
                i += 2
                dist = ((b1 & 0x0F) << 8) | b2
                count = b1 >> 4
                if count == 0:
                    count = src[i] + 0x12
                    i += 1
                else:
                    count += 2
                start = len(out) - (dist + 1)
                for k in range(count):
                    out.append(out[start + k])
    return bytes(out[:size])


def main(argv) -> int:
    if len(argv) < 2 or len(argv) > 3:
        print(__doc__)
        return 2
    data = open(argv[1], "rb").read()
    raw = yaz0_decode(data)
    out = argv[2] if len(argv) > 2 else argv[1].rsplit(".", 1)[0] + ".rarc"
    with open(out, "wb") as fp:
        fp.write(raw)
    print("%s: %d -> %d bytes, magic %r, wrote %s" % (argv[1], len(data), len(raw), raw[:4], out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
