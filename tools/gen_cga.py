#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a sample 320x200 4-color raw interlaced CGA picture (.CGA) for hobbyOS.

import struct
import sys

def create_sample_cga(out_path):
    # 200 scanlines, 320 pixels each (values 0, 1, 2, 3)
    pixels = [[0 for _ in range(320)] for _ in range(200)]

    # Border (color 3)
    for x in range(320):
        pixels[0][x] = 3
        pixels[1][x] = 3
        pixels[198][x] = 3
        pixels[199][x] = 3
    for y in range(200):
        pixels[y][0] = 3
        pixels[y][1] = 3
        pixels[y][318] = 3
        pixels[y][319] = 3

    # Top color test bars (rows 4..30)
    for y in range(4, 30):
        for x in range(4, 316):
            pixels[y][x] = (x - 4) // 78  # 0, 1, 2, 3

    # Diagonal criss-cross lines in background (rows 35..165)
    for y in range(35, 165):
        for x in range(10, 310):
            if ((x + y) % 16 == 0) or ((x - y) % 16 == 0):
                pixels[y][x] = 1

    # Concentric boxes in center (rows 40..160, cols 40..280)
    for step in range(5):
        c = (step % 3) + 1
        x1 = 50 + step * 10
        x2 = 270 - step * 10
        y1 = 45 + step * 8
        y2 = 155 - step * 8
        for x in range(x1, x2 + 1):
            pixels[y1][x] = c
            pixels[y2][x] = c
        for y in range(y1, y2 + 1):
            pixels[y][x1] = c
            pixels[y][x2] = c

    # Simple 8x8 font rendering for text
    FONT = {
        'H': [0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00],
        'O': [0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00],
        'B': [0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00],
        'Y': [0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00],
        'S': [0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00],
        ' ': [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],
        '1': [0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00],
        '.': [0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00],
        '0': [0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00],
        'C': [0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00],
        'G': [0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00],
        'A': [0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00],
        'V': [0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00],
        'I': [0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00],
        'E': [0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00],
        'W': [0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00],
        'R': [0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00],
    }

    def draw_text(text, sx, sy, scale=2, color=3, bg=0):
        cur_x = sx
        for ch in text:
            glyph = FONT.get(ch, FONT[' '])
            for r in range(8):
                row_bits = glyph[r]
                for c in range(8):
                    pixel_on = (row_bits & (0x80 >> c)) != 0
                    val = color if pixel_on else bg
                    if val is not None:
                        for dy in range(scale):
                            for dx in range(scale):
                                py = sy + r * scale + dy
                                px = cur_x + c * scale + dx
                                if 0 <= px < 320 and 0 <= py < 200:
                                    pixels[py][px] = val
            cur_x += 8 * scale

    # Solid banner background for text
    for y in range(82, 118):
        for x in range(95, 225):
            pixels[y][x] = 0

    draw_text("HOBBYOS 1.0", 100, 85, scale=2, color=3, bg=0)
    draw_text("CGA VIEWER", 108, 103, scale=2, color=2, bg=0)

    # Convert 320x200 pixel matrix into CGA interleaved banks:
    # Bank 0: scanlines 0, 2, 4, ..., 198 (100 scanlines, 80 bytes each = 8000 bytes)
    # Bank 1: scanlines 1, 3, 5, ..., 199 (100 scanlines, 80 bytes each = 8000 bytes)
    bank0 = bytearray()
    bank1 = bytearray()

    for y in range(0, 200, 2):
        for x in range(0, 320, 4):
            b = ((pixels[y][x] & 3) << 6) | \
                ((pixels[y][x+1] & 3) << 4) | \
                ((pixels[y][x+2] & 3) << 2) | \
                (pixels[y][x+3] & 3)
            bank0.append(b)

    for y in range(1, 200, 2):
        for x in range(0, 320, 4):
            b = ((pixels[y][x] & 3) << 6) | \
                ((pixels[y][x+1] & 3) << 4) | \
                ((pixels[y][x+2] & 3) << 2) | \
                (pixels[y][x+3] & 3)
            bank1.append(b)

    # Standard raw 16,384-byte (16 KB) CGA VRAM dump:
    # 8,000 bytes Bank 0 + 192 bytes padding + 8,000 bytes Bank 1 + 192 bytes padding
    raw_data = bank0 + bytes(192) + bank1 + bytes(192)
    assert len(raw_data) == 16384, f"Unexpected length {len(raw_data)}"

    with open(out_path, 'wb') as f:
        f.write(raw_data)
    print(f"Generated {out_path} ({len(raw_data)} bytes)")

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'build/sample.cga'
    create_sample_cga(out)
