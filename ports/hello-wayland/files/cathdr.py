#!/usr/bin/env python3
"""cat.png -> cat.h for hello-wayland, as upstream's Makefile makes it with
ImageMagick (`convert cat.png -define h:format=bgra -depth 8 cat.h`): one
C array, MagickImage, of the picture's pixels as B, G, R, A bytes. Pure
Python (zlib only), for an 8-bit RGBA, non-interlaced PNG, so the build
machine needs no ImageMagick.

    cathdr.py <in.png> <out.h>
"""
import struct
import sys
import zlib


def pixels(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("cathdr: %s: not a PNG" % path)
    pos, idat, w = 8, b"", None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype != 6 or interlace:
                sys.exit("cathdr: only 8-bit RGBA, non-interlaced PNGs")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, bpp, stride = zlib.decompress(idat), 4, w * 4
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    for i in range(0, len(out), 4):   # RGBA -> BGRA
        out[i], out[i + 2] = out[i + 2], out[i]
    return out


def main():
    px = pixels(sys.argv[1])
    with open(sys.argv[2], "w") as f:
        f.write("static const unsigned char\n  MagickImage[] =\n  {\n")
        for i in range(0, len(px), 12):
            f.write("    " + ", ".join("0x%02X" % b for b in px[i:i + 12]) + ",\n")
        f.write("  };\n")


if __name__ == "__main__":
    main()
