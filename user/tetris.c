/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * hobbyTris - Authentic CGA Mode 4 Tetris for hobbyOS
 * Features direct CGA VRAM rendering for 4.77 MHz 8088 performance,
 * hardware BDA timer synchronization (18.2 Hz), dynamic low-intensity
 * palette swapping per level (Palettes 1, 0, 2), background OPL/IMF music,
 * sound effects, 7-bag piece generation, and persistent high scores.
 */

#include "../include/hobbyos.h"

/* --------------------------------------------------------------------------
 * Constants & Definitions
 * -------------------------------------------------------------------------- */

#define GRID_W          10
#define GRID_H          20
#define BLOCK_SIZE      8

#define FIELD_X         28
#define FIELD_Y         18
#define FIELD_PX_W      (GRID_W * BLOCK_SIZE)
#define FIELD_PX_H      (GRID_H * BLOCK_SIZE)

/* BIOS Scancodes */
#define SC_UP           0x48
#define SC_LEFT         0x4B
#define SC_RIGHT        0x4D
#define SC_DOWN         0x50

#define KEY_ESC         0x1B
#define KEY_ENTER       0x0D
#define KEY_SPACE       0x20

#define CGA_VRAM_SEG    0xB800
#define BIOS_FONT_SEG   0xF000
#define BIOS_FONT_OFF   0xFA6E

#define FP_HERE(p)      ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(p)))
#define GET_TICKS()     (*((volatile uint16_t __far *)MK_FP(0x0040, 0x006C)))

/* --------------------------------------------------------------------------
 * Tetrominoes (7 Pieces, 4 Rotations, 4 Blocks)
 * -------------------------------------------------------------------------- */

typedef struct {
    int8_t x, y;
} point_t;

static const point_t PIECES[7][4][4] = {
    /* 0: I piece */
    { { {0,1},{1,1},{2,1},{3,1} }, { {2,0},{2,1},{2,2},{2,3} }, { {0,2},{1,2},{2,2},{3,2} }, { {1,0},{1,1},{1,2},{1,3} } },
    /* 1: J piece */
    { { {0,0},{0,1},{1,1},{2,1} }, { {1,0},{2,0},{1,1},{1,2} }, { {0,1},{1,1},{2,1},{2,2} }, { {1,0},{1,1},{0,2},{1,2} } },
    /* 2: L piece */
    { { {2,0},{0,1},{1,1},{2,1} }, { {1,0},{1,1},{1,2},{2,2} }, { {0,1},{1,1},{2,1},{0,2} }, { {0,0},{1,0},{1,1},{1,2} } },
    /* 3: O piece */
    { { {1,0},{2,0},{1,1},{2,1} }, { {1,0},{2,0},{1,1},{2,1} }, { {1,0},{2,0},{1,1},{2,1} }, { {1,0},{2,0},{1,1},{2,1} } },
    /* 4: S piece */
    { { {1,0},{2,0},{0,1},{1,1} }, { {1,0},{1,1},{2,1},{2,2} }, { {1,1},{2,1},{0,2},{1,2} }, { {0,0},{0,1},{1,1},{1,2} } },
    /* 5: T piece */
    { { {1,0},{0,1},{1,1},{2,1} }, { {1,0},{1,1},{2,1},{1,2} }, { {0,1},{1,1},{2,1},{1,2} }, { {1,0},{0,1},{1,1},{1,2} } },
    /* 6: Z piece */
    { { {0,0},{1,0},{1,1},{2,1} }, { {2,0},{1,1},{2,1},{1,2} }, { {0,1},{1,1},{1,2},{2,2} }, { {1,0},{0,1},{1,1},{0,2} } }
};

/* Color mapped to each tetromino (1, 2, or 3) */
static const uint8_t PIECE_COLOR[7] = { 1, 1, 2, 3, 1, 2, 2 };

/* Expands a 4-bit font nibble into a 4-pixel (8-bit) 2bpp CGA mask */
static const uint8_t CGA_NIBBLE_MASK[16] = {
    0x00, 0x03, 0x0C, 0x0F,
    0x30, 0x33, 0x3C, 0x3F,
    0xC0, 0xC3, 0xCC, 0xCF,
    0xF0, 0xF3, 0xFC, 0xFF
};

static const uint8_t CGA_COLOR_FILL[4] = { 0x00, 0x55, 0xAA, 0xFF };

/* --------------------------------------------------------------------------
 * Game State
 * -------------------------------------------------------------------------- */

static uint8_t  s_grid[GRID_H][GRID_W];

static int8_t   s_cur_type;
static int8_t   s_cur_rot;
static int8_t   s_cur_x;
static int8_t   s_cur_y;

static int8_t   s_next_type;

static uint32_t s_score;
static uint32_t s_high_score;
static uint16_t s_lines;
static uint16_t s_level;

static uint8_t  s_bag[7];
static uint8_t  s_bag_idx = 7;
static uint16_t s_prng_seed = 0x1234;

static uint8_t  s_sound_on = 1;
static uint16_t s_imf_seg  = 0;
static uint16_t s_imf_size = 0;

static uint16_t s_sfx_timer = 0;

/* --------------------------------------------------------------------------
 * Freestanding 16-bit Math Primitives (No Watcom 32-bit Division Helpers)
 * -------------------------------------------------------------------------- */

static uint32_t mul16(uint16_t a, uint16_t b);
#pragma aux mul16 = \
    "mul dx" \
    parm [ax] [dx] value [dx ax];

static uint16_t u32_div10(uint32_t *val) {
    uint16_t hi = (uint16_t)(*val >> 16);
    uint16_t lo = (uint16_t)(*val & 0xFFFF);
    uint16_t q_hi = hi / 10;
    uint16_t r_hi = hi % 10;
    uint16_t d_hi = (r_hi << 4) + (lo >> 12);
    uint16_t q_mid = d_hi / 10;
    uint16_t r_mid = d_hi % 10;
    uint16_t d_lo = (r_mid << 12) + (lo & 0x0FFF);
    uint16_t q_lo = d_lo / 10;
    uint16_t rem  = d_lo % 10;
    *val = ((uint32_t)q_hi << 16) | ((uint32_t)(q_mid << 12) + q_lo);
    return rem;
}

static uint16_t rand16(void) {
    s_prng_seed = (uint16_t)(s_prng_seed * 25173u + 13849u);
    return s_prng_seed;
}

static uint8_t next_bag_piece(void) {
    if (s_bag_idx >= 7) {
        uint8_t i, j, tmp;
        for (i = 0; i < 7; i++) s_bag[i] = i;
        /* Fisher-Yates shuffle */
        for (i = 6; i > 0; i--) {
            j = (uint8_t)(rand16() % (i + 1));
            tmp = s_bag[i]; s_bag[i] = s_bag[j]; s_bag[j] = tmp;
        }
        s_bag_idx = 0;
    }
    return s_bag[s_bag_idx++];
}

static void u32_to_str(uint32_t val, char *buf) {
    char tmp[12];
    int tp = 0;
    int bp = 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (val > 0) {
        tmp[tp++] = (char)('0' + u32_div10(&val));
    }
    while (tp > 0) {
        buf[bp++] = tmp[--tp];
    }
    buf[bp] = '\0';
}

static void u16_to_str(uint16_t val, char *buf) {
    uint32_t v = (uint32_t)val;
    u32_to_str(v, buf);
}

/* --------------------------------------------------------------------------
 * Audio Management (Tick-Synchronized SFX + OPL Channel 2 Key-Off)
 * -------------------------------------------------------------------------- */

static void sfx_play(uint16_t freq, uint16_t ticks) {
    if (!s_sound_on) return;
    os_sound_tone(freq, 0, 2); /* Channel 2 for non-blocking SFX */
    s_sfx_timer = ticks;
}

static void sfx_update_tick(void) {
    if (s_sfx_timer > 0) {
        s_sfx_timer--;
        if (s_sfx_timer == 0) {
            /* Explicitly key-off OPL channel 2 (0xB2) because os_sound_stop()
             * preserves OPL channels while background IMF music is active.
             */
            os_sound_opl_write(0xB2, 0x00);
            os_sound_tone(0, 0, 2);
        }
    }
}

static void init_music(void) {
    long sz = os_file_size("/MUSIC/TETRIS.IMF");
    if (sz <= 0 || sz > 32000L) {
        sz = os_file_size("SYS:MUSIC/TETRIS.IMF");
    }
    if (sz > 0 && sz <= 32000L) {
        uint16_t sz16 = (uint16_t)sz;
        uint16_t paras = (uint16_t)((sz16 + 15) >> 4);
        s_imf_seg = os_mem_alloc(paras);
        if (s_imf_seg != 0) {
            s_imf_size = sz16;
            if (os_file_read("/MUSIC/TETRIS.IMF", (void __far *)MK_FP(s_imf_seg, 0), s_imf_size) == 0) {
                os_file_read("SYS:MUSIC/TETRIS.IMF", (void __far *)MK_FP(s_imf_seg, 0), s_imf_size);
            }
        }
    }
}

static void start_bg_music(void) {
    if (s_sound_on && s_imf_seg != 0 && s_imf_size > 0) {
        os_sound_bg_play(560, 1, s_imf_size, (const void __far *)MK_FP(s_imf_seg, 0), "TETRIS");
    }
}

static void toggle_sound(void) {
    s_sound_on = !s_sound_on;
    if (s_sound_on) {
        start_bg_music();
    } else {
        os_sound_bg_stop();
        os_sound_opl_write(0xB2, 0x00);
        os_sound_stop();
    }
}

static void stop_all_audio(void) {
    os_sound_bg_stop();
    os_sound_opl_write(0xB2, 0x00);
    os_sound_stop();
    if (s_imf_seg != 0) {
        os_mem_free(s_imf_seg);
        s_imf_seg = 0;
    }
}

/* --------------------------------------------------------------------------
 * High Score Persistence
 * -------------------------------------------------------------------------- */

static void load_high_score(void) {
    uint32_t val = 0;
    s_high_score = 5000; /* Default classic benchmark */
    if (os_file_read("/GAMES/TETRIS.DAT", (void __far *)FP_HERE(&val), sizeof(val)) == sizeof(val)) {
        if (val > 0 && val < 10000000UL) {
            s_high_score = val;
        }
    } else if (os_file_read("SYS:GAMES/TETRIS.DAT", (void __far *)FP_HERE(&val), sizeof(val)) == sizeof(val)) {
        if (val > 0 && val < 10000000UL) {
            s_high_score = val;
        }
    }
}

static void save_high_score(void) {
    if (s_score > s_high_score) {
        s_high_score = s_score;
    }
    os_file_write("/GAMES/TETRIS.DAT", (const void __far *)FP_HERE(&s_high_score), sizeof(s_high_score));
}

/* --------------------------------------------------------------------------
 * Fast Direct CGA Mode 4 VRAM Rendering Primitives (Zero INT 0x21 Overhead)
 * -------------------------------------------------------------------------- */

static __inline uint16_t cga_row_offset(uint16_t y) {
    uint16_t half = y >> 1;
    uint16_t off  = (half << 6) + (half << 4); /* (y / 2) * 80 */
    if (y & 1) off += 0x2000;
    return off;
}

static void cga_fill_rect_fast(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    volatile uint8_t __far *vram = (volatile uint8_t __far *)MK_FP(CGA_VRAM_SEG, 0);
    uint8_t fill = CGA_COLOR_FILL[color & 3];
    int16_t r, c;

    if (w <= 0 || h <= 0) return;

    for (r = 0; r < h; r++) {
        int16_t py = y + r;
        uint16_t row_off;
        int16_t x0 = x;
        int16_t x1 = x + w - 1;
        int16_t b0, b1;

        if ((uint16_t)py >= 200) continue;
        row_off = cga_row_offset((uint16_t)py);

        b0 = x0 >> 2;
        b1 = x1 >> 2;

        if (b0 == b1) {
            uint8_t mask = 0;
            for (c = (x0 & 3); c <= (x1 & 3); c++) {
                mask |= (uint8_t)(0xC0 >> (c << 1));
            }
            vram[row_off + b0] = (uint8_t)((vram[row_off + b0] & ~mask) | (fill & mask));
        } else {
            if (x0 & 3) {
                uint8_t mask = (uint8_t)(0xFF >> ((x0 & 3) << 1));
                vram[row_off + b0] = (uint8_t)((vram[row_off + b0] & ~mask) | (fill & mask));
                b0++;
            }
            if ((x1 & 3) != 3) {
                uint8_t mask = (uint8_t)(0xFF << ((3 - (x1 & 3)) << 1));
                vram[row_off + b1] = (uint8_t)((vram[row_off + b1] & ~mask) | (fill & mask));
                b1--;
            }
            for (c = b0; c <= b1; c++) {
                vram[row_off + c] = fill;
            }
        }
    }
}

/* Fast byte-aligned 8x8 ROM BIOS text writer (x must be a multiple of 4).
 * Overwrites background to black in-place so no clear rectangle is needed.
 */
static void cga_text_aligned(int16_t x, int16_t y, const char *str, uint8_t color, uint8_t pad_chars) {
    volatile uint8_t __far *vram = (volatile uint8_t __far *)MK_FP(CGA_VRAM_SEG, 0);
    const uint8_t __far *rom_font = (const uint8_t __far *)MK_FP(BIOS_FONT_SEG, BIOS_FONT_OFF);
    uint8_t cmask = CGA_COLOR_FILL[color & 3];
    uint16_t byte_x = (uint16_t)x >> 2;
    uint8_t count = 0;

    while (*str || count < pad_chars) {
        uint8_t ch = *str ? (uint8_t)(*str++) : ' ';
        const uint8_t __far *glyph = &rom_font[(uint16_t)(ch & 0x7F) << 3];
        uint8_t row;

        for (row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            uint16_t off = cga_row_offset((uint16_t)(y + row)) + byte_x;
            vram[off]     = (uint8_t)(CGA_NIBBLE_MASK[bits >> 4] & cmask);
            vram[off + 1] = (uint8_t)(CGA_NIBBLE_MASK[bits & 0x0F] & cmask);
        }
        byte_x += 2;
        count++;
    }
}

static void apply_palette(uint16_t lvl) {
    uint8_t p = (uint8_t)(lvl % 3);
    /* Level % 3 == 0: Palette 1 (Cyan / Magenta / Light Gray)
     * Level % 3 == 1: Palette 0 (Green / Red / Brown)
     * Level % 3 == 2: Palette 2 (Cyan / Red / Light Gray)
     */
    uint8_t hw_pal = (p == 0) ? 1 : ((p == 1) ? 0 : 2);
    os_gfx_set_palette(hw_pal, 0, 0);
}

/* Direct VRAM 8x8 block writer (px is always a multiple of 4 = 2 bytes per row) */
static void draw_block(uint16_t px, uint16_t py, uint8_t color) {
    volatile uint8_t __far *vram = (volatile uint8_t __far *)MK_FP(CGA_VRAM_SEG, 0);
    uint16_t byte_x = px >> 2;
    uint8_t r;

    if (color == 0) {
        for (r = 0; r < 8; r++) {
            uint16_t off = cga_row_offset(py + r) + byte_x;
            vram[off]     = 0x00;
            vram[off + 1] = 0x00;
        }
    } else {
        uint8_t fill = CGA_COLOR_FILL[color & 3];
        uint8_t hi   = (color == 3) ? 0 : 3;
        /* Row 1 has the 1px highlight bevel at x_offset = 1 (bits 5..4 of first byte) */
        uint8_t row1_b0 = (uint8_t)((fill & ~0x30) | (hi << 4));

        for (r = 0; r < 8; r++) {
            uint16_t off = cga_row_offset(py + r) + byte_x;
            vram[off]     = (r == 1) ? row1_b0 : fill;
            vram[off + 1] = fill;
        }
    }
}

static void draw_field_cell(int8_t cx, int8_t cy, uint8_t color) {
    uint16_t px = FIELD_X + (uint16_t)cx * BLOCK_SIZE;
    uint16_t py = FIELD_Y + (uint16_t)cy * BLOCK_SIZE;
    draw_block(px, py, color);
}

static void render_piece(int8_t type, int8_t rot, int8_t x, int8_t y, uint8_t color) {
    uint8_t i;
    for (i = 0; i < 4; i++) {
        int8_t bx = x + PIECES[type][rot][i].x;
        int8_t by = y + PIECES[type][rot][i].y;
        if (by >= 0 && by < GRID_H && bx >= 0 && bx < GRID_W) {
            draw_field_cell(bx, by, color);
        }
    }
}

static void render_next_box(void) {
    uint8_t i;
    /* Clear preview box interior */
    cga_fill_rect_fast(150, 38, 36, 36, 0);
    for (i = 0; i < 4; i++) {
        int8_t bx = PIECES[s_next_type][0][i].x;
        int8_t by = PIECES[s_next_type][0][i].y;
        uint16_t px = 152 + (uint16_t)bx * BLOCK_SIZE;
        uint16_t py = 42 + (uint16_t)by * BLOCK_SIZE;
        draw_block(px, py, PIECE_COLOR[s_next_type]);
    }
}

static void update_hud_text(void) {
    char buf[16];

    /* Score (x=132 is a multiple of 4 for fast byte-aligned ROM font blit) */
    u32_to_str(s_score, buf);
    cga_text_aligned(132, 96, buf, 3, 7);

    /* Level */
    u16_to_str(s_level, buf);
    cga_text_aligned(132, 122, buf, 3, 7);

    /* Lines */
    u16_to_str(s_lines, buf);
    cga_text_aligned(132, 148, buf, 3, 7);

    /* High Score */
    u32_to_str((s_score > s_high_score) ? s_score : s_high_score, buf);
    cga_text_aligned(132, 174, buf, 3, 7);
}

static void render_playfield_grid(void) {
    int8_t r, c;
    for (r = 0; r < GRID_H; r++) {
        for (c = 0; c < GRID_W; c++) {
            draw_field_cell(c, r, s_grid[r][c]);
        }
    }
}

static void render_full_screen(void) {
    os_gfx_clear(0);

    /* Outer playfield framing */
    os_gfx_rect(FIELD_X - 2, FIELD_Y - 2, FIELD_PX_W + 4, FIELD_PX_H + 4, 3, 0);
    os_gfx_rect(FIELD_X - 1, FIELD_Y - 1, FIELD_PX_W + 2, FIELD_PX_H + 2, 1, 0);

    /* Title & UI Headers (x coordinates multiple of 4) */
    cga_text_aligned(132, 18,  "hobbyTris", 3, 0);
    cga_text_aligned(148, 28,  "NEXT", 1, 0);
    os_gfx_rect(148, 36, 40, 40, 1, 0);

    cga_text_aligned(132, 84,  "SCORE", 2, 0);
    cga_text_aligned(132, 110, "LEVEL", 2, 0);
    cga_text_aligned(132, 136, "LINES", 2, 0);
    cga_text_aligned(132, 162, "HIGH",  1, 0);

    /* Key controls legend with CP437 arrows */
    cga_text_aligned(212, 84,  "CONTROLS", 1, 0);
    cga_text_aligned(212, 98,  "\x1B/\x1A Move", 3, 0);
    cga_text_aligned(212, 110, "\x18/Spc Rot", 3, 0);
    cga_text_aligned(212, 122, "\x19     Soft", 3, 0);
    cga_text_aligned(212, 134, "Enter Drop", 3, 0);
    cga_text_aligned(212, 148, "P     Pause", 1, 0);
    cga_text_aligned(212, 160, "M     Mute", 1, 0);
    cga_text_aligned(212, 172, "Esc/Q Quit", 1, 0);

    render_playfield_grid();
    render_next_box();
    update_hud_text();
}

/* Draws a centered modal prompt box over the playfield */
static void draw_prompt_box(const char *line1, uint8_t color1, const char *line2, uint8_t color2) {
    int16_t bx = 16; /* Centered around playfield X=28..108, multiple of 4 */
    int16_t by = FIELD_Y + 64;
    int16_t bw = 104;
    int16_t bh = 28;

    cga_fill_rect_fast(bx, by, bw, bh, 0);
    os_gfx_rect(bx, by, bw, bh, 3, 0);
    os_gfx_rect(bx + 2, by + 2, bw - 4, bh - 4, 1, 0);

    cga_text_aligned(bx + 8, by + 5,  line1, color1, 0);
    cga_text_aligned(bx + 8, by + 15, line2, color2, 0);
}

static void clear_prompt_box(void) {
    /* Erase left and right overhang outside playfield border, restore border and playfield */
    cga_fill_rect_fast(16, FIELD_Y + 64, FIELD_X - 2 - 16, 28, 0);
    cga_fill_rect_fast(FIELD_X + FIELD_PX_W + 2, FIELD_Y + 64, 120 - (FIELD_X + FIELD_PX_W + 2), 28, 0);
    os_gfx_rect(FIELD_X - 2, FIELD_Y - 2, FIELD_PX_W + 4, FIELD_PX_H + 4, 3, 0);
    os_gfx_rect(FIELD_X - 1, FIELD_Y - 1, FIELD_PX_W + 2, FIELD_PX_H + 2, 1, 0);
    render_playfield_grid();
}

/* --------------------------------------------------------------------------
 * Game Mechanics & Collision
 * -------------------------------------------------------------------------- */

static int can_place(int8_t type, int8_t rot, int8_t x, int8_t y) {
    uint8_t i;
    for (i = 0; i < 4; i++) {
        int8_t bx = x + PIECES[type][rot][i].x;
        int8_t by = y + PIECES[type][rot][i].y;
        if (bx < 0 || bx >= GRID_W || by >= GRID_H) return 0;
        if (by >= 0 && s_grid[by][bx] != 0) return 0;
    }
    return 1;
}

static uint16_t get_drop_ticks_for_level(uint16_t lvl) {
    if (lvl == 0) return 14;
    if (lvl == 1) return 12;
    if (lvl == 2) return 10;
    if (lvl == 3) return 8;
    if (lvl == 4) return 6;
    if (lvl == 5) return 5;
    if (lvl == 6) return 4;
    if (lvl == 7) return 3;
    if (lvl == 8) return 2;
    return 1;
}

static void clear_completed_lines(void) {
    int8_t r, c, k;
    uint8_t cleared = 0;
    uint8_t full_rows[4];

    for (r = GRID_H - 1; r >= 0; r--) {
        uint8_t full = 1;
        for (c = 0; c < GRID_W; c++) {
            if (s_grid[r][c] == 0) {
                full = 0;
                break;
            }
        }
        if (full) {
            full_rows[cleared++] = (uint8_t)r;
        }
    }

    if (cleared == 0) return;

    /* Flash cleared rows in bright white (Color 3) */
    for (k = 0; k < cleared; k++) {
        uint8_t fr = full_rows[k];
        for (c = 0; c < GRID_W; c++) {
            draw_field_cell(c, fr, 3);
        }
    }

    sfx_play((cleared == 4) ? 880 : 587, 4);

    /* Short visual pause for line clear flash (2 hardware BDA ticks) */
    {
        uint16_t start_tick = GET_TICKS();
        while ((uint16_t)(GET_TICKS() - start_tick) < 2) {
        }
    }

    /* Shift lines above cleared rows down */
    for (k = 0; k < cleared; k++) {
        uint8_t fr = full_rows[k];
        for (r = fr; r > 0; r--) {
            for (c = 0; c < GRID_W; c++) {
                s_grid[r][c] = s_grid[r - 1][c];
            }
        }
        for (c = 0; c < GRID_W; c++) {
            s_grid[0][c] = 0;
        }
        /* Adjust remaining indices for lines shifted above */
        for (r = k + 1; r < cleared; r++) {
            full_rows[r]++;
        }
    }

    /* Redraw playfield via fast direct VRAM block writer */
    render_playfield_grid();

    /* Scoring: 1=40, 2=100, 3=300, 4=1200 * (level + 1) */
    {
        uint16_t base_pts = 0;
        if (cleared == 1) base_pts = 40;
        else if (cleared == 2) base_pts = 100;
        else if (cleared == 3) base_pts = 300;
        else if (cleared >= 4) base_pts = 1200;

        s_score += mul16(base_pts, (uint16_t)(s_level + 1));
        s_lines += (uint16_t)cleared;

        /* Level up every 10 lines */
        if (s_lines / 10 > s_level) {
            s_level = s_lines / 10;
            apply_palette(s_level);
            sfx_play(988, 5);
        }
    }

    update_hud_text();
}

/* --------------------------------------------------------------------------
 * Main Entry Point & Game Loop
 * -------------------------------------------------------------------------- */

int app_main(void) {
    const char *args = os_get_args();
    int running = 1;
    int paused = 0;
    uint8_t auto_test = 0;
    uint16_t test_ticks = 0;
    uint16_t last_tick;
    uint16_t last_sfx_tick;

    if (args) {
        while (*args == ' ') args++;
        if (*args == '/' || *args == '-') {
            if (args[1] == 't' || args[1] == 'T') auto_test = 1;
        }
    }

    /* Initialize PRNG from system BDA timer */
    s_prng_seed = GET_TICKS();
    if (s_prng_seed == 0) s_prng_seed = 0xA55A;

    load_high_score();

    /* Switch to CGA Mode 4 (320x200 4-color) */
    os_gfx_set_mode(OS_MODE_CGA_320x200);
    init_music();

new_game:
    /* Reset game grid */
    {
        int8_t r, c;
        for (r = 0; r < GRID_H; r++) {
            for (c = 0; c < GRID_W; c++) {
                s_grid[r][c] = 0;
            }
        }
    }

    s_score = 0;
    s_lines = 0;
    s_level = 0;
    s_bag_idx = 7;
    paused = 0;

    apply_palette(0);

    s_cur_type  = next_bag_piece();
    s_cur_rot   = 0;
    s_cur_x     = 3;
    s_cur_y     = 0;

    s_next_type = next_bag_piece();

    render_full_screen();

    /* Wait for player to press SPACE before starting gameplay */
    if (!auto_test) {
        draw_prompt_box("PRESS SPACE", 3, "  TO PLAY  ", 1);
        for (;;) {
            if (os_kbhit()) {
                uint16_t k = os_getc();
                uint8_t ch = (uint8_t)(k & 0xFF);
                if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                    running = 0;
                    goto cleanup;
                }
                if (ch == 'm' || ch == 'M') {
                    toggle_sound();
                }
                if (ch == KEY_SPACE || ch == KEY_ENTER) {
                    break;
                }
            }
        }
        clear_prompt_box();
    }

    start_bg_music();
    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);

    last_tick = GET_TICKS();
    last_sfx_tick = last_tick;

    while (running) {
        uint16_t now = GET_TICKS();
        uint16_t dt = now - last_tick;
        uint16_t drop_interval = get_drop_ticks_for_level(s_level);

        /* Advance SFX timer strictly once per 18.2 Hz hardware tick */
        if (now != last_sfx_tick) {
            last_sfx_tick = now;
            sfx_update_tick();
            if (auto_test) {
                test_ticks++;
                if (test_ticks >= 36) {
                    running = 0;
                    break;
                }
            }
        }

        /* Process keyboard inputs non-blockingly */
        while (os_kbhit()) {
            uint16_t key = os_getc();
            uint8_t  ch  = (uint8_t)(key & 0xFF);
            uint8_t  sc  = (uint8_t)(key >> 8);

            if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                running = 0;
                goto cleanup;
            }

            if (ch == 'p' || ch == 'P') {
                paused = !paused;
                if (paused) {
                    draw_prompt_box(" [PAUSED]  ", 3, "PRESS P/SPC", 1);
                } else {
                    clear_prompt_box();
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    last_tick = GET_TICKS();
                }
                continue;
            }

            if (paused && (ch == KEY_SPACE || ch == KEY_ENTER)) {
                paused = 0;
                clear_prompt_box();
                render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                last_tick = GET_TICKS();
                continue;
            }

            if (ch == 'm' || ch == 'M') {
                toggle_sound();
                continue;
            }

            if (paused) continue;

            /* Move Left */
            if (sc == SC_LEFT || ch == 'a' || ch == 'A' || ch == '4') {
                if (can_place(s_cur_type, s_cur_rot, s_cur_x - 1, s_cur_y)) {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                    s_cur_x--;
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    sfx_play(330, 1);
                }
            }
            /* Move Right */
            else if (sc == SC_RIGHT || ch == 'd' || ch == 'D' || ch == '6') {
                if (can_place(s_cur_type, s_cur_rot, s_cur_x + 1, s_cur_y)) {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                    s_cur_x++;
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    sfx_play(330, 1);
                }
            }
            /* Rotate Clockwise (Up / Space / W) with Wall Kick */
            else if (sc == SC_UP || ch == KEY_SPACE || ch == 'w' || ch == 'W' || ch == '8') {
                int8_t new_rot = (s_cur_rot + 1) % 4;
                if (can_place(s_cur_type, new_rot, s_cur_x, s_cur_y)) {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                    s_cur_rot = new_rot;
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    sfx_play(440, 1);
                } else if (can_place(s_cur_type, new_rot, s_cur_x - 1, s_cur_y)) {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                    s_cur_x--;
                    s_cur_rot = new_rot;
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    sfx_play(440, 1);
                } else if (can_place(s_cur_type, new_rot, s_cur_x + 1, s_cur_y)) {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                    s_cur_x++;
                    s_cur_rot = new_rot;
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    sfx_play(440, 1);
                }
            }
            /* Soft Drop (Down / S) */
            else if (sc == SC_DOWN || ch == 's' || ch == 'S' || ch == '2') {
                if (can_place(s_cur_type, s_cur_rot, s_cur_x, s_cur_y + 1)) {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                    s_cur_y++;
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    s_score += 1;
                    update_hud_text();
                    last_tick = now;
                }
            }
            /* Hard Drop (Enter) */
            else if (ch == KEY_ENTER) {
                uint16_t drop_dist = 0;
                render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                while (can_place(s_cur_type, s_cur_rot, s_cur_x, s_cur_y + 1)) {
                    s_cur_y++;
                    drop_dist++;
                }
                render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                s_score += (uint32_t)(drop_dist * 2);
                update_hud_text();
                sfx_play(220, 2);
                dt = drop_interval; /* Trigger lock immediately */
            }
        }

        if (paused) {
            continue;
        }

        /* Gravity step */
        if (dt >= drop_interval) {
            last_tick = now;
            if (can_place(s_cur_type, s_cur_rot, s_cur_x, s_cur_y + 1)) {
                render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, 0);
                s_cur_y++;
                render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
            } else {
                /* Lock current piece into grid */
                uint8_t i;
                for (i = 0; i < 4; i++) {
                    int8_t bx = s_cur_x + PIECES[s_cur_type][s_cur_rot][i].x;
                    int8_t by = s_cur_y + PIECES[s_cur_type][s_cur_rot][i].y;
                    if (by >= 0 && by < GRID_H && bx >= 0 && bx < GRID_W) {
                        s_grid[by][bx] = PIECE_COLOR[s_cur_type];
                    }
                }

                sfx_play(165, 2);

                clear_completed_lines();

                /* Spawn next piece */
                s_cur_type  = s_next_type;
                s_cur_rot   = 0;
                s_cur_x     = 3;
                s_cur_y     = 0;
                s_next_type = next_bag_piece();

                render_next_box();

                /* Check for Game Over condition */
                if (!can_place(s_cur_type, s_cur_rot, s_cur_x, s_cur_y)) {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                    save_high_score();
                    update_hud_text();
                    sfx_play(110, 6);
                    draw_prompt_box(" GAME OVER ", 2, "PRESS SPACE", 3);
                    for (;;) {
                        uint16_t cur_t = GET_TICKS();
                        if (cur_t != last_sfx_tick) {
                            last_sfx_tick = cur_t;
                            sfx_update_tick();
                        }
                        if (os_kbhit()) {
                            uint16_t k = os_getc();
                            uint8_t ch = (uint8_t)(k & 0xFF);
                            if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                                running = 0;
                                goto cleanup;
                            }
                            if (ch == 'm' || ch == 'M') {
                                toggle_sound();
                            }
                            if (ch == KEY_SPACE || ch == KEY_ENTER) {
                                goto new_game;
                            }
                        }
                    }
                } else {
                    render_piece(s_cur_type, s_cur_rot, s_cur_x, s_cur_y, PIECE_COLOR[s_cur_type]);
                }
            }
        }
    }

cleanup:
    save_high_score();
    stop_all_audio();

    /* Restore text mode 80x25 */
    os_gfx_set_mode(OS_MODE_TEXT_80x25);

    return 0;
}
