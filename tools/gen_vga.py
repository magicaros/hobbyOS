#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a sample 320x200 256-color raw VGA dump (.VGA) for hobbyOS.
# Layout: 64,000 bytes pixels + 768 bytes VGA DAC palette (0-63 scale) = 64,768 bytes.

import sys
import math

def generate_vga(out_path):
    width = 320
    height = 200

    # 1. Palette: 256 colors * 3 bytes (R, G, B in 0-63 range for VGA DAC)
    palette = bytearray(768)
    for i in range(256):
        # 6-bit DAC values (0..63)
        r = int(31.5 + 31.5 * math.sin(i * 0.05))
        g = int(31.5 + 31.5 * math.sin(i * 0.05 + 2.094))
        b = int(31.5 + 31.5 * math.sin(i * 0.05 + 4.188))
        palette[i*3]     = max(0, min(63, r))
        palette[i*3 + 1] = max(0, min(63, g))
        palette[i*3 + 2] = max(0, min(63, b))

    # 2. Pixels: 320 x 200
    pixels = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            c = (x + y * 2) % 256
            pixels[y * width + x] = c

    # Write file: 64,000 bytes pixels followed by 768 bytes palette
    with open(out_path, 'wb') as f:
        f.write(pixels + palette)
    print(f"Generated VGA: {out_path} ({len(pixels) + len(palette)} bytes)")

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'build/sample.vga'
    generate_vga(out)
