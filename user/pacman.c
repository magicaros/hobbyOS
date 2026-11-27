/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * hobbyPac - Authentic CGA Mode 4 Pac-Man for hobbyOS
 * Features direct CGA VRAM rendering for 4.77 MHz 8088 performance, hardware BDA
 * timer synchronization (18.2 Hz), strict 2-pixel sub-stepped grid alignment,
 * dynamic fruit spawning in cleared corridors, ghost AI (chaser + simple roamers),
 * CGA hardware palette swapping, background IMF music, and high score persistence.
 */

#include "../include/hobbyos.h"

/* --------------------------------------------------------------------------
 * Constants & Definitions
 * -------------------------------------------------------------------------- */

#define MAZE_COLS       28
#define MAZE_ROWS       31
#define TILE_SIZE       6
#define STEP_PX         2   /* Strict 2-pixel step: 6 is divisible by 2 (offsets 0, 2, 4) */

#define MAZE_X          16
#define MAZE_Y          7
#define MAZE_PX_W       (MAZE_COLS * TILE_SIZE) /* 168 px */
#define MAZE_PX_H       (MAZE_ROWS * TILE_SIZE) /* 186 px */

/* Tile types */
#define TILE_EMPTY      0
#define TILE_WALL       1
#define TILE_DOT        2
#define TILE_ENERGIZER  3
#define TILE_DOOR       4
#define TILE_PEN        5

/* Direction constants */
#define DIR_NONE        0
#define DIR_UP          1
#define DIR_LEFT        2
#define DIR_DOWN        3
#define DIR_RIGHT       4

/* Ghost states */
#define GSTATE_IN_PEN       0
#define GSTATE_LEAVING_PEN  1
#define GSTATE_CHASE        2
#define GSTATE_FRIGHTENED   3
#define GSTATE_EYES         4

/* BIOS Scancodes */
#define SC_UP           0x48
#define SC_LEFT         0x4B
#define SC_RIGHT        0x4D
#define SC_DOWN         0x50

#define KEY_ESC         0x1B
#define KEY_SPACE       0x20

#define CGA_VRAM_SEG    0xB800
#define BIOS_FONT_SEG   0xF000
#define BIOS_FONT_OFF   0xFA6E

#define FP_HERE(p)      ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(p)))

/* 18.2 Hz BIOS timer tick counter at 0x0040:0x006C (hardware clock) */
#define GET_TICKS()     (*((volatile uint16_t __far *)MK_FP(0x0040, 0x006C)))

/* --------------------------------------------------------------------------
 * Maze Layout Template
 * -------------------------------------------------------------------------- */

static const char * const MAZE_TEMPLATE[MAZE_ROWS] = {
    "############################",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#o####.#####.##.#####.####o#",
    "#.####.#####.##.#####.####.#",
    "#..........................#",
    "#.####.##.########.##.####.#",
    "#.####.##.########.##.####.#",
    "#......##....##....##......#",
    "######.##### ## #####.######",
    "     #.##### ## #####.#     ",
    "     #.##          ##.#     ",
    "     #.## ###--### ##.#     ",
    "######.## #______# ##.######",
    "      .   #______#   .      ",
    "######.## #______# ##.######",
    "     #.## ######## ##.#     ",
    "     #.##          ##.#     ",
    "     #.## ######## ##.#     ",
    "######.## ######## ##.######",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#.####.#####.##.#####.####.#",
    "#o..##................##..o#",
    "###.##.##.########.##.##.###",
    "###.##.##.########.##.##.###",
    "#......##....##....##......#",
    "#.##########.##.##########.#",
    "#.##########.##.##########.#",
    "#..........................#",
    "############################"
};

/* --------------------------------------------------------------------------
 * 6x6 Sprite Bitmaps
 * -------------------------------------------------------------------------- */

/* Pac-Man mouth frames (6 rows of 6 bits, bit 5 = leftmost, bit 0 = rightmost) */
static const uint8_t SPRITE_PAC_CIRCLE[6] = { 0x1E, 0x3F, 0x3F, 0x3F, 0x3F, 0x1E };
static const uint8_t SPRITE_PAC_RIGHT[6]  = { 0x1E, 0x3E, 0x38, 0x38, 0x3E, 0x1E };
static const uint8_t SPRITE_PAC_LEFT[6]   = { 0x1E, 0x1F, 0x07, 0x07, 0x1F, 0x1E };
static const uint8_t SPRITE_PAC_UP[6]     = { 0x0E, 0x1B, 0x3B, 0x3F, 0x3F, 0x1E };
static const uint8_t SPRITE_PAC_DOWN[6]   = { 0x1E, 0x3F, 0x3F, 0x3B, 0x1B, 0x0E };

/* Ghost bodies */
static const uint8_t SPRITE_GHOST[6]       = { 0x1E, 0x3F, 0x3F, 0x3F, 0x3F, 0x2D };
static const uint8_t SPRITE_GHOST_FRIGHT[6]= { 0x1E, 0x3F, 0x33, 0x2D, 0x3F, 0x2D };
static const uint8_t SPRITE_EYES_ONLY[6]   = { 0x00, 0x12, 0x12, 0x00, 0x00, 0x00 };

/* Fruits (Cherry, Strawberry, Orange, Apple, Melon) */
static const uint8_t SPRITE_CHERRY[6]     = { 0x04, 0x0A, 0x1F, 0x3F, 0x3F, 0x1E };
static const uint8_t SPRITE_STRAWBERRY[6] = { 0x0E, 0x1F, 0x3F, 0x3F, 0x1E, 0x0C };
static const uint8_t SPRITE_ORANGE[6]     = { 0x04, 0x1E, 0x3F, 0x3F, 0x3F, 0x1E };
static const uint8_t SPRITE_APPLE[6]      = { 0x04, 0x1B, 0x3F, 0x3F, 0x3F, 0x1E };
static const uint8_t SPRITE_MELON[6]      = { 0x0E, 0x1F, 0x3F, 0x3F, 0x1F, 0x0E };

static const uint16_t FRUIT_POINTS[5] = { 100, 300, 500, 700, 1000 };

/* Expands a 4-bit font nibble into a 4-pixel (8-bit) 2bpp CGA mask (all 1s per set pixel) */
static const uint8_t CGA_NIBBLE_MASK[16] = {
    0x00, 0x03, 0x0C, 0x0F,
    0x30, 0x33, 0x3C, 0x3F,
    0xC0, 0xC3, 0xCC, 0xCF,
    0xF0, 0xF3, 0xFC, 0xFF
};

static const uint8_t CGA_COLOR_FILL[4] = { 0x00, 0x55, 0xAA, 0xFF };

/* --------------------------------------------------------------------------
 * Entity Structures
 * -------------------------------------------------------------------------- */

typedef struct {
    int16_t px;         /* Pixel position X relative to MAZE_X (always even) */
    int16_t py;         /* Pixel position Y relative to MAZE_Y (always even) */
    uint8_t cur_dir;    /* Current moving direction */
    uint8_t next_dir;   /* Buffered next turn */
    uint8_t anim_frame; /* Animation frame toggle */
    uint8_t speed_cnt;  /* Sub-step cadence counter */
} pacman_t;

typedef struct {
    int16_t px;         /* Pixel X (always even) */
    int16_t py;         /* Pixel Y (always even) */
    uint8_t cur_dir;    /* Current moving direction */
    uint8_t state;      /* GSTATE_* */
    uint8_t color;      /* CGA color (1=Green, 2=Red, 3=Yellow) */
    uint8_t pen_timer;  /* Dot count before release */
    uint8_t speed_cnt;  /* Sub-step cadence counter */
} ghost_t;

/* --------------------------------------------------------------------------
 * Game State
 * -------------------------------------------------------------------------- */

static uint8_t  s_grid[MAZE_ROWS][MAZE_COLS];

static pacman_t s_pacman;
static ghost_t  s_ghosts[4];

static uint32_t s_score;
static uint32_t s_high_score;
static uint16_t s_level;
static uint8_t  s_lives;

static uint16_t s_dots_total;
static uint16_t s_dots_eaten;
static uint16_t s_dots_remaining;

static uint16_t s_frightened_timer;
static uint8_t  s_frightened_eaten_count;
static uint8_t  s_palette_mode; /* 0=Normal, 1=Frightened Blue */

/* Dynamic Fruit in cleared path */
static uint8_t  s_fruit_active;
static uint16_t s_fruit_timer;
static int8_t   s_fruit_r;
static int8_t   s_fruit_c;
static uint8_t  s_fruit_type;

/* Audio & System */
static uint8_t  s_sound_on = 1;
static uint16_t s_imf_seg  = 0;
static uint16_t s_imf_size = 0;
static uint16_t s_sfx_timer= 0;
static uint16_t s_prng_seed= 0x4321;
static uint8_t  s_waka_toggle = 0;

/* --------------------------------------------------------------------------
 * Freestanding Math Primitives (Strict 8088 / Watcom -zl compliant)
 * -------------------------------------------------------------------------- */

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
 * Audio Management
 * -------------------------------------------------------------------------- */

static void sfx_play(uint16_t freq, uint16_t ticks) {
    if (!s_sound_on) return;
    os_sound_tone(freq, 0, 2); /* Channel 2 for non-blocking SFX */
    s_sfx_timer = ticks;
}

static void sfx_update(void) {
    if (s_sfx_timer > 0) {
        s_sfx_timer--;
        if (s_sfx_timer == 0) {
            os_sound_opl_write(0xB2, 0x00);
            os_sound_tone(0, 0, 2);
        }
    }
}

static void init_music(void) {
    long sz = os_file_size("/MUSIC/PACMAN.IMF");
    if (sz <= 0 || sz > 32000L) {
        sz = os_file_size("SYS:MUSIC/PACMAN.IMF");
        if (sz <= 0 || sz > 32000L) return;
        s_imf_seg = os_mem_alloc((uint16_t)((sz + 15) >> 4));
        if (s_imf_seg) {
            os_file_read("SYS:MUSIC/PACMAN.IMF", (void __far *)MK_FP(s_imf_seg, 0), (uint16_t)sz);
            s_imf_size = (uint16_t)sz;
        }
    } else {
        s_imf_seg = os_mem_alloc((uint16_t)((sz + 15) >> 4));
        if (s_imf_seg) {
            os_file_read("/MUSIC/PACMAN.IMF", (void __far *)MK_FP(s_imf_seg, 0), (uint16_t)sz);
            s_imf_size = (uint16_t)sz;
        }
    }
}

static void play_intro_tune(void) {
    if (s_sound_on && s_imf_seg && s_imf_size > 0) {
        os_sound_bg_play(560, 0, s_imf_size, (const void __far *)MK_FP(s_imf_seg, 0), "PACMAN");
    }
}

static void stop_audio(void) {
    os_sound_bg_stop();
    os_sound_stop();
}

/* --------------------------------------------------------------------------
 * Persistence (High Score)
 * -------------------------------------------------------------------------- */

static void load_high_score(void) {
    s_high_score = 0;
    if (os_file_size("/GAMES/PACMAN.DAT") >= 4) {
        os_file_read("/GAMES/PACMAN.DAT", (void __far *)FP_HERE(&s_high_score), sizeof(s_high_score));
    } else if (os_file_size("SYS:GAMES/PACMAN.DAT") >= 4) {
        os_file_read("SYS:GAMES/PACMAN.DAT", (void __far *)FP_HERE(&s_high_score), sizeof(s_high_score));
    }
}

static void save_high_score(void) {
    if (s_score > s_high_score) {
        s_high_score = s_score;
    }
    os_file_write("/GAMES/PACMAN.DAT", (const void __far *)FP_HERE(&s_high_score), sizeof(s_high_score));
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

static void cga_putpixel_fast(int16_t x, int16_t y, uint8_t color) {
    volatile uint8_t __far *vram = (volatile uint8_t __far *)MK_FP(CGA_VRAM_SEG, 0);
    uint16_t off;
    uint8_t  shift;
    if ((uint16_t)x >= 320 || (uint16_t)y >= 200) return;
    off   = cga_row_offset((uint16_t)y) + ((uint16_t)x >> 2);
    shift = (uint8_t)((3 - (x & 3)) << 1);
    vram[off] = (uint8_t)((vram[off] & ~(0x03 << shift)) | ((color & 0x03) << shift));
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
            /* Left partial byte */
            if (x0 & 3) {
                uint8_t mask = (uint8_t)(0xFF >> ((x0 & 3) << 1));
                vram[row_off + b0] = (uint8_t)((vram[row_off + b0] & ~mask) | (fill & mask));
                b0++;
            }
            /* Right partial byte */
            if ((x1 & 3) != 3) {
                uint8_t mask = (uint8_t)(0xFF << ((3 - (x1 & 3)) << 1));
                vram[row_off + b1] = (uint8_t)((vram[row_off + b1] & ~mask) | (fill & mask));
                b1--;
            }
            /* Full interior bytes (4 pixels per write) */
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
        byte_x += 2; /* 8 pixels = 2 bytes */
        count++;
    }
}

static void draw_sprite_6x6(int16_t x, int16_t y, const uint8_t *rows, uint8_t color) {
    volatile uint8_t __far *vram = (volatile uint8_t __far *)MK_FP(CGA_VRAM_SEG, 0);
    uint8_t cval = (uint8_t)(color & 3);
    int8_t r, c;

    for (r = 0; r < 6; r++) {
        uint8_t m = rows[r];
        int16_t py = y + r;
        uint16_t row_off;
        if (!m || (uint16_t)py >= 200) continue;
        row_off = cga_row_offset((uint16_t)py);
        for (c = 0; c < 6; c++) {
            if (m & (0x20 >> c)) {
                uint16_t px = (uint16_t)(x + c);
                uint16_t off = row_off + (px >> 2);
                uint8_t shift = (uint8_t)((3 - (px & 3)) << 1);
                vram[off] = (uint8_t)((vram[off] & ~(0x03 << shift)) | (cval << shift));
            }
        }
    }
}

static void set_palette(uint8_t mode) {
    s_palette_mode = mode;
    if (mode == 0) {
        /* Normal: Palette 0, High Intensity (1=LtGreen walls, 2=LtRed ghost, 3=Yellow pacman/dots) */
        os_gfx_set_palette(0, 1, 0);
    } else {
        /* Frightened: Palette 1, High Intensity (1=LtCyan blue ghosts, 2=LtMagenta, 3=White flashing) */
        os_gfx_set_palette(1, 1, 0);
    }
}

/* Redraws whatever background tile element belongs at (r, c) */
static void render_tile_background(int16_t r, int16_t c) {
    int16_t sx, sy;
    uint8_t t;

    if (r < 0 || r >= MAZE_ROWS || c < 0 || c >= MAZE_COLS) return;

    sx = MAZE_X + c * TILE_SIZE;
    sy = MAZE_Y + r * TILE_SIZE;
    t  = s_grid[r][c];

    if (t == TILE_WALL) {
        cga_fill_rect_fast(sx, sy, TILE_SIZE, TILE_SIZE, 1);
        return;
    }

    /* Clear tile background to black */
    cga_fill_rect_fast(sx, sy, TILE_SIZE, TILE_SIZE, 0);

    if (t == TILE_DOT) {
        /* Centered 2x2 dot */
        cga_fill_rect_fast(sx + 2, sy + 2, 2, 2, 3);
    } else if (t == TILE_ENERGIZER) {
        /* Centered 4x4 energizer */
        cga_fill_rect_fast(sx + 1, sy + 1, 4, 4, 3);
    } else if (t == TILE_DOOR) {
        /* Door horizontal bar */
        cga_fill_rect_fast(sx, sy + 2, TILE_SIZE, 1, 2);
    }

    /* If fruit is active on this tile, draw the fruit */
    if (s_fruit_active && s_fruit_r == r && s_fruit_c == c) {
        const uint8_t *f_spr = SPRITE_CHERRY;
        if (s_fruit_type == 1) f_spr = SPRITE_STRAWBERRY;
        else if (s_fruit_type == 2) f_spr = SPRITE_ORANGE;
        else if (s_fruit_type == 3) f_spr = SPRITE_APPLE;
        else if (s_fruit_type >= 4) f_spr = SPRITE_MELON;

        /* Blink during final 3 seconds (~55 ticks) */
        if (s_fruit_timer > 55 || (s_fruit_timer & 2)) {
            draw_sprite_6x6(sx, sy, f_spr, 2);
        }
    }
}

/* Safely erases and restores background behind a 6x6 entity */
static void restore_entity_region(int16_t px, int16_t py) {
    int16_t c0, r0, c1, r1, r, c;

    if (px < 0) px = 0;
    if (py < 0) py = 0;

    c0 = px / TILE_SIZE;
    r0 = py / TILE_SIZE;
    c1 = (px + TILE_SIZE - 1) / TILE_SIZE;
    r1 = (py + TILE_SIZE - 1) / TILE_SIZE;

    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    if (r1 >= MAZE_ROWS) r1 = MAZE_ROWS - 1;
    if (c1 >= MAZE_COLS) c1 = MAZE_COLS - 1;

    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) {
            render_tile_background(r, c);
        }
    }
}

/* Restores the maze tile region where READY! / [PAUSED] / prompts are drawn */
static void restore_banner_region(int16_t r0, int16_t r1, int16_t c0, int16_t c1) {
    int16_t r, c;
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) {
            render_tile_background(r, c);
        }
    }
}

/* Draws a centered modal prompt box over the middle of the maze */
static void draw_prompt_box(const char *line1, uint8_t color1, const char *line2, uint8_t color2) {
    int16_t bx = MAZE_X + 32;  /* 16 + 32 = 48 (multiple of 4) */
    int16_t by = MAZE_Y + 86;
    int16_t bw = 104;
    int16_t bh = 28;

    cga_fill_rect_fast(bx, by, bw, bh, 0);
    os_gfx_rect(bx, by, bw, bh, 3, 0);
    os_gfx_rect(bx + 2, by + 2, bw - 4, bh - 4, 1, 0);

    cga_text_aligned(bx + 8, by + 5,  line1, color1, 0);
    cga_text_aligned(bx + 8, by + 15, line2, color2, 0);
}

/* Fast score-only HUD update (called when eating dots/fruits/ghosts) */
static void update_score_hud(void) {
    char buf[16];

    u32_to_str(s_score, buf);
    cga_text_aligned(200, 62, buf, 3, 7);

    if (s_score >= s_high_score) {
        s_high_score = s_score;
        cga_text_aligned(200, 36, buf, 3, 7);
    }
}

/* Full HUD update (called at level start or when lives change) */
static void update_hud(void) {
    char buf[16];

    /* High score */
    u32_to_str((s_score > s_high_score) ? s_score : s_high_score, buf);
    cga_text_aligned(200, 36, buf, 3, 7);

    /* Score */
    u32_to_str(s_score, buf);
    cga_text_aligned(200, 62, buf, 3, 7);

    /* Level */
    u16_to_str(s_level + 1, buf);
    cga_text_aligned(200, 88, buf, 3, 4);

    /* Lives */
    cga_fill_rect_fast(200, 116, 60, 8, 0);
    {
        uint8_t l;
        for (l = 0; l < s_lives && l < 5; l++) {
            draw_sprite_6x6(200 + l * 10, 116, SPRITE_PAC_RIGHT, 3);
        }
    }

    /* Bonus fruit display */
    cga_fill_rect_fast(200, 142, 100, 10, 0);
    {
        uint8_t ft = (s_level < 4) ? (uint8_t)s_level : 4;
        const uint8_t *f_spr = SPRITE_CHERRY;
        if (ft == 1) f_spr = SPRITE_STRAWBERRY;
        else if (ft == 2) f_spr = SPRITE_ORANGE;
        else if (ft == 3) f_spr = SPRITE_APPLE;
        else if (ft >= 4) f_spr = SPRITE_MELON;

        draw_sprite_6x6(200, 142, f_spr, 2);
        u16_to_str(FRUIT_POINTS[ft], buf);
        cga_text_aligned(212, 142, buf, 1, 5);
    }
}

static void render_full_screen(void) {
    int16_t r, c;

    os_gfx_clear(0);

    /* Outer border of maze */
    os_gfx_rect(MAZE_X - 1, MAZE_Y - 1, MAZE_PX_W + 2, MAZE_PX_H + 2, 1, 0);

    /* Render all maze tiles */
    for (r = 0; r < MAZE_ROWS; r++) {
        for (c = 0; c < MAZE_COLS; c++) {
            render_tile_background(r, c);
        }
    }

    /* Sidebar info (all x coordinates multiple of 4 for fast byte alignment) */
    cga_text_aligned(200, 10, "HOBBY-PAC", 3, 0);
    cga_fill_rect_fast(200, 20, 108, 1, 1);

    cga_text_aligned(200, 26, "HIGH SCORE", 1, 0);
    cga_text_aligned(200, 52, "SCORE", 2, 0);
    cga_text_aligned(200, 78, "LEVEL", 1, 0);
    cga_text_aligned(200, 104, "LIVES", 2, 0);
    cga_text_aligned(200, 130, "BONUS FRUIT", 1, 0);

    /* Controls */
    cga_text_aligned(200, 156, "CONTROLS", 1, 0);
    cga_text_aligned(200, 166, "\x18\x19\x1B\x1A / WASD", 3, 0);
    cga_text_aligned(200, 176, "P:Pause M:Mute", 3, 0);
    cga_text_aligned(200, 186, "Esc:Quit", 3, 0);

    update_hud();
}

/* --------------------------------------------------------------------------
 * Entity Drawing
 * -------------------------------------------------------------------------- */

static void draw_pacman(void) {
    int16_t sx = MAZE_X + s_pacman.px;
    int16_t sy = MAZE_Y + s_pacman.py;
    const uint8_t *spr;

    if (s_pacman.anim_frame == 0) {
        spr = SPRITE_PAC_CIRCLE;
    } else {
        switch (s_pacman.cur_dir) {
            case DIR_UP:    spr = SPRITE_PAC_UP; break;
            case DIR_LEFT:  spr = SPRITE_PAC_LEFT; break;
            case DIR_DOWN:  spr = SPRITE_PAC_DOWN; break;
            default:        spr = SPRITE_PAC_RIGHT; break;
        }
    }
    draw_sprite_6x6(sx, sy, spr, 3);
}

static void draw_ghost(const ghost_t *g) {
    int16_t sx = MAZE_X + g->px;
    int16_t sy = MAZE_Y + g->py;

    if (g->state == GSTATE_EYES) {
        draw_sprite_6x6(sx, sy, SPRITE_EYES_ONLY, 3);
    } else if (g->state == GSTATE_FRIGHTENED) {
        /* Flash white in final 2 seconds (~36 ticks) */
        uint8_t color = 1; /* Cyan */
        if (s_frightened_timer < 36 && (s_frightened_timer & 2)) {
            color = 3; /* White */
        }
        draw_sprite_6x6(sx, sy, SPRITE_GHOST_FRIGHT, color);
    } else {
        /* Normal ghost */
        draw_sprite_6x6(sx, sy, SPRITE_GHOST, g->color);
        /* Draw eye pupils looking in direction */
        if (g->cur_dir == DIR_LEFT) {
            cga_putpixel_fast(sx + 1, sy + 2, 0);
            cga_putpixel_fast(sx + 3, sy + 2, 0);
        } else if (g->cur_dir == DIR_RIGHT) {
            cga_putpixel_fast(sx + 2, sy + 2, 0);
            cga_putpixel_fast(sx + 4, sy + 2, 0);
        } else if (g->cur_dir == DIR_UP) {
            cga_putpixel_fast(sx + 1, sy + 1, 0);
            cga_putpixel_fast(sx + 4, sy + 1, 0);
        } else {
            cga_putpixel_fast(sx + 1, sy + 3, 0);
            cga_putpixel_fast(sx + 4, sy + 3, 0);
        }
    }
}

/* --------------------------------------------------------------------------
 * Fruit Spawning Logic (Dynamic Cleared-Corridor Spawn)
 * -------------------------------------------------------------------------- */

static void try_spawn_fruit(void) {
    int8_t r, c;
    uint16_t empty_count = 0;
    uint16_t chosen_idx;

    if (s_fruit_active) return;

    /* Count candidate tiles */
    for (r = 1; r < MAZE_ROWS - 1; r++) {
        for (c = 1; c < MAZE_COLS - 1; c++) {
            /* Must have originally been a dot, but is now empty */
            if (MAZE_TEMPLATE[r][c] == '.' && s_grid[r][c] == TILE_EMPTY) {
                /* Exclude ghost house and side tunnels */
                if (r >= 11 && r <= 17 && c >= 9 && c <= 18) continue;
                if (r == 14) continue;
                /* Don't spawn right on top of Pac-Man */
                if ((int16_t)(s_pacman.px / TILE_SIZE) == c &&
                    (int16_t)(s_pacman.py / TILE_SIZE) == r) continue;
                empty_count++;
            }
        }
    }

    if (empty_count == 0) return;

    chosen_idx = rand16() % empty_count;
    empty_count = 0;

    for (r = 1; r < MAZE_ROWS - 1; r++) {
        for (c = 1; c < MAZE_COLS - 1; c++) {
            if (MAZE_TEMPLATE[r][c] == '.' && s_grid[r][c] == TILE_EMPTY) {
                if (r >= 11 && r <= 17 && c >= 9 && c <= 18) continue;
                if (r == 14) continue;
                if ((int16_t)(s_pacman.px / TILE_SIZE) == c &&
                    (int16_t)(s_pacman.py / TILE_SIZE) == r) continue;

                if (empty_count == chosen_idx) {
                    s_fruit_active = 1;
                    s_fruit_timer = 180; /* ~10 seconds @ 18.2 ticks/sec */
                    s_fruit_r = r;
                    s_fruit_c = c;
                    s_fruit_type = (s_level < 4) ? (uint8_t)s_level : 4;
                    render_tile_background(r, c);
                    sfx_play(587, 2); /* Spawn cue */
                    return;
                }
                empty_count++;
            }
        }
    }
}

static void update_fruit(void) {
    if (s_fruit_active) {
        s_fruit_timer--;
        if (s_fruit_timer == 0) {
            s_fruit_active = 0;
            render_tile_background(s_fruit_r, s_fruit_c);
        } else if (s_fruit_timer < 55 && ((s_fruit_timer & 1) == 0)) {
            render_tile_background(s_fruit_r, s_fruit_c);
        }
    }
}

/* --------------------------------------------------------------------------
 * Maze & Game Setup
 * -------------------------------------------------------------------------- */

static void init_maze(void) {
    int8_t r, c;
    s_dots_total = 0;
    s_dots_eaten = 0;

    for (r = 0; r < MAZE_ROWS; r++) {
        for (c = 0; c < MAZE_COLS; c++) {
            char ch = MAZE_TEMPLATE[r][c];
            if (ch == '#') {
                s_grid[r][c] = TILE_WALL;
            } else if (ch == '.') {
                s_grid[r][c] = TILE_DOT;
                s_dots_total++;
            } else if (ch == 'o') {
                s_grid[r][c] = TILE_ENERGIZER;
                s_dots_total++;
            } else if (ch == '-') {
                s_grid[r][c] = TILE_DOOR;
            } else if (ch == '_') {
                s_grid[r][c] = TILE_PEN;
            } else {
                s_grid[r][c] = TILE_EMPTY;
            }
        }
    }
    s_dots_remaining = s_dots_total;
    s_fruit_active = 0;
    s_fruit_timer = 0;
}

static void reset_positions(void) {
    /* Pac-Man start: Row 23, Col 13 (aligned to multiple of 6) */
    s_pacman.px = 13 * TILE_SIZE;
    s_pacman.py = 23 * TILE_SIZE;
    s_pacman.cur_dir = DIR_LEFT;
    s_pacman.next_dir = DIR_LEFT;
    s_pacman.anim_frame = 0;
    s_pacman.speed_cnt = 0;

    /* Ghost 0 (Blinky / Red): Chaser - starts outside house */
    s_ghosts[0].px = 13 * TILE_SIZE;
    s_ghosts[0].py = 11 * TILE_SIZE;
    s_ghosts[0].cur_dir = DIR_LEFT;
    s_ghosts[0].state = GSTATE_CHASE;
    s_ghosts[0].color = 2; /* Red */
    s_ghosts[0].pen_timer = 0;
    s_ghosts[0].speed_cnt = 0;

    /* Ghost 1 (Pinky / Yellow-White): Flanker - in house center */
    s_ghosts[1].px = 13 * TILE_SIZE;
    s_ghosts[1].py = 14 * TILE_SIZE;
    s_ghosts[1].cur_dir = DIR_UP;
    s_ghosts[1].state = GSTATE_IN_PEN;
    s_ghosts[1].color = 3; /* Yellow/White */
    s_ghosts[1].pen_timer = 5;
    s_ghosts[1].speed_cnt = 1;

    /* Ghost 2 (Inky / Green-Cyan): Simple Roamer - in house left */
    s_ghosts[2].px = 11 * TILE_SIZE;
    s_ghosts[2].py = 14 * TILE_SIZE;
    s_ghosts[2].cur_dir = DIR_UP;
    s_ghosts[2].state = GSTATE_IN_PEN;
    s_ghosts[2].color = 1; /* Green/Cyan */
    s_ghosts[2].pen_timer = 25;
    s_ghosts[2].speed_cnt = 2;

    /* Ghost 3 (Clyde / Red-Orange): Simple Roamer - in house right */
    s_ghosts[3].px = 15 * TILE_SIZE;
    s_ghosts[3].py = 14 * TILE_SIZE;
    s_ghosts[3].cur_dir = DIR_UP;
    s_ghosts[3].state = GSTATE_IN_PEN;
    s_ghosts[3].color = 2; /* Red/Orange */
    s_ghosts[3].pen_timer = 50;
    s_ghosts[3].speed_cnt = 0;

    s_frightened_timer = 0;
    set_palette(0);
}

/* --------------------------------------------------------------------------
 * Movement Helpers & Collision
 * -------------------------------------------------------------------------- */

static uint8_t is_opposite(uint8_t d1, uint8_t d2) {
    if (d1 == DIR_UP && d2 == DIR_DOWN) return 1;
    if (d1 == DIR_DOWN && d2 == DIR_UP) return 1;
    if (d1 == DIR_LEFT && d2 == DIR_RIGHT) return 1;
    if (d1 == DIR_RIGHT && d2 == DIR_LEFT) return 1;
    return 0;
}

static uint8_t opposite_dir(uint8_t d) {
    if (d == DIR_UP) return DIR_DOWN;
    if (d == DIR_DOWN) return DIR_UP;
    if (d == DIR_LEFT) return DIR_RIGHT;
    if (d == DIR_RIGHT) return DIR_LEFT;
    return DIR_NONE;
}

static uint8_t can_walk(int16_t r, int16_t c) {
    uint8_t t;
    if (r < 0 || r >= MAZE_ROWS) return 0;
    /* Tunnel wrap columns on row 14 */
    if (c < 0 || c >= MAZE_COLS) return (r == 14) ? 1 : 0;

    t = s_grid[r][c];
    if (t == TILE_WALL || t == TILE_DOOR || t == TILE_PEN) return 0;
    return 1;
}

/* Single 2-pixel sub-step for Pac-Man */
static void step_pacman_2px(void) {
    int16_t tile_c = s_pacman.px / TILE_SIZE;
    int16_t tile_r = s_pacman.py / TILE_SIZE;
    int16_t rem_x  = s_pacman.px % TILE_SIZE;
    int16_t rem_y  = s_pacman.py % TILE_SIZE;

    /* Immediate 180 reversal mid-tile */
    if (is_opposite(s_pacman.cur_dir, s_pacman.next_dir)) {
        s_pacman.cur_dir = s_pacman.next_dir;
    }

    /* Check turning and dot consumption at exact tile alignment */
    if (rem_x == 0 && rem_y == 0) {
        if (s_pacman.next_dir != s_pacman.cur_dir && s_pacman.next_dir != DIR_NONE) {
            int16_t nr = tile_r;
            int16_t nc = tile_c;
            if (s_pacman.next_dir == DIR_UP) nr--;
            else if (s_pacman.next_dir == DIR_DOWN) nr++;
            else if (s_pacman.next_dir == DIR_LEFT) nc--;
            else if (s_pacman.next_dir == DIR_RIGHT) nc++;

            if (can_walk(nr, nc)) {
                s_pacman.cur_dir = s_pacman.next_dir;
            }
        }

        /* Check if blocked in current direction */
        {
            int16_t nr = tile_r;
            int16_t nc = tile_c;
            if (s_pacman.cur_dir == DIR_UP) nr--;
            else if (s_pacman.cur_dir == DIR_DOWN) nr++;
            else if (s_pacman.cur_dir == DIR_LEFT) nc--;
            else if (s_pacman.cur_dir == DIR_RIGHT) nc++;

            if (!can_walk(nr, nc)) {
                s_pacman.cur_dir = DIR_NONE;
            }
        }

        /* Dot eating check */
        if (tile_r >= 0 && tile_r < MAZE_ROWS && tile_c >= 0 && tile_c < MAZE_COLS) {
            uint8_t t = s_grid[tile_r][tile_c];
            if (t == TILE_DOT) {
                s_grid[tile_r][tile_c] = TILE_EMPTY;
                s_score += 10;
                s_dots_eaten++;
                s_dots_remaining--;
                s_waka_toggle = !s_waka_toggle;
                sfx_play(s_waka_toggle ? 294 : 587, 1);
                update_score_hud();

                /* Fruit spawn trigger at 70 and 170 dots */
                if (s_dots_eaten == 70 || s_dots_eaten == 170) {
                    try_spawn_fruit();
                }
            } else if (t == TILE_ENERGIZER) {
                uint8_t i;
                s_grid[tile_r][tile_c] = TILE_EMPTY;
                s_score += 50;
                s_dots_eaten++;
                s_dots_remaining--;
                s_frightened_timer = 145; /* ~8 seconds @ 18.2 Hz */
                s_frightened_eaten_count = 0;
                set_palette(1); /* Switch CGA hardware palette to cyan/white */
                sfx_play(440, 3);
                update_score_hud();

                for (i = 0; i < 4; i++) {
                    if (s_ghosts[i].state == GSTATE_CHASE) {
                        s_ghosts[i].state = GSTATE_FRIGHTENED;
                        s_ghosts[i].cur_dir = opposite_dir(s_ghosts[i].cur_dir);
                    }
                }
            }
        }
    }

    /* Advance Pac-Man by STEP_PX (2 pixels) */
    if (s_pacman.cur_dir == DIR_UP) s_pacman.py -= STEP_PX;
    else if (s_pacman.cur_dir == DIR_DOWN) s_pacman.py += STEP_PX;
    else if (s_pacman.cur_dir == DIR_LEFT) s_pacman.px -= STEP_PX;
    else if (s_pacman.cur_dir == DIR_RIGHT) s_pacman.px += STEP_PX;

    /* Tunnel wrapping (maintains exact multiple-of-6 alignment) */
    if (s_pacman.px < 0) {
        s_pacman.px = (MAZE_COLS - 1) * TILE_SIZE;
    } else if (s_pacman.px > (MAZE_COLS - 1) * TILE_SIZE) {
        s_pacman.px = 0;
    }

    /* Clamp vertical bounds defensively */
    if (s_pacman.py < TILE_SIZE) s_pacman.py = TILE_SIZE;
    if (s_pacman.py > (MAZE_ROWS - 2) * TILE_SIZE) s_pacman.py = (MAZE_ROWS - 2) * TILE_SIZE;

    /* Check fruit collection */
    if (s_fruit_active) {
        int16_t fx = s_fruit_c * TILE_SIZE;
        int16_t fy = s_fruit_r * TILE_SIZE;
        int16_t dx = s_pacman.px - fx;
        int16_t dy = s_pacman.py - fy;
        if (dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4) {
            uint16_t pts = FRUIT_POINTS[s_fruit_type];
            s_score += pts;
            s_fruit_active = 0;
            render_tile_background(s_fruit_r, s_fruit_c);
            sfx_play(880, 4);
            update_score_hud();
        }
    }
}

static void update_pacman(void) {
    int16_t old_px = s_pacman.px;
    int16_t old_py = s_pacman.py;
    uint8_t steps, s;

    /* Pac-Man cadence: alternates 1 and 2 steps of 2px per tick
     * Average = 1.5 steps (3 px/tick) = 54.6 px/s = 9.1 tiles/s (arcade speed)
     */
    s_pacman.speed_cnt++;
    steps = (s_pacman.speed_cnt & 1) ? 2 : 1;

    for (s = 0; s < steps; s++) {
        step_pacman_2px();
    }

    if (s_pacman.cur_dir != DIR_NONE) {
        s_pacman.anim_frame = (uint8_t)((s_pacman.speed_cnt) & 1);
    }

    if (old_px != s_pacman.px || old_py != s_pacman.py) {
        restore_entity_region(old_px, old_py);
    }
    draw_pacman();
}

/* --------------------------------------------------------------------------
 * Ghost AI & Movement (Strict 2-Pixel Sub-Stepping)
 * -------------------------------------------------------------------------- */

static uint16_t dist_sq(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
    int16_t dx = x1 - x2;
    int16_t dy = y1 - y2;
    return (uint16_t)(dx * dx + dy * dy);
}

/* Single 2-pixel sub-step for a ghost (guarantees px, py are always even and never miss tile boundaries) */
static void step_ghost_2px(ghost_t *g, uint8_t idx) {
    int16_t tile_c, tile_r, rem_x, rem_y;

    /* In pen behavior */
    if (g->state == GSTATE_IN_PEN) {
        if (s_dots_eaten >= g->pen_timer) {
            g->state = GSTATE_LEAVING_PEN;
        } else {
            /* Bounce up and down inside pen */
            if (g->py <= 13 * TILE_SIZE) g->cur_dir = DIR_DOWN;
            else if (g->py >= 15 * TILE_SIZE) g->cur_dir = DIR_UP;
            if (g->cur_dir == DIR_UP) g->py -= STEP_PX; else g->py += STEP_PX;
            return;
        }
    }

    /* Leaving pen: move horizontally to col 13, then up through door to row 11 */
    if (g->state == GSTATE_LEAVING_PEN) {
        int16_t target_x = 13 * TILE_SIZE;
        int16_t target_y = 11 * TILE_SIZE;

        if (g->px < target_x) g->px += STEP_PX;
        else if (g->px > target_x) g->px -= STEP_PX;
        else if (g->py > target_y) g->py -= STEP_PX;
        else {
            g->px = target_x;
            g->py = target_y;
            g->state = GSTATE_CHASE;
            g->cur_dir = (idx & 1) ? DIR_RIGHT : DIR_LEFT;
        }
        return;
    }

    tile_c = g->px / TILE_SIZE;
    tile_r = g->py / TILE_SIZE;
    rem_x  = g->px % TILE_SIZE;
    rem_y  = g->py % TILE_SIZE;

    /* At exact tile intersection, choose next direction */
    if (rem_x == 0 && rem_y == 0) {
        uint8_t valid[4];
        uint8_t v_count = 0;
        uint8_t d;
        uint8_t best_dir = DIR_NONE;

        /* If eyes reached ghost house entrance (row 11, col 13 or 14), drop into pen and revive */
        if (g->state == GSTATE_EYES && tile_r == 11 && (tile_c == 13 || tile_c == 14)) {
            g->px = 13 * TILE_SIZE;
            g->py = 14 * TILE_SIZE;
            g->state = GSTATE_LEAVING_PEN;
            g->cur_dir = DIR_UP;
            return;
        }

        /* Collect all valid non-reverse open directions */
        for (d = 1; d <= 4; d++) {
            int16_t nr = tile_r, nc = tile_c;
            if (is_opposite(g->cur_dir, d)) continue;
            if (d == DIR_UP) nr--;
            else if (d == DIR_DOWN) nr++;
            else if (d == DIR_LEFT) nc--;
            else if (d == DIR_RIGHT) nc++;

            if (can_walk(nr, nc)) {
                valid[v_count++] = d;
            }
        }

        /* Fallback: if dead-end (should not occur in standard maze, but prevents walking into wall), allow reverse */
        if (v_count == 0) {
            uint8_t rev = opposite_dir(g->cur_dir);
            int16_t nr = tile_r, nc = tile_c;
            if (rev == DIR_UP) nr--;
            else if (rev == DIR_DOWN) nr++;
            else if (rev == DIR_LEFT) nc--;
            else if (rev == DIR_RIGHT) nc++;
            if (can_walk(nr, nc)) {
                valid[v_count++] = rev;
            }
        }

        if (v_count == 1) {
            /* Only one path (corridor or corner): take it immediately */
            best_dir = valid[0];
        } else if (v_count > 1) {
            /* Intersection with multiple choices */
            if (g->state == GSTATE_FRIGHTENED || idx >= 2) {
                /* Frightened ghosts and Ghosts 2 & 3 (Inky & Clyde - simple roamers) pick random open turns */
                best_dir = valid[rand16() % v_count];
            } else {
                /* Targeted AI for Blinky (0: direct chaser) and Pinky (1: 4-tile ahead ambusher) */
                int16_t tx, ty;
                uint16_t min_dist = 0xFFFF;
                uint8_t i;

                if (g->state == GSTATE_EYES) {
                    tx = 13;
                    ty = 11;
                } else {
                    tx = s_pacman.px / TILE_SIZE;
                    ty = s_pacman.py / TILE_SIZE;
                    if (idx == 1) {
                        /* Pinky aims 4 tiles ahead of Pac-Man (with occasional random turn so she doesn't lock with Blinky) */
                        if ((rand16() & 3) == 0) {
                            best_dir = valid[rand16() % v_count];
                        }
                        if (s_pacman.cur_dir == DIR_UP) ty -= 4;
                        else if (s_pacman.cur_dir == DIR_DOWN) ty += 4;
                        else if (s_pacman.cur_dir == DIR_LEFT) tx -= 4;
                        else if (s_pacman.cur_dir == DIR_RIGHT) tx += 4;
                    }
                }

                if (best_dir == DIR_NONE) {
                    for (i = 0; i < v_count; i++) {
                        uint8_t cand = valid[i];
                        int16_t nr = tile_r, nc = tile_c;
                        uint16_t dst;
                        if (cand == DIR_UP) nr--;
                        else if (cand == DIR_DOWN) nr++;
                        else if (cand == DIR_LEFT) nc--;
                        else if (cand == DIR_RIGHT) nc++;

                        dst = dist_sq(nc, nr, tx, ty);
                        if (dst < min_dist) {
                            min_dist = dst;
                            best_dir = cand;
                        }
                    }
                }
            }
        }

        if (best_dir != DIR_NONE) {
            g->cur_dir = best_dir;
        } else {
            g->cur_dir = DIR_NONE;
        }
    }

    /* Advance ghost by STEP_PX (2 pixels) */
    if (g->cur_dir == DIR_UP) g->py -= STEP_PX;
    else if (g->cur_dir == DIR_DOWN) g->py += STEP_PX;
    else if (g->cur_dir == DIR_LEFT) g->px -= STEP_PX;
    else if (g->cur_dir == DIR_RIGHT) g->px += STEP_PX;

    /* Tunnel wrap (maintains exact multiple-of-6 alignment) */
    if (g->px < 0) {
        g->px = (MAZE_COLS - 1) * TILE_SIZE;
    } else if (g->px > (MAZE_COLS - 1) * TILE_SIZE) {
        g->px = 0;
    }

    /* Defensive bounds clamp */
    if (g->py < TILE_SIZE) g->py = TILE_SIZE;
    if (g->py > (MAZE_ROWS - 2) * TILE_SIZE) g->py = (MAZE_ROWS - 2) * TILE_SIZE;
}

static void update_ghost(ghost_t *g, uint8_t idx) {
    int16_t old_px = g->px;
    int16_t old_py = g->py;
    int16_t tile_c = g->px / TILE_SIZE;
    int16_t tile_r = g->py / TILE_SIZE;
    uint8_t steps  = 1;
    uint8_t s;

    g->speed_cnt++;

    /* Ghost speed cadence (all steps are 2-pixel sub-steps):
     * - Eyes: 3 sub-steps per tick (6 px/tick = 18.2 tiles/s)
     * - Frightened or in tunnel: 1 sub-step on 2 out of 3 ticks (1.33 px/tick = 4.04 tiles/s)
     * - Normal Chase: 1 sub-step on 2 ticks, 2 sub-steps on every 3rd tick (2.67 px/tick = 8.08 tiles/s,
     *   slightly slower than Pac-Man's 9.1 tiles/s so Pac-Man can outrun ghosts on straight corridors)
     */
    if (g->state == GSTATE_EYES) {
        steps = 3;
    } else if (g->state == GSTATE_FRIGHTENED || (tile_r == 14 && (tile_c < 5 || tile_c > 22))) {
        steps = ((g->speed_cnt % 3) == 0) ? 0 : 1;
    } else if (g->state == GSTATE_IN_PEN || g->state == GSTATE_LEAVING_PEN) {
        steps = 1;
    } else {
        steps = ((g->speed_cnt % 3) == 0) ? 2 : 1;
    }

    for (s = 0; s < steps; s++) {
        step_ghost_2px(g, idx);
    }

    if (old_px != g->px || old_py != g->py) {
        restore_entity_region(old_px, old_py);
    }
    draw_ghost(g);
}

/* --------------------------------------------------------------------------
 * Collision Check & Life Handling
 * -------------------------------------------------------------------------- */

static int check_collisions(void) {
    uint8_t i;
    for (i = 0; i < 4; i++) {
        ghost_t *g = &s_ghosts[i];
        int16_t dx = s_pacman.px - g->px;
        int16_t dy = s_pacman.py - g->py;

        if (dx >= -4 && dx <= 4 && dy >= -4 && dy <= 4) {
            if (g->state == GSTATE_FRIGHTENED) {
                /* Ghost eaten! */
                uint16_t pts = (uint16_t)(200 << s_frightened_eaten_count);
                if (s_frightened_eaten_count < 3) s_frightened_eaten_count++;
                s_score += pts;
                g->state = GSTATE_EYES;
                sfx_play(784, 4);
                update_score_hud();
            } else if (g->state == GSTATE_CHASE) {
                /* Pac-Man caught! */
                return 1;
            }
        }
    }
    return 0;
}

static void play_death_animation(void) {
    uint16_t freq;
    stop_audio();

    /* Chromatic descending sound synchronized to hardware ticks */
    for (freq = 900; freq >= 150; freq -= 75) {
        uint16_t t0;
        if (s_sound_on) {
            os_sound_tone(freq, 0, 2);
        }
        t0 = GET_TICKS();
        while (GET_TICKS() == t0) {
        }
    }
    os_sound_stop();
}

/* --------------------------------------------------------------------------
 * Game Loop & User Entry Point
 * -------------------------------------------------------------------------- */

int app_main(void) {
    const char *args = os_get_args();
    uint16_t frame_ticks = 0;
    uint16_t last_tick;
    uint8_t paused = 0;
    uint8_t game_running = 1;
    uint8_t auto_test = 0;

    if (args) {
        while (*args == ' ') args++;
        if (*args == '/' || *args == '-') {
            if (args[1] == 't' || args[1] == 'T') auto_test = 1;
        }
    }

    /* Initialize PRNG seed from system BDA timer */
    s_prng_seed = GET_TICKS();
    if (s_prng_seed == 0) s_prng_seed = 0x5AA5;

    load_high_score();
    init_music();

    /* Switch to authentic CGA Mode 4 (320x200, 4-color) */
    os_gfx_set_mode(OS_MODE_CGA_320x200);
    set_palette(0);

new_game:
    s_score = 0;
    s_level = 0;
    s_lives = 3;

    init_maze();
    reset_positions();
    render_full_screen();
    draw_pacman();
    {
        uint8_t g;
        for (g = 0; g < 4; g++) draw_ghost(&s_ghosts[g]);
    }

    /* Wait for player to press SPACE before starting the game */
    if (!auto_test) {
        draw_prompt_box("PRESS SPACE", 3, "  TO PLAY  ", 1);
        for (;;) {
            if (os_kbhit()) {
                uint16_t k = os_getc();
                uint8_t ch = (uint8_t)(k & 0xFF);
                if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                    game_running = 0;
                    goto cleanup;
                }
                if (ch == 'm' || ch == 'M') {
                    s_sound_on = !s_sound_on;
                    if (!s_sound_on) stop_audio();
                }
                if (ch == KEY_SPACE || ch == 0x0D) {
                    break;
                }
            }
        }
        /* Restore maze tiles underneath the prompt box and redraw entities */
        restore_banner_region(13, 19, 4, 23);
        draw_pacman();
        {
            uint8_t g;
            for (g = 0; g < 4; g++) draw_ghost(&s_ghosts[g]);
        }
    }

next_level:
    if (s_level > 0 || s_dots_eaten > 0) {
        init_maze();
        reset_positions();
        render_full_screen();
        draw_pacman();
        {
            uint8_t g;
            for (g = 0; g < 4; g++) draw_ghost(&s_ghosts[g]);
        }
    }

    play_intro_tune();

    /* Ready prompt delay (exact hardware timer delay) */
    cga_text_aligned(MAZE_X + 60, MAZE_Y + 17 * TILE_SIZE - 1, "READY!", 3, 0);
    {
        uint16_t t0 = GET_TICKS();
        uint16_t wait_ticks = auto_test ? 6 : 55; /* ~3.0 seconds while intro plays */
        while ((uint16_t)(GET_TICKS() - t0) < wait_ticks) {
            if (os_kbhit()) {
                uint16_t k = os_getc();
                uint8_t ch = (uint8_t)(k & 0xFF);
                if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                    game_running = 0;
                    goto cleanup;
                }
                if (ch == 'm' || ch == 'M') {
                    s_sound_on = !s_sound_on;
                    if (!s_sound_on) stop_audio();
                }
            }
        }
    }
    /* Clear READY! across rows 16..18 and cols 9..19 so 8x8 font overhang is fully erased */
    restore_banner_region(16, 18, 9, 19);

    last_tick = GET_TICKS();

    /* Main play loop synchronized to hardware BDA ticks (18.2 Hz) */
    while (game_running) {
        uint16_t now = GET_TICKS();
        uint16_t dt = now - last_tick;

        /* Process keyboard input immediately on every loop spin */
        while (os_kbhit()) {
            uint16_t key = os_getc();
            uint8_t sc = (uint8_t)(key >> 8);
            uint8_t ch = (uint8_t)(key & 0xFF);

            if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                game_running = 0;
                goto cleanup;
            }
            if (ch == 'p' || ch == 'P') {
                paused = !paused;
                if (paused) {
                    cga_text_aligned(MAZE_X + 56, MAZE_Y + 17 * TILE_SIZE - 1, "[PAUSE]", 3, 0);
                } else {
                    restore_banner_region(16, 18, 8, 19);
                    draw_pacman();
                    {
                        uint8_t g;
                        for (g = 0; g < 4; g++) draw_ghost(&s_ghosts[g]);
                    }
                    last_tick = GET_TICKS();
                }
                continue;
            }
            if (ch == 'm' || ch == 'M') {
                s_sound_on = !s_sound_on;
                if (!s_sound_on) stop_audio();
                continue;
            }

            if (!paused) {
                if (sc == SC_UP || ch == 'w' || ch == 'W') s_pacman.next_dir = DIR_UP;
                else if (sc == SC_DOWN || ch == 's' || ch == 'S') s_pacman.next_dir = DIR_DOWN;
                else if (sc == SC_LEFT || ch == 'a' || ch == 'A') s_pacman.next_dir = DIR_LEFT;
                else if (sc == SC_RIGHT || ch == 'd' || ch == 'D') s_pacman.next_dir = DIR_RIGHT;
            }
        }

        /* Wait for next 18.2 Hz timer tick */
        if (dt == 0) {
            continue;
        }

        last_tick = now;

        if (paused) continue;

        frame_ticks++;
        if (auto_test && frame_ticks >= 36) { /* 2 seconds of automated test */
            game_running = 0;
            goto cleanup;
        }

        /* Audio & timer update */
        sfx_update();
        update_fruit();

        /* Frightened timer */
        if (s_frightened_timer > 0) {
            s_frightened_timer--;
            if (s_frightened_timer == 0) {
                uint8_t g;
                set_palette(0); /* Restore normal colors */
                for (g = 0; g < 4; g++) {
                    if (s_ghosts[g].state == GSTATE_FRIGHTENED) {
                        s_ghosts[g].state = GSTATE_CHASE;
                    }
                }
            }
        }

        /* Update Entities */
        update_pacman();
        if (check_collisions()) goto handle_death;

        {
            uint8_t g;
            for (g = 0; g < 4; g++) {
                update_ghost(&s_ghosts[g], g);
            }
        }

        /* Check collision after ghost movement */
        if (check_collisions()) {
handle_death:
            play_death_animation();
            if (s_lives > 1) {
                s_lives--;
                reset_positions();
                render_full_screen();
                draw_pacman();
                {
                    uint8_t g;
                    for (g = 0; g < 4; g++) draw_ghost(&s_ghosts[g]);
                }
                /* Brief READY! pause before resuming life */
                cga_text_aligned(MAZE_X + 60, MAZE_Y + 17 * TILE_SIZE - 1, "READY!", 3, 0);
                {
                    uint16_t t0 = GET_TICKS();
                    while ((uint16_t)(GET_TICKS() - t0) < 25) {
                        if (os_kbhit()) {
                            uint16_t k = os_getc();
                            uint8_t ch = (uint8_t)(k & 0xFF);
                            if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                                game_running = 0;
                                goto cleanup;
                            }
                        }
                    }
                }
                restore_banner_region(16, 18, 9, 19);
                last_tick = GET_TICKS();
                continue;
            } else {
                /* Game Over: wait for player to press Space/Enter or Esc/Q */
                s_lives = 0;
                update_hud();
                save_high_score();
                draw_prompt_box(" GAME OVER ", 2, "PRESS SPACE", 3);
                for (;;) {
                    if (os_kbhit()) {
                        uint16_t k = os_getc();
                        uint8_t ch = (uint8_t)(k & 0xFF);
                        if (ch == KEY_ESC || ch == 'q' || ch == 'Q') {
                            game_running = 0;
                            goto cleanup;
                        }
                        if (ch == KEY_SPACE || ch == 0x0D) {
                            goto new_game;
                        }
                    }
                }
            }
        }

        /* Check level clear */
        if (s_dots_remaining == 0) {
            uint8_t f;
            stop_audio();
            /* Flash maze */
            for (f = 0; f < 6; f++) {
                uint16_t t0;
                set_palette(f & 1);
                t0 = GET_TICKS();
                while ((uint16_t)(GET_TICKS() - t0) < 3) {
                }
            }
            set_palette(0);
            s_level++;
            goto next_level;
        }
    }

cleanup:
    save_high_score();
    stop_audio();
    if (s_imf_seg) {
        os_mem_free(s_imf_seg);
        s_imf_seg = 0;
    }
    /* Restore standard 80x25 text mode */
    os_gfx_set_mode(OS_MODE_TEXT_80x25);
    return 0;
}
