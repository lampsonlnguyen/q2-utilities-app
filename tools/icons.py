#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate Q2 Pod Utilities icons with standard-library-only supersampling."""
import argparse
import math
import pathlib
import struct
import zlib

S = 8
BLUE = (47, 112, 181, 255)
WHITE = (255, 255, 255, 255)


def rounded(x, y, x0, y0, x1, y1, r):
    dx = max(x0 + r - x, 0, x - x1 + r)
    dy = max(y0 + r - y, 0, y - y1 + r)
    return dx * dx + dy * dy <= r * r


def line(x, y, x0, y0, x1, y1, width):
    dx, dy = x1 - x0, y1 - y0
    t = max(0, min(1, ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy)))
    return (x - x0 - t * dx) ** 2 + (y - y0 - t * dy) ** 2 <= (width / 2) ** 2


def symbol(x, y):
    # Four compact tools, drawn as a single white silhouette with open counters.
    # Coordinates use a 92-unit square.
    if 0 <= x < 44 and 0 <= y < 44:
        # Clock ring and hands.
        d = math.hypot(x - 22, y - 22)
        return 16 <= d <= 21 or line(x, y, 22, 22, 22, 10, 4) or line(x, y, 22, 22, 31, 27, 4)
    if 48 <= x <= 92 and 0 <= y < 44:
        # Calculator, display and four keypad cutouts.
        if not rounded(x, y, 50, 1, 91, 43, 7):
            return False
        if rounded(x, y, 56, 7, 85, 17, 2):
            return False
        return not any(rounded(x, y, cx - 4, cy - 4, cx + 4, cy + 4, 2)
                       for cx in (61, 80) for cy in (25, 35))
    if 0 <= x < 44 and 48 <= y <= 92:
        # Calendar frame, two binding tabs and date dots.
        if rounded(x, y, 0, 53, 43, 92, 7):
            if y < 65:
                return True
            if not rounded(x, y, 5, 68, 38, 87, 2):
                return True
            return any((x - cx) ** 2 + (y - cy) ** 2 <= 2.8 ** 2
                       for cx in (13, 22, 31) for cy in (73, 82))
        return rounded(x, y, 8, 48, 14, 59, 3) or rounded(x, y, 29, 48, 35, 59, 3)
    if 48 <= x <= 92 and 48 <= y <= 92:
        # Ringtone waveform: five rounded vertical bars.
        return any(rounded(x, y, cx - 2.7, 70 - h / 2, cx + 2.7, 70 + h / 2, 2.7)
                   for cx, h in zip((51, 60, 70, 80, 89), (14, 28, 42, 28, 14)))
    return False


def png(size, pane):
    fit = 92 if pane else size * .74
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            accum = [0, 0, 0, 0]
            for sy in range(S):
                for sx in range(S):
                    px, py = x + (sx + .5) / S, y + (sy + .5) / S
                    u, v = (px - (size - fit) / 2) * 92 / fit, (py - (size - fit) / 2) * 92 / fit
                    if symbol(u, v):
                        c = WHITE
                    elif not pane and rounded(px, py, 0, 0, size, size, size * .22):
                        c = BLUE
                    else:
                        c = (255, 255, 255, 0)
                    for k in range(3):
                        accum[k] += c[k] * c[3]
                    accum[3] += c[3]
            alpha = (accum[3] + S * S // 2) // (S * S)
            rgb = [round(accum[k] / accum[3]) for k in range(3)] if accum[3] else [255] * 3
            rows.extend((*rgb, alpha))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path, nargs="?", default=pathlib.Path("."))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, size, pane in (("icon.png", 52, False), ("icon-40.png", 40, False), ("pane.png", 96, True)):
        (args.output / name).write_bytes(png(size, pane))


if __name__ == "__main__":
    main()
