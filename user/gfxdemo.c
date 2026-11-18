/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GFXDEMO.BIN — Interactive CGA and VGA graphics demonstration program for hobbyOS.
 */

#include "hobbyos.h"

/* 64-entry fixed-point (scaled by 256) Sine table for 3D rotation */
static const int16_t sin_table[64] = {
      0,  25,  49,  74,  97, 120, 142, 162,
    181, 197, 212, 225, 236, 244, 250, 254,
    256, 254, 250, 244, 236, 225, 212, 197,
    181, 162, 142, 120,  97,  74,  49,  25,
      0, -25, -49, -74, -97,-120,-142,-162,
   -181,-197,-212,-225,-236,-244,-250,-254,
   -256,-254,-250,-244,-236,-225,-212,-197,
   -181,-162,-142,-120, -97, -74, -49, -25
};

static int16_t fx_sin(uint8_t angle) {
    return sin_table[angle & 63];
}

static int16_t fx_cos(uint8_t angle) {
    return sin_table[(angle + 16) & 63];
}

/* Fast 16-bit fixed-point multiplication (a * b) >> 8 via 8086 imul */
int16_t fx_mul(int16_t a, int16_t b);
#pragma aux fx_mul = \
    "imul dx" \
    "mov al, ah" \
    "mov ah, dl" \
    parm [ax] [dx] value [ax];

/* Fast 16-bit word clear for 16,000-byte linear backbuffer */
static void clear_buf_fast(uint8_t __far *buf);
#pragma aux clear_buf_fast = \
    "push di" \
    "xor ax, ax" \
    "mov cx, 8000" \
    "cld" \
    "rep stosw" \
    "pop di" \
    parm [es di] modify [ax cx];

/* 3D Cube Vertices (Coordinates -40 to +40) */
static const int16_t cube_v[8][3] = {
    { -36, -36, -36 },
    {  36, -36, -36 },
    {  36,  36, -36 },
    { -36,  36, -36 },
    { -36, -36,  36 },
    {  36, -36,  36 },
    {  36,  36,  36 },
    { -36,  36,  36 }
};

/* 12 Cube Edges */
static const uint8_t cube_e[12][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 0},
    {4, 5}, {5, 6}, {6, 7}, {7, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7}
};

static void buf_line_320(uint8_t __far *buf, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
    int16_t dx  = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int16_t sx  = (x0 < x1) ? 1 : -1;
    int16_t dy  = (y1 >= y0) ? -(y1 - y0) : -(y0 - y1);
    int16_t sy  = (y0 < y1) ? 1 : -1;
    int16_t err = dx + dy;

    for (;;) {
        os_buf_putpixel_320(buf, (uint16_t)x0, (uint16_t)y0, color);
        if (x0 == x1 && y0 == y1) break;
        {
            int16_t e2 = err << 1;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

static void wait_key_or_test(int auto_test) {
    if (auto_test) {
        uint16_t i;
        for (i = 0; i < 2; i++) {
            os_gfx_vsync();
            if (os_kbhit()) {
                os_getc();
                return;
            }
        }
    } else {
        /* Flush any keys pressed during drawing or transitions */
        while (os_kbhit()) {
            os_getc();
        }
        /* Wait indefinitely for a keypress */
        os_getc();
    }
}

/* Screen 1: CGA 320x200 4-Color Palettes & Geometric Primitives */
static void demo_cga_320(int auto_test) {
    int pal;
    os_gfx_set_mode(OS_MODE_CGA_320x200);

    for (pal = 0; pal < 3; pal++) {
        int i;
        os_gfx_set_palette((uint8_t)pal, 1, 0);
        os_gfx_clear(0);

        /* Draw title banner */
        os_gfx_text(16, 8, "hobbyOS CGA 320x200 4-Color", 3);
        if (pal == 0) {
            os_gfx_text(24, 20, "Palette 0: Green / Red / Brown", 1);
        } else if (pal == 1) {
            os_gfx_text(24, 20, "Palette 1: Cyan / Magenta / White", 2);
        } else {
            os_gfx_text(24, 20, "Palette 2: Cyan / Red / White", 1);
        }

        /* Draw concentric rectangles */
        for (i = 0; i < 6; i++) {
            uint8_t col = (uint8_t)((i % 3) + 1);
            os_gfx_rect((uint16_t)(80 + i * 10), (uint16_t)(40 + i * 8),
                        (uint16_t)(160 - i * 20), (uint16_t)(110 - i * 16),
                        col, 0);
        }

        /* Draw ray starburst from center */
        for (i = 0; i < 320; i += 32) {
            os_gfx_line(160, 95, (int16_t)i, 185, 2);
            os_gfx_line(160, 95, (int16_t)i, 35, 1);
        }

        os_gfx_text(40, 190, "Press any key to continue...", 3);
        wait_key_or_test(auto_test);
    }
}

/* Screen 2: CGA 640x200 2-Color High-Resolution Screen */
static void demo_cga_640(int auto_test) {
    int i;
    os_gfx_set_mode(OS_MODE_CGA_640x200);
    os_gfx_clear(0);

    os_gfx_text(80, 8, "hobbyOS CGA 640x200 High-Resolution Graphics", 1);
    os_gfx_text(160, 20, "Geometric Line Art & Mathematical Curves", 1);

    /* Draw border */
    os_gfx_rect(10, 32, 620, 150, 1, 0);

    /* Draw grid lines and curves */
    for (i = 0; i <= 64; i += 4) {
        int16_t s = fx_sin((uint8_t)i);
        int16_t c = fx_cos((uint8_t)i);
        int16_t x = 320 + fx_mul(c, 260);
        int16_t y = 107 + fx_mul(s, 60);
        os_gfx_line(320, 107, x, y, 1);
    }

    /* Moiré pattern */
    for (i = 20; i < 620; i += 15) {
        os_gfx_line(20, 40, (int16_t)i, 175, 1);
        os_gfx_line(620, 40, 640 - (int16_t)i, 175, 1);
    }

    os_gfx_text(200, 188, "Press any key to continue...", 1);
    wait_key_or_test(auto_test);
}

/* Screen 3: Double-Buffered 3D Rotating Cube with FPU & VSync */
static void demo_cube_3d(int auto_test) {
    uint16_t buf_seg;
    uint8_t __far *buf;
    uint8_t ang_x = 0;
    uint8_t ang_y = 0;
    uint16_t frame = 0;

    os_gfx_set_mode(OS_MODE_CGA_320x200);
    os_gfx_set_palette(1, 1, 0); /* Cyan/Magenta/White */

    /* Allocate 16,000-byte linear backbuffer */
    buf_seg = os_mem_alloc(OS_CGA_LINEAR_BUF_PARAS);
    if (buf_seg == 0) {
        return;
    }
    buf = (uint8_t __far *)(((unsigned long)buf_seg << 16) | 0);

    if (!auto_test) {
        while (os_kbhit()) {
            os_getc();
        }
    }

    for (;;) {
        int v;
        int16_t proj_x[8];
        int16_t proj_y[8];
        int e;

        /* Fast clear linear backbuffer to 0 */
        clear_buf_fast(buf);

        /* Project all 8 vertices */
        for (v = 0; v < 8; v++) {
            int16_t x0 = cube_v[v][0];
            int16_t y0 = cube_v[v][1];
            int16_t z0 = cube_v[v][2];

            /* Rotate Y */
            int16_t x1 = fx_mul(x0, fx_cos(ang_y)) - fx_mul(z0, fx_sin(ang_y));
            int16_t z1 = fx_mul(x0, fx_sin(ang_y)) + fx_mul(z0, fx_cos(ang_y));

            /* Rotate X */
            int16_t y2 = fx_mul(y0, fx_cos(ang_x)) - fx_mul(z1, fx_sin(ang_x));
            int16_t z2 = fx_mul(y0, fx_sin(ang_x)) + fx_mul(z1, fx_cos(ang_x));

            /* Perspective projection */
            int16_t dist = 140 + z2;
            if (dist < 10) dist = 10;
            proj_x[v] = 160 + (int16_t)((x1 * 130) / dist);
            proj_y[v] =  95 + (int16_t)((y2 * 130) / dist);
        }

        /* Draw 12 edges into backbuffer */
        for (e = 0; e < 12; e++) {
            uint8_t i0 = cube_e[e][0];
            uint8_t i1 = cube_e[e][1];
            uint8_t col = (uint8_t)((e % 3) + 1);
            buf_line_320(buf, proj_x[i0], proj_y[i0], proj_x[i1], proj_y[i1], col);
        }

        /* Wait for VSync and blit tear-free */
        os_gfx_vsync();
        os_gfx_blit(buf);

        ang_x += 1;
        ang_y += 2;

        if (auto_test) {
            frame++;
            if (frame >= 4) {
                break;
            }
        } else {
            if (os_kbhit()) {
                os_getc();
                break;
            }
        }
    }

    os_mem_free(buf_seg);
}

/* Screen 4: VGA Mode 12h (640x480 16-Color Planar Graphics) */
static void demo_vga_640x480(int auto_test) {
    int i;
    if (os_video_type() < VIDEO_TYPE_VGA) {
        return;
    }

    if (os_gfx_set_mode(OS_MODE_VGA_640x480) != 0) {
        return;
    }
    os_gfx_clear(0);

    os_gfx_text(160, 16, "hobbyOS VGA 640x480 16-Color Graphics (Mode 12h)", 15);
    os_gfx_text(200, 32, "Planar 4-Bitplane Hardware Acceleration", 11);

    /* Draw 16 color test bars */
    for (i = 0; i < 16; i++) {
        os_gfx_rect((uint16_t)(32 + i * 36), 64, 32, 28, (uint8_t)i, 1);
        os_gfx_rect((uint16_t)(32 + i * 36), 64, 32, 28, 15, 0);
    }

    /* Outer border */
    os_gfx_rect(20, 110, 600, 330, 14, 0);

    /* Geometric diamond pattern */
    for (i = 0; i < 160; i += 10) {
        uint8_t c = (uint8_t)((i / 10) % 15 + 1);
        os_gfx_line(320, (int16_t)(130 + i), (int16_t)(100 + i * 2), 275, c);
        os_gfx_line(320, (int16_t)(130 + i), (int16_t)(540 - i * 2), 275, c);
        os_gfx_line(320, (int16_t)(420 - i), (int16_t)(100 + i * 2), 275, c);
        os_gfx_line(320, (int16_t)(420 - i), (int16_t)(540 - i * 2), 275, c);
    }

    os_gfx_text(200, 456, "Press any key to continue...", 15);
    wait_key_or_test(auto_test);
}

/* Screen 5: VGA Mode 13h (320x200 256-Color Linear Graphics & DAC Palette) */
static void demo_vga_320x200_256(int auto_test) {
    int i, x, y;
    if (os_video_type() < VIDEO_TYPE_VGA) {
        return;
    }

    if (os_gfx_set_mode(OS_MODE_VGA_320x200_256) != 0) {
        return;
    }
    os_gfx_clear(0);

    /* Program smooth 256-color gradient DAC palette (Black -> Red -> Yellow -> White -> Cyan -> Blue) */
    for (i = 0; i < 64; i++) {
        os_gfx_set_dac((uint8_t)i, (uint8_t)i, 0, 0);                  /* Black to Red */
        os_gfx_set_dac((uint8_t)(64 + i), 63, (uint8_t)i, 0);          /* Red to Yellow */
        os_gfx_set_dac((uint8_t)(128 + i), 63, 63, (uint8_t)i);        /* Yellow to White */
        os_gfx_set_dac((uint8_t)(192 + i), (uint8_t)(63 - i), (uint8_t)(63 - i), 63); /* White to Blue */
    }

    os_gfx_text(24, 12, "hobbyOS VGA 320x200 256-Color (Mode 13h)", 127);
    os_gfx_text(48, 26, "256-Color DAC Gradient & Direct Linear VRAM", 95);

    /* Draw smooth 256-color gradient bars */
    for (x = 0; x < 256; x++) {
        for (y = 50; y < 90; y++) {
            os_gfx_putpixel((uint16_t)(32 + x), (uint16_t)y, (uint8_t)x);
        }
    }

    /* Draw concentric circular color rings */
    for (i = 10; i < 80; i += 2) {
        int a;
        uint8_t c = (uint8_t)(i * 3);
        for (a = 0; a < 64; a += 2) {
            int16_t px = 160 + fx_mul(fx_cos((uint8_t)a), (int16_t)i);
            int16_t py = 145 + fx_mul(fx_sin((uint8_t)a), (int16_t)(i >> 1));
            os_gfx_putpixel((uint16_t)px, (uint16_t)py, c);
        }
    }

    os_gfx_text(50, 186, "Press any key to complete demo...", 127);
    wait_key_or_test(auto_test);
}

int app_main(void) {
    const char *args = os_get_args();
    int auto_test = 0;

    if (args) {
        while (*args == ' ') args++;
        if (*args == '/' || *args == '-') {
            auto_test = 1;
        } else if (*args != '\0') {
            auto_test = 1;
        }
    }

    /* Run Graphics Demonstrations */
    demo_cga_320(auto_test);
    demo_cga_640(auto_test);
    demo_cube_3d(auto_test);
    demo_vga_640x480(auto_test);
    demo_vga_320x200_256(auto_test);

    /* Restore standard 80x25 text console */
    os_gfx_set_mode(OS_MODE_TEXT_80x25);
    os_puts("GFXDEMO completed successfully.\n");
    return 0;
}
