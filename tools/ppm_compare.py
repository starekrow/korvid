#!/usr/bin/env python3
"""Compares screenshots picture by picture: two files, or two directories by name.

    tools/ppm_compare.py A B [--threshold N] [--max-fraction F] [--max-channel N] [--diff DIR] [--png DIR]

A and B are .ppm (binary P6, what --screenshot and example_gallery write) or .png (8-bit RGB
or RGBA, not interlaced: what --png writes, and what references are kept as). Given two
directories, every picture in A is compared with the one of the same stem in B.

For each pair: the size (a different size fails), the largest difference in any channel, the
pixels whose largest channel difference passes --threshold and their fraction of the picture,
and the first such pixel with both values. A pair passes when that fraction is at most
--max-fraction and no channel differs by more than --max-channel. Two GPUs round linear
filtering and blending differently and an edge may land a pixel over, so a few pixels off by
a little are expected; a wrong blend, a missing layer, a sort out of order or a picture upside
down changes many pixels by a lot. The defaults are a first guess, to be fixed by the first
comparison of two backends and not loosened after (docs/PRDs/Engine-Slice-7-PRD.md, R9).

--diff DIR writes, for each pair that differs at all, the difference amplified (x8) as a PNG.
--png DIR writes every picture of A as a PNG (references are kept so: a gallery picture is a
few kilobytes instead of 1.5 MB). Exit status 1 if any pair fails. Standard library only.
"""
import argparse
import os
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    tokens, pos = [], 0
    while len(tokens) < 4:  # P6, width, height, maxval; '#' comments allowed
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        tokens.append(data[pos:end])
        pos = end
    if tokens[0] != b"P6" or int(tokens[3]) != 255:
        raise ValueError("%s: not a binary 8-bit PPM" % path)
    w, h = int(tokens[1]), int(tokens[2])
    pos += 1  # the one whitespace byte after maxval
    return w, h, bytes(data[pos:pos + w * h * 3])


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos, idat, w = 8, bytearray(), 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError("%s: only 8-bit RGB or RGBA PNGs, not interlaced" % path)
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(bytes(idat)), w * bpp
    out, prev = bytearray(w * h * 3), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):  # the five PNG filters
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * bpp:x * bpp + 3]
        prev = line
    return w, h, bytes(out)


def read(path):
    return read_png(path) if path.lower().endswith(".png") else read_ppm(path)


def write_png(path, w, h, rgb):
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        rows += rgb[y * w * 3:(y + 1) * w * 3]
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def compare(name, a, b, args):
    (wa, ha, pa), (wb, hb, pb) = a, b
    if (wa, ha) != (wb, hb):
        print("FAIL %-28s size %dx%d against %dx%d" % (name, wa, ha, wb, hb))
        return False
    worst, over, first = 0, 0, None
    diff = bytearray(len(pa)) if args.diff else None
    for i in range(0, len(pa), 3):
        d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
        if d > worst:
            worst = d
        if d > args.threshold:
            over += 1
            if first is None:
                first = (i // 3 % wa, i // 3 // wa, tuple(pa[i:i + 3]), tuple(pb[i:i + 3]))
        if diff is not None and d:
            for k in range(3):
                diff[i + k] = min(255, abs(pa[i + k] - pb[i + k]) * 8)
    fraction = over / float(wa * ha)
    ok = fraction <= args.max_fraction and worst <= args.max_channel
    where = "" if first is None else "; first at (%d, %d): %s against %s" % first
    print("%s %-28s largest channel difference %3d, %d pixels past %d (%.4f%%)%s" %
          ("ok  " if ok else "FAIL", name, worst, over, args.threshold, fraction * 100.0, where))
    if diff is not None and worst:
        os.makedirs(args.diff, exist_ok=True)
        write_png(os.path.join(args.diff, name + ".diff.png"), wa, ha, diff)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--threshold", type=int, default=8, help="a pixel counts as different past this channel difference (8)")
    ap.add_argument("--max-fraction", type=float, default=0.005, help="of the picture's pixels that may be different (0.005)")
    ap.add_argument("--max-channel", type=int, default=255, help="largest channel difference allowed anywhere (255: no limit)")
    ap.add_argument("--diff", help="write amplified differences here")
    ap.add_argument("--png", help="write A's pictures here as PNGs")
    args = ap.parse_args()

    if os.path.isdir(args.a):
        names = sorted(f for f in os.listdir(args.a) if f.lower().endswith((".ppm", ".png")))
        pairs = [(os.path.splitext(f)[0], os.path.join(args.a, f)) for f in names]
    else:
        pairs = [(os.path.splitext(os.path.basename(args.a))[0], args.a)]
    if args.png:
        os.makedirs(args.png, exist_ok=True)
        for stem, path in pairs:
            w, h, rgb = read(path)
            write_png(os.path.join(args.png, stem + ".png"), w, h, rgb)
        print("%d pictures written to %s" % (len(pairs), args.png))
    if not args.b:
        return 0
    failed = 0
    for stem, path in pairs:
        other = args.b
        if os.path.isdir(args.b):
            other = next((os.path.join(args.b, stem + ext) for ext in (".ppm", ".png", ".PPM", ".PNG")
                          if os.path.exists(os.path.join(args.b, stem + ext))), None)
            if other is None:
                print("FAIL %-28s not in %s" % (stem, args.b))
                failed += 1
                continue
        if not compare(stem, read(path), read(other), args):
            failed += 1
    print("%d of %d pictures within tolerance" % (len(pairs) - failed, len(pairs)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
