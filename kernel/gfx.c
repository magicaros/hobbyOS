/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CGA Graphics Subsystem (320x200 4-color, 640x200 2-color, 80x25 text mode
 * restoration, palettes, VSync, drawing primitives, 8x8 BIOS ROM font text,
 * and linear-to-interleaved 16 KB backbuffer blitting).
 */

#include "kernel.h"

#define CGA_MODE_CTRL_PORT  0x03D8
#define CGA_COLOR_SEL_PORT  0x03D9
#define CGA_VRAM_SEG        0xB800
#define VGA_VRAM_SEG        0xA000
#define VGA_GFX_INDEX_PORT  0x03CE
#define VGA_GFX_DATA_PORT   0x03CF
#define VGA_DAC_WRITE_INDEX 0x03C8
#define VGA_DAC_DATA_PORT   0x03C9
#define BIOS_FONT_SEG       0xF000
#define BIOS_FONT_OFF       0xFA6E

static uint8_t cur_video_mode = OS_MODE_TEXT_80x25;

int gfx_set_mode(uint8_t mode) {
    if (video_get_type() == VIDEO_TYPE_MDA) {
        if (mode == OS_MODE_TEXT_80x25 || mode == 0x07) {
            bios_set_video_mode(0x07);
            cur_video_mode = 0x07;
            con_init();
            return 0;
        }
        return -1; /* MDA does not support CGA color graphics modes */
    }

    if (mode == OS_MODE_VGA_640x480 || mode == OS_MODE_VGA_320x200_256) {
        if (video_get_type() < VIDEO_TYPE_VGA) {
            return -1; /* Requires VGA */
        }
        bios_set_video_mode(mode);
        cur_video_mode = mode;
        gfx_clear(0);
        return 0;
    }

    if (mode != OS_MODE_TEXT_80x25 &&
        mode != OS_MODE_CGA_320x200 &&
        mode != OS_MODE_CGA_320x200_ALT &&
        mode != OS_MODE_CGA_640x200) {
        return -1;
    }

    bios_set_video_mode(mode);
    cur_video_mode = mode;

    if (mode == OS_MODE_TEXT_80x25) {
        con_init();
    } else if (mode == OS_MODE_CGA_320x200) {
        gfx_set_palette(1, 1, 0); /* Palette 1 (Cyan/Magenta/White), bright, black bg */
        gfx_clear(0);
    } else if (mode == OS_MODE_CGA_320x200_ALT) {
        gfx_set_palette(2, 1, 0); /* Palette 2 (Cyan/Red/White), bright, black bg */
        gfx_clear(0);
    } else if (mode == OS_MODE_CGA_640x200) {
        gfx_set_palette(0, 1, 0x0F); /* Bright white foreground on black */
        gfx_clear(0);
    }

    return 0;
}

uint8_t gfx_get_mode(void) {
    return cur_video_mode;
}

void gfx_set_palette(uint8_t palette, uint8_t intensity, uint8_t color) {
    uint8_t color_sel = (uint8_t)(color & 0x0F);

    if (cur_video_mode == OS_MODE_CGA_320x200 ||
        cur_video_mode == OS_MODE_CGA_320x200_ALT) {
        if (intensity) {
            color_sel |= 0x10;
        }
        if (palette == 1) {
            color_sel |= 0x20;
        }
        /* Palette 2 enables the CGA B/W burst bit (0x04) on Port 0x3D8 for Cyan/Red/White */
        if (palette == 2) {
            outb(CGA_MODE_CTRL_PORT, 0x0E);
            cur_video_mode = OS_MODE_CGA_320x200_ALT;
        } else {
            outb(CGA_MODE_CTRL_PORT, 0x0A);
            cur_video_mode = OS_MODE_CGA_320x200;
        }
    }

    outb(CGA_COLOR_SEL_PORT, color_sel);

    /* On EGA and VGA, emulate CGA palette swaps via Attribute Controller */
    if (video_get_type() >= VIDEO_TYPE_EGA) {
        uint8_t c0 = (uint8_t)(color & 0x0F);
        uint8_t c1, c2, c3;

        if (cur_video_mode == OS_MODE_CGA_640x200) {
            bios_set_palette_reg(0, 0);
            bios_set_palette_reg(1, c0 ? c0 : 15);
        } else if (cur_video_mode == OS_MODE_CGA_320x200 ||
                   cur_video_mode == OS_MODE_CGA_320x200_ALT) {
            if (palette == 0) {
                /* Green, Red, Brown (or bright equivalents) */
                c1 = intensity ? 10 : 2;  /* Light Green / Green */
                c2 = intensity ? 12 : 4;  /* Light Red / Red */
                c3 = intensity ? 14 : 6;  /* Yellow / Brown */
            } else if (palette == 1) {
                /* Cyan, Magenta, White */
                c1 = intensity ? 11 : 3;  /* Light Cyan / Cyan */
                c2 = intensity ? 13 : 5;  /* Light Magenta / Magenta */
                c3 = intensity ? 15 : 7;  /* High White / Light Gray */
            } else {
                /* Palette 2 (Mode 5): Cyan, Red, White */
                c1 = intensity ? 11 : 3;  /* Light Cyan / Cyan */
                c2 = intensity ? 12 : 4;  /* Light Red / Red */
                c3 = intensity ? 15 : 7;  /* High White / Light Gray */
            }

            bios_set_palette_reg(0, c0);
            bios_set_palette_reg(1, c1);
            bios_set_palette_reg(2, c2);
            bios_set_palette_reg(3, c3);
            bios_set_palette_reg(0x11, c0); /* Overscan / border */
        }

        /* Reset flip-flop and ensure PAS bit 5 is 1 (display unblanked) */
        inb(0x3DA);
        outb(0x3C0, 0x20);
    }
}

void gfx_vsync(void) {
    cga_wait_vsync();
}

void gfx_clear(uint8_t color) {
    volatile uint16_t __far *vram = (volatile uint16_t __far *)MK_FP(CGA_VRAM_SEG, 0x0000);
    uint8_t fill_byte = 0;
    uint16_t fill_word;
    uint16_t i;

    if (cur_video_mode == OS_MODE_TEXT_80x25) {
        con_clear(color ? color : ATTR_DEFAULT);
        return;
    }

    if (cur_video_mode == OS_MODE_VGA_320x200_256) {
        volatile uint16_t __far *vga_vram = (volatile uint16_t __far *)MK_FP(VGA_VRAM_SEG, 0x0000);
        fill_word = ((uint16_t)color << 8) | color;
        for (i = 0; i < 32000U; i++) {
            vga_vram[i] = fill_word;
        }
        return;
    }

    if (cur_video_mode == OS_MODE_VGA_640x480) {
        volatile uint16_t __far *vga_vram = (volatile uint16_t __far *)MK_FP(VGA_VRAM_SEG, 0x0000);
        outb(VGA_GFX_INDEX_PORT, 0x00); outb(VGA_GFX_DATA_PORT, (uint8_t)(color & 0x0F)); /* Set/Reset */
        outb(VGA_GFX_INDEX_PORT, 0x01); outb(VGA_GFX_DATA_PORT, 0x0F);                    /* Enable Set/Reset */
        outb(VGA_GFX_INDEX_PORT, 0x08); outb(VGA_GFX_DATA_PORT, 0xFF);                    /* Bit Mask */
        for (i = 0; i < 19200U; i++) {
            vga_vram[i] = 0xFFFF;
        }
        outb(VGA_GFX_INDEX_PORT, 0x01); outb(VGA_GFX_DATA_PORT, 0x00);                    /* Restore defaults */
        outb(VGA_GFX_INDEX_PORT, 0x00); outb(VGA_GFX_DATA_PORT, 0x00);
        return;
    }

    if (cur_video_mode == OS_MODE_CGA_640x200) {
        fill_byte = (color & 1) ? 0xFF : 0x00;
    } else {
        uint8_t c = (uint8_t)(color & 0x03);
        fill_byte = (uint8_t)((c << 6) | (c << 4) | (c << 2) | c);
    }
    fill_word = ((uint16_t)fill_byte << 8) | fill_byte;

    /* Fill Bank 0 (0x0000..0x1F3F = 4000 words) and Bank 1 (0x2000..0x3F3F = 4000 words) */
    for (i = 0; i < 4000; i++) {
        vram[i]          = fill_word;
        vram[0x1000 + i] = fill_word;
    }
}

void gfx_putpixel(uint16_t x, uint16_t y, uint8_t color) {
    volatile uint8_t __far *vram = (volatile uint8_t __far *)MK_FP(CGA_VRAM_SEG, 0x0000);
    uint16_t bank_off;
    uint16_t row_off;

    if (cur_video_mode == OS_MODE_VGA_320x200_256) {
        if (x >= 320 || y >= 200) {
            return;
        }
        *(volatile uint8_t __far *)MK_FP(VGA_VRAM_SEG, (y << 8) + (y << 6) + x) = color;
        return;
    }

    if (cur_video_mode == OS_MODE_VGA_640x480) {
        uint16_t off;
        uint8_t mask;
        uint8_t dummy;
        if (x >= 640 || y >= 480) {
            return;
        }
        off = (y << 6) + (y << 4) + (x >> 3); /* y * 80 + x / 8 */
        mask = (uint8_t)(0x80 >> (x & 7));
        outb(VGA_GFX_INDEX_PORT, 0x00); outb(VGA_GFX_DATA_PORT, (uint8_t)(color & 0x0F));
        outb(VGA_GFX_INDEX_PORT, 0x01); outb(VGA_GFX_DATA_PORT, 0x0F);
        outb(VGA_GFX_INDEX_PORT, 0x08); outb(VGA_GFX_DATA_PORT, mask);
        dummy = *(volatile uint8_t __far *)MK_FP(VGA_VRAM_SEG, off);
        *(volatile uint8_t __far *)MK_FP(VGA_VRAM_SEG, off) = 0xFF;
        outb(VGA_GFX_INDEX_PORT, 0x08); outb(VGA_GFX_DATA_PORT, 0xFF);
        outb(VGA_GFX_INDEX_PORT, 0x01); outb(VGA_GFX_DATA_PORT, 0x00);
        outb(VGA_GFX_INDEX_PORT, 0x00); outb(VGA_GFX_DATA_PORT, 0x00);
        return;
    }

    if (y >= 200) {
        return;
    }

    bank_off = (y & 1) ? 0x2000 : 0x0000;
    row_off  = ((y >> 1) << 6) + ((y >> 1) << 4); /* (y / 2) * 80 */

    if (cur_video_mode == OS_MODE_CGA_320x200 ||
        cur_video_mode == OS_MODE_CGA_320x200_ALT) {
        uint16_t off;
        uint8_t  shift;
        if (x >= 320) {
            return;
        }
        off   = bank_off + row_off + (x >> 2);
        shift = (uint8_t)((3 - (x & 3)) << 1);
        vram[off] = (uint8_t)((vram[off] & ~(0x03 << shift)) | ((color & 0x03) << shift));
    } else if (cur_video_mode == OS_MODE_CGA_640x200) {
        uint16_t off;
        uint8_t  mask;
        if (x >= 640) {
            return;
        }
        off  = bank_off + row_off + (x >> 3);
        mask = (uint8_t)(0x80 >> (x & 7));
        if (color & 1) {
            vram[off] |= mask;
        } else {
            vram[off] &= (uint8_t)~mask;
        }
    }
}

void gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
    int16_t dx  = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int16_t sx  = (x0 < x1) ? 1 : -1;
    int16_t dy  = (y1 >= y0) ? -(y1 - y0) : -(y0 - y1);
    int16_t sy  = (y0 < y1) ? 1 : -1;
    int16_t err = dx + dy;

    for (;;) {
        if (x0 >= 0 && y0 >= 0) {
            gfx_putpixel((uint16_t)x0, (uint16_t)y0, color);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int16_t e2 = err << 1;
            if (e2 >= dy) {
                err += dy;
                x0  += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0  += sy;
            }
        }
    }
}

void gfx_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color, uint8_t filled) {
    uint16_t r, c;

    if (w == 0 || h == 0) {
        return;
    }

    if (filled) {
        for (r = 0; r < h; r++) {
            for (c = 0; c < w; c++) {
                gfx_putpixel(x + c, y + r, color);
            }
        }
    } else {
        for (c = 0; c < w; c++) {
            gfx_putpixel(x + c, y, color);
            gfx_putpixel(x + c, y + h - 1, color);
        }
        for (r = 0; r < h; r++) {
            gfx_putpixel(x, y + r, color);
            gfx_putpixel(x + w - 1, y + r, color);
        }
    }
}

void gfx_draw_text(uint16_t x, uint16_t y, const char __far *str, uint8_t color) {
    const uint8_t __far *rom_font = (const uint8_t __far *)MK_FP(BIOS_FONT_SEG, BIOS_FONT_OFF);
    uint16_t cur_x = x;
    uint16_t cur_y = y;

    if (!str) {
        return;
    }

    while (*str) {
        uint8_t ch = (uint8_t)(*str++);
        uint16_t row, col;

        if (ch == '\n') {
            cur_x = x;
            cur_y += 8;
            continue;
        }
        if (ch == '\r') {
            cur_x = x;
            continue;
        }

        if (ch < 128) {
            const uint8_t __far *glyph = &rom_font[(uint16_t)ch << 3];
            for (row = 0; row < 8; row++) {
                uint8_t bits = glyph[row];
                for (col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        gfx_putpixel(cur_x + col, cur_y + row, color);
                    }
                }
            }
        }
        cur_x += 8;
    }
}

void gfx_blit(const void __far *linear_buf) {
    if (!linear_buf || cur_video_mode == OS_MODE_TEXT_80x25) {
        return;
    }
    if (cur_video_mode == OS_MODE_VGA_320x200_256) {
        vga_blit_linear(linear_buf);
    } else {
        cga_blit_linear(linear_buf);
    }
}

void gfx_set_dac(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (video_get_type() >= VIDEO_TYPE_VGA) {
        outb(VGA_DAC_WRITE_INDEX, index);
        outb(VGA_DAC_DATA_PORT, (uint8_t)(r & 0x3F));
        outb(VGA_DAC_DATA_PORT, (uint8_t)(g & 0x3F));
        outb(VGA_DAC_DATA_PORT, (uint8_t)(b & 0x3F));
    }
}
