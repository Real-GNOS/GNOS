#!/usr/bin/env python3
import struct
import sys
import zlib

W_DEF, H_DEF = 1024, 768

BLOBS = [
    (0.18, 0.16, 0.60, 0.55),
    (0.86, 0.22, 0.52, 0.45),
    (0.70, 0.78, 0.66, 0.40),
    (0.24, 0.86, 0.58, 0.42),
    (0.50, 0.48, 0.44, 0.55),
]

PALETTES = [
    [(168, 205, 245), (206, 226, 250), (196, 226, 226), (216, 214, 244),
     (232, 240, 252)],
    [(198, 214, 240), (226, 208, 238), (238, 214, 206), (206, 226, 232),
     (240, 236, 246)],
    [(180, 220, 214), (206, 232, 210), (238, 232, 200), (222, 212, 240),
     (234, 244, 246)],
]


def lerp(a, b, t):
    return a + (b - a) * t


def vignette(nx, ny):
    dx = nx - 0.5
    dy = ny - 0.5
    r = (dx * dx + dy * dy) * 2.0
    return 1.0 - 0.10 * min(1.0, max(0.0, r - 0.12) / 0.88)


def shade(nx, ny, base, palette):
    r, g, b = base
    for (cx, cy, rad, amt), col in zip(BLOBS, palette):
        dx = nx - cx
        dy = (ny - cy) * 1.15
        d = (dx * dx + dy * dy) / (rad * rad)
        if d >= 1.0:
            continue
        w = (1.0 - d) ** 2 * amt
        r = lerp(r, col[0], w)
        g = lerp(g, col[1], w)
        b = lerp(b, col[2], w)
    stripe = ((nx * 1024 + ny * 768) % 64)
    adj = 1.0 + (0.0022 if stripe < 32 else -0.0022)
    v = vignette(nx, ny)
    return (int(min(255.0, max(0.0, r * adj * v))),
            int(min(255.0, max(0.0, g * adj * v))),
            int(min(255.0, max(0.0, b * adj * v))))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def encode_rgb(rows, w, h):
    raw = b""
    prev = bytes(w * 3)
    for row in rows:
        f = bytearray(w * 3)
        for i in range(w * 3):
            a = row[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            f[i] = (row[i] - paeth(a, b, c)) & 0xFF
        raw += b"\x04" + bytes(f)
        prev = bytes(row)
    comp = zlib.compress(raw, 9)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", comp) + chunk(b"IEND", b""))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "wallpaper.png"
    w = int(sys.argv[2]) if len(sys.argv) > 2 else W_DEF
    h = int(sys.argv[3]) if len(sys.argv) > 3 else H_DEF
    palette = PALETTES[(int(sys.argv[4]) if len(sys.argv) > 4 else 0) %
                       len(PALETTES)]
    top = (243.0, 247.0, 252.0)
    bot = (226.0, 236.0, 248.0)
    rows = []
    for y in range(h):
        ny = y / (h - 1)
        base = (lerp(top[0], bot[0], ny), lerp(top[1], bot[1], ny),
                lerp(top[2], bot[2], ny))
        row = bytearray()
        for x in range(w):
            r, g, b = shade(x / (w - 1), ny, base, palette)
            row += bytes((r, g, b))
        rows.append(row)
    png = encode_rgb(rows, w, h)
    with open(out, "wb") as f:
        f.write(png)
    print(f"{out}: {w}x{h} {len(png)} bytes")


if __name__ == "__main__":
    main()
