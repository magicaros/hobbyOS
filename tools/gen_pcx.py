#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a sample 320x200 256-color ZSoft PCX picture for hobbyOS.

import sys
import math

def generate_pcx(out_path):
    width = 320
    height = 200

    # 1. Generate 256-color Palette (256 RGB triplets, 0-255)
    palette = bytearray(768)

    # First 16 colors: standard 16 EGA/CGA colors
    std_16 = [
        (0, 0, 0),       (0, 0, 168),     (0, 168, 0),     (0, 168, 168),
        (168, 0, 0),     (168, 0, 168),   (168, 84, 0),    (168, 168, 168),
        (84, 84, 84),    (84, 84, 252),   (84, 252, 84),   (84, 252, 252),
        (252, 84, 84),   (252, 84, 252),  (252, 252, 84),  (252, 252, 252)
    ]
    for i in range(16):
        palette[i*3]     = std_16[i][0]
        palette[i*3 + 1] = std_16[i][1]
        palette[i*3 + 2] = std_16[i][2]

    # Colors 16..255: Smooth color wheels and gradients
    for i in range(16, 256):
        idx = i - 16
        # Generate smooth plasma/rainbow gradient
        r = int(127 + 127 * math.sin(idx * 0.08))
        g = int(127 + 127 * math.sin(idx * 0.08 + 2.094))
        b = int(127 + 127 * math.sin(idx * 0.08 + 4.188))
        palette[i*3]     = max(0, min(255, r))
        palette[i*3 + 1] = max(0, min(255, g))
        palette[i*3 + 2] = max(0, min(255, b))

    # 2. Generate Pixel Matrix (320 x 200)
    pixels = bytearray(width * height)

    # Background: Smooth circular plasma pattern
    for y in range(height):
        for x in range(width):
            dx = x - 160
            dy = (y - 100) * 1.6
            dist = int(math.sqrt(dx*dx + dy*dy))
            angle = int(math.atan2(dy, dx) * 180 / math.pi + 180)
            c = 16 + ((dist * 2 + angle // 3) % 240)
            pixels[y * width + x] = c

    # Draw a stylish banner box in center
    for y in range(75, 125):
        for x in range(50, 270):
            if y == 75 or y == 124 or x == 50 or x == 269:
                pixels[y * width + x] = 15  # White border
            elif y == 76 or y == 123 or x == 51 or x == 268:
                pixels[y * width + x] = 14  # Yellow inner border
            else:
                pixels[y * width + x] = 0   # Black interior

    # Simple 8x8 font rendering for text inside banner
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
        'V': [0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00],
        'G': [0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00],
        'A': [0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00],
        '2': [0x3C, 0x66, 0x06, 0x1C, 0x30, 0x60, 0x7E, 0x00],
        '5': [0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00],
        '6': [0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00],
        'C': [0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00],
        'L': [0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00],
        'R': [0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00],
        'P': [0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00],
        'X': [0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00],
    }

    def draw_text(text, sx, sy, scale=2, color=15):
        cur_x = sx
        for ch in text:
            glyph = FONT.get(ch, FONT[' '])
            for r in range(8):
                row_bits = glyph[r]
                for c in range(8):
                    if (row_bits & (0x80 >> c)) != 0:
                        for dy in range(scale):
                            for dx in range(scale):
                                py = sy + r * scale + dy
                                px = cur_x + c * scale + dx
                                if 0 <= px < width and 0 <= py < height:
                                    pixels[py * width + px] = color
            cur_x += 8 * scale

    draw_text("HOBBYOS 1.0", 72, 83, scale=2, color=14)
    draw_text("VGA 256C PCX", 64, 102, scale=2, color=11)

    # 3. Build PCX 128-byte Header
    header = bytearray(128)
    header[0] = 0x0A         # Manufacturer: ZSoft
    header[1] = 5            # Version: 3.0+ with 256-color palette
    header[2] = 1            # Encoding: 1 = RLE
    header[3] = 8            # Bits per pixel: 8
    # Window dimensions: 0, 0, 319, 199
    header[4] = 0; header[5] = 0
    header[6] = 0; header[7] = 0
    header[8] = (width - 1) & 0xFF
    header[9] = ((width - 1) >> 8) & 0xFF
    header[10] = (height - 1) & 0xFF
    header[11] = ((height - 1) >> 8) & 0xFF
    header[12] = 320 & 0xFF  # HRes
    header[13] = (320 >> 8) & 0xFF
    header[14] = 200 & 0xFF  # VRes
    header[15] = (200 >> 8) & 0xFF
    header[65] = 1           # Number of color planes
    header[66] = width & 0xFF # Bytes per line
    header[67] = (width >> 8) & 0xFF
    header[68] = 1           # Palette info: 1 = color/BW
    header[69] = 0

    # 4. RLE Encode Scanlines
    rle_payload = bytearray()
    for y in range(height):
        row = pixels[y * width : (y + 1) * width]
        x = 0
        while x < width:
            val = row[x]
            run = 1
            while x + run < width and row[x + run] == val and run < 63:
                run += 1
            if run > 1 or (val & 0xC0) == 0xC0:
                rle_payload.append(0xC0 | run)
                rle_payload.append(val)
            else:
                rle_payload.append(val)
            x += run

    # 5. Assemble final file: Header + RLE data + 0x0C marker + 768-byte palette
    out_data = header + rle_payload + b'\x0C' + palette
    with open(out_path, 'wb') as f:
        f.write(out_data)
    print(f"Generated PCX: {out_path} ({len(out_data)} bytes, compressed from {width*height} bytes)")

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'build/sample.pcx'
    generate_pcx(out)
