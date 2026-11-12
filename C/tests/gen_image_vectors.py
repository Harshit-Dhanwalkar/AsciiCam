#!/usr/bin/env python3
"""Writes tests/image_vectors.inc: PNG files for test_image.c with RGB every
decoder must produce
Own PNG writer (zlib only for data stream) go every colour type, bit depth
and filter type is covered
Run: python3 tests/gen_image_vectors.py > tests/image_vectors.inc"""

import struct
import zlib


def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filt(ft, row, prev, bpp):
    out = bytearray()
    for i, v in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][ft]
        out.append((v - pred) & 255)
    return bytes([ft]) + bytes(out)


def pack_samples(samples, depth):
    if depth >= 8:
        if depth == 8:
            return bytes(samples)
        return b"".join(struct.pack(">H", s) for s in samples)
    bits = "".join(format(s, "0%db" % depth) for s in samples)
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
         (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]

def png(w, h, ctype, depth, rows, pal=None, level=6, filters=(0, 1, 2, 3, 4),
        interlace=0):
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, ch * depth // 8)
    raw = b""
    if interlace:
        # each pass is its own little image with its own filtering
        for x0, y0, dx, dy in ADAM7:
            sub = [[v for x in range(x0, w, dx) for v in r[x * ch:(x + 1) * ch]]
                   for r in rows[y0::dy]]
            if not sub or not sub[0]:
                continue
            prev = bytes(len(pack_samples(sub[0], depth)))
            for y, r in enumerate(sub):
                packed = pack_samples(r, depth)
                raw += filt(filters[y % len(filters)], packed, prev, bpp)
                prev = packed
    else:
        prev = bytes(len(pack_samples(rows[0], depth)))
        for y, r in enumerate(rows):
            packed = pack_samples(r, depth)
            raw += filt(filters[y % len(filters)], packed, prev, bpp)
            prev = packed
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))
    if pal:
        out += chunk(b"PLTE", bytes(v for c in pal for v in c))
    z = zlib.compress(raw, level)
    half = len(z) // 2  # two IDAT chunks: must be concatenated
    out += chunk(b"IDAT", z[:half]) + chunk(b"IDAT", z[half:])
    out += chunk(b"IEND", b"")
    return out


def s8(v, depth):
    return v if depth == 8 else (v >> 8 if depth == 16 else v * 255 // ((1 << depth) - 1))


def over_black(c, a):
    return (c * a + 127) // 255


def carray(name, data):
    body = ",".join(str(b) for b in data)
    return "static const uint8_t %s[] = {%s};\n" % (name, body)


vectors = []  # (name, png bytes, w, h, expected rgb bytes)


def add(name, w, h, ctype, depth, pixels, pal=None, **kw):
    """pixels: rows of per-pixel tuples of raw samples"""
    rows, exp = [], []
    for r in pixels:
        flat = []
        for px in r:
            flat += list(px)
            if ctype == 3:
                exp += list(pal[px[0]])
            elif ctype in (0, 4):
                g = s8(px[0], depth)
                a = s8(px[1], depth) if ctype == 4 else 255
                exp += [over_black(g, a)] * 3
            else:
                v = [s8(c, depth) for c in px]
                a = v[3] if ctype == 6 else 255
                exp += [over_black(c, a) for c in v[:3]]
        rows.append(flat)
    vectors.append((name, png(w, h, ctype, depth, rows, pal, **kw), w, h, bytes(exp)))


W, H = 7, 6
for depth in (1, 2, 4, 8, 16):
    mx = (1 << depth) - 1
    add("gray%d" % depth, W, H, 0, depth,
        [[((x * 5 + y * 3) % (mx + 1),) for x in range(W)] for y in range(H)])
add("gray_alpha8", W, H, 4, 8,
    [[((x * 37 + y) % 256, (x * 40 + 30) % 256) for x in range(W)] for y in range(H)])
add("gray_alpha16", W, H, 4, 16,
    [[((x * 9000 + y * 100) % 65536, (x * 9100 + 3000) % 65536) for x in range(W)] for y in range(H)])
add("rgb8", W, H, 2, 8,
    [[((x * 36) % 256, (y * 50) % 256, (x * y * 7) % 256) for x in range(W)] for y in range(H)])
add("rgb16", W, H, 2, 16,
    [[((x * 9000) % 65536, (y * 11000) % 65536, (x * y * 1300) % 65536) for x in range(W)] for y in range(H)])
add("rgba8", W, H, 6, 8,
    [[((x * 36) % 256, (y * 50) % 256, 200, (x * 40 + y * 9) % 256) for x in range(W)] for y in range(H)])
add("rgba16", W, H, 6, 16,
    [[((x * 9000) % 65536, 60000, (y * 11000) % 65536, (x * 9500 + y * 700) % 65536) for x in range(W)] for y in range(H)])
for depth in (1, 2, 4, 8):
    n = min(1 << depth, 16)
    pal = [((i * 53 + 10) % 256, (i * 97 + 5) % 256, (i * 29 + 200) % 256) for i in range(n)]
    add("pal%d" % depth, W, H, 3, depth,
        [[((x + y * 2) % n,) for x in range(W)] for y in range(H)], pal=pal)
# One filter type at a time, so a broken filter is named
for ft in range(5):
    add("filter%d" % ft, W, H, 2, 8,
        [[((x * 31 + y * 7) % 256, (x * y * 11) % 256, (x + y * 40) % 256) for x in range(W)] for y in range(H)],
        filters=(ft,))
# Noise through the Paeth and Average filters: ties between the three predictors
# and rounding of the average show up there
import random

rnd = random.Random(7)
for ft in (3, 4):
    add("noise_f%d" % ft, 16, 16, 2, 8,
        [[tuple(rnd.randrange(256) for _ in range(3)) for x in range(16)] for y in range(16)],
        filters=(ft,))
# Compression: stored, fixed Huffman (tiny input), dynamic, long runs
add("stored", W, H, 2, 8, [[(x * 30, y * 40, 7) for x in range(W)] for y in range(H)], level=0)
add("fixed", 2, 2, 0, 8, [[(10,), (200,)], [(90,), (255,)]], level=9)
add("onepixel", 1, 1, 2, 8, [[(1, 2, 3)]])
add("runs", 40, 3, 0, 8, [[(0,) for x in range(40)], [(255,) for x in range(40)], [(128,) for x in range(40)]], level=9)

# Adam7: sizes where some passes are empty (1x1, 3x2, 5x1) and where all seven
# exist (9x9, 13x11), for every colour type and bit depth
for (iw, ih) in ((1, 1), (3, 2), (5, 1), (9, 9), (13, 11)):
    tag = "%dx%d" % (iw, ih)
    for depth in (1, 2, 4, 8, 16):
        mx = (1 << depth) - 1
        add("a7_gray%d_%s" % (depth, tag), iw, ih, 0, depth,
            [[((x * 5 + y * 3) % (mx + 1),) for x in range(iw)] for y in range(ih)], interlace=1)
    add("a7_rgb8_" + tag, iw, ih, 2, 8,
        [[((x * 36 + 3) % 256, (y * 50) % 256, (x * y * 7) % 256) for x in range(iw)] for y in range(ih)], interlace=1)
    add("a7_rgb16_" + tag, iw, ih, 2, 16,
        [[((x * 9000) % 65536, (y * 11000) % 65536, (x * y * 1300) % 65536) for x in range(iw)] for y in range(ih)], interlace=1)
    add("a7_rgba8_" + tag, iw, ih, 6, 8,
        [[((x * 36) % 256, (y * 50) % 256, 200, (x * 40 + y * 9 + 20) % 256) for x in range(iw)] for y in range(ih)], interlace=1)
    add("a7_ga8_" + tag, iw, ih, 4, 8,
        [[((x * 37 + y) % 256, (x * 40 + 30) % 256) for x in range(iw)] for y in range(ih)], interlace=1)
    for depth in (1, 2, 4, 8):
        n = min(1 << depth, 16)
        pal = [((i * 53 + 10) % 256, (i * 97 + 5) % 256, (i * 29 + 200) % 256) for i in range(n)]
        add("a7_pal%d_%s" % (depth, tag), iw, ih, 3, depth,
            [[((x + y * 2) % n,) for x in range(iw)] for y in range(ih)], pal=pal, interlace=1)
# Same picture with and without interlacing must decode identically
add("a7_noise", 21, 17, 2, 8,
    [[tuple(rnd.randrange(256) for _ in range(3)) for x in range(21)] for y in range(17)],
    filters=(4, 3, 2, 1, 0), interlace=1)


# Big one is a formula in test_image.c too: 48 x 32 smooth RGB, dynamic blocks
BW, BH = 48, 32


def bigpx(x, y):
    return ((x * 5 + y) % 256, (y * 7 + x * x // 8) % 256, (x * y // 3) % 256)


vectors.append(("big_l1", png(BW, BH, 2, 8, [sum(([c for c in bigpx(x, y)] for x in range(BW)), []) for y in range(BH)], level=1),
                BW, BH, b""))
vectors.append(("big_l9", png(BW, BH, 2, 8, [sum(([c for c in bigpx(x, y)] for x in range(BW)), []) for y in range(BH)], level=9),
                BW, BH, b""))

print("/* Generated by tests/gen_image_vectors.py, do not edit */")
for name, data, w, h, exp in vectors:
    print(carray("png_" + name, data), end="")
    if exp:
        print(carray("rgb_" + name, exp), end="")
print("static const vec_t vectors[] = {")
for name, data, w, h, exp in vectors:
    print('    {"%s", png_%s, sizeof(png_%s), %d, %d, %s},' % (name, name, name, w, h, ("rgb_" + name) if exp else "NULL"))
print("};")

