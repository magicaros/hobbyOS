/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CGA 80x25 text screen (with hardware retrace synchronization and differential
 * RAM shadow buffer to eliminate CGA snow), PC/XT keyboard, and COM1 init.
 */

#include "kernel.h"

#define CGA_COLS      80
#define CGA_ROWS      25
#define CGA_CELLS     (CGA_COLS * CGA_ROWS)

static uint8_t  cur_row  = 0;
static uint8_t  cur_col  = 0;
static uint8_t  cur_attr = ATTR_DEFAULT;
static uint8_t  cursor_visible = 1;
static uint8_t  scroll_bottom = CGA_ROWS;
static uint16_t cga_shadow[CGA_CELLS];

static uint16_t active_video_type = VIDEO_TYPE_CGA;
static uint16_t crtc_index_port   = 0x03D4;
static uint16_t crtc_data_port    = 0x03D5;

uint16_t video_detect_type(void) {
    uint16_t vga_dcc = bios_get_vga_dcc();
    uint16_t ega_info;
    uint8_t mode;
    volatile uint16_t __far *bda_equip;
    volatile uint16_t __far *bda_crtc;

    /* 1. Check for VGA via Display Combination Code (AX=0x1A00) */
    if ((vga_dcc & 0xFF) == 0x1A) {
        return VIDEO_TYPE_VGA;
    }

    /* 2. Check for EGA (AH=0x12, BL=0x10) */
    ega_info = bios_get_ega_info();
    if ((ega_info & 0xFF) != 0x10) {
        return VIDEO_TYPE_EGA;
    }

    /* 3. Distinguish CGA vs MDA */
    mode = bios_get_video_mode();
    if (mode == 0x07) {
        return VIDEO_TYPE_MDA;
    }

    /* Check BIOS Data Area Equipment Word at 0040:0010 (bits 4-5) */
    bda_equip = (volatile uint16_t __far *)MK_FP(0x0040, 0x0010);
    if (((*bda_equip >> 4) & 0x03) == 0x03) {
        return VIDEO_TYPE_MDA;
    }

    /* Check BDA CRTC port at 0040:0063 */
    bda_crtc = (volatile uint16_t __far *)MK_FP(0x0040, 0x0063);
    if (*bda_crtc == 0x03B4) {
        return VIDEO_TYPE_MDA;
    }

    return VIDEO_TYPE_CGA;
}

uint16_t video_get_type(void) {
    return active_video_type;
}

const char *video_get_name(void) {
    return os_video_name(active_video_type);
}

static uint8_t translate_attr(uint8_t attr) {
    if (active_video_type != VIDEO_TYPE_MDA) {
        return attr;
    }
    /*
     * MDA attribute mapping:
     * 0x00 = Non-display / hidden
     * 0x01 = Underline
     * 0x07 = Normal white on black
     * 0x0F = High intensity / Bold
     * 0x70 = Reverse video
     */
    if ((attr & 0x0F) == 0) {
        return 0x00;
    }
    if ((attr & 0x70) != 0) {
        return 0x70; /* Reverse video if background is non-black */
    }
    if ((attr & 0x08) != 0 || (attr & 0x07) >= 0x03) {
        return 0x0F; /* High intensity / Bold */
    }
    if ((attr & 0x07) == 0x01) {
        return 0x01; /* Blue -> Underline */
    }
    return 0x07;
}

static uint8_t serial_console_enabled = 1;

void serial_init(void) {
    serial_console_enabled = 1;
    outb(COM1_BASE + 1, 0x00); /* Disable interrupts */
    outb(COM1_BASE + 3, 0x80); /* Enable DLAB */
    outb(COM1_BASE + 0, 0x0C); /* 9600 baud (divisor 12) */
    outb(COM1_BASE + 1, 0x00);
    outb(COM1_BASE + 3, 0x03); /* 8N1 */
    outb(COM1_BASE + 4, 0x03); /* DTR + RTS */
}

void serial_enable(void) {
    serial_console_enabled = 1;
}

void serial_disable(void) {
    serial_console_enabled = 0;
}

static void serial_putc(char c) {
    uint16_t timeout = 5000;
    if (!serial_console_enabled) {
        return;
    }
    while ((inb(COM1_BASE + 5) & 0x20) == 0 && --timeout > 0);
    outb(COM1_BASE, (uint8_t)c);
}

static void serial_puts(const char *s) {
    if (!serial_console_enabled) {
        return;
    }
    while (*s) {
        serial_putc(*s++);
    }
}

void con_set_cell(uint16_t idx, uint16_t val) {
    if (idx < CGA_CELLS) {
        cga_shadow[idx] = val;
        cga_write_cell(idx, val);
    }
}

uint16_t con_get_cell(uint16_t idx) {
    if (idx < CGA_CELLS) {
        return cga_shadow[idx];
    }
    return 0;
}

static void put_cell(uint16_t idx, uint16_t val) {
    if (cga_shadow[idx] != val) {
        cga_shadow[idx] = val;
        cga_write_cell(idx, val);
    }
}

static void update_hw_cursor(void) {
    uint16_t pos;
    volatile uint8_t __far *bda_cursor = (volatile uint8_t __far *)MK_FP(0x0040, 0x0050);

    bda_cursor[0] = cur_col;
    bda_cursor[1] = cur_row;

    if (!cursor_visible) {
        /* Park cursor off-screen at line 25 (offset 2000) */
        pos = CGA_CELLS;
    } else {
        pos = (uint16_t)cur_row * CGA_COLS + cur_col;
    }

    outb(crtc_index_port, 0x0E);
    outb(crtc_data_port, (uint8_t)(pos >> 8));
    outb(crtc_index_port, 0x0F);
    outb(crtc_data_port, (uint8_t)(pos & 0xFF));
}

static void scroll_up(void) {
    uint16_t blank = ((uint16_t)cur_attr << 8) | ' ';
    uint16_t i;
    uint16_t limit = (uint16_t)(scroll_bottom - 1) * CGA_COLS;
    uint16_t clear_end = (uint16_t)scroll_bottom * CGA_COLS;

    for (i = 0; i < limit; i++) {
        cga_shadow[i] = cga_shadow[i + CGA_COLS];
    }
    for (i = limit; i < clear_end; i++) {
        cga_shadow[i] = blank;
    }

    bios_scroll_up(1, cur_attr, 0, 0, (uint8_t)(scroll_bottom - 1), (uint8_t)(CGA_COLS - 1));
}

void con_clear(uint8_t attr) {
    uint16_t blank;
    uint16_t i;
    uint16_t clear_end = (uint16_t)scroll_bottom * CGA_COLS;

    if (attr != 0) {
        cur_attr = translate_attr(attr);
    }
    blank = ((uint16_t)cur_attr << 8) | ' ';
    for (i = 0; i < clear_end; i++) {
        cga_shadow[i] = blank;
    }
    cga_sync_copy(cga_shadow, clear_end);
    cur_row = 0;
    cur_col = 0;
    update_hw_cursor();
    serial_puts("\033[2J\033[H");
}

void con_init(void) {
    active_video_type = video_detect_type();

    if (active_video_type == VIDEO_TYPE_MDA) {
        video_vram_seg    = 0xB000;
        video_has_snow    = 0;
        video_status_port = 0x03BA;
        crtc_index_port   = 0x03B4;
        crtc_data_port    = 0x03B5;
    } else if (active_video_type == VIDEO_TYPE_CGA) {
        video_vram_seg    = 0xB800;
        video_has_snow    = 1;
        video_status_port = 0x03DA;
        crtc_index_port   = 0x03D4;
        crtc_data_port    = 0x03D5;
    } else {
        /* EGA or VGA */
        video_vram_seg    = 0xB800;
        video_has_snow    = 0;
        video_status_port = 0x03DA;
        crtc_index_port   = 0x03D4;
        crtc_data_port    = 0x03D5;
    }

    cur_attr = translate_attr(ATTR_DEFAULT);
    cursor_visible = 1;
    scroll_bottom = CGA_ROWS;
    con_clear(cur_attr);
}

void con_set_attr(uint8_t attr) {
    cur_attr = translate_attr(attr);
}

void con_set_cursor(uint8_t row, uint8_t col) {
    if (row >= scroll_bottom) row = scroll_bottom - 1;
    if (col >= CGA_COLS) col = CGA_COLS - 1;
    cur_row = row;
    cur_col = col;
    update_hw_cursor();
}

void con_set_scroll_rows(uint8_t rows) {
    if (rows == 0 || rows > CGA_ROWS) {
        scroll_bottom = CGA_ROWS;
    } else {
        scroll_bottom = rows;
    }
}

uint8_t con_get_scroll_rows(void) {
    return scroll_bottom;
}

void con_write_cell(uint8_t row, uint8_t col, uint16_t val) {
    if (row < CGA_ROWS && col < CGA_COLS) {
        put_cell((uint16_t)row * CGA_COLS + col, val);
    }
}

void con_get_cursor(uint8_t *row, uint8_t *col) {
    if (row) *row = cur_row;
    if (col) *col = cur_col;
}

void con_set_cursor_visible(uint8_t visible) {
    cursor_visible = visible ? 1 : 0;
    update_hw_cursor();
}

void con_get_shadow(uint16_t __far *dst) {
    uint16_t i;
    if (!dst) return;
    for (i = 0; i < CGA_CELLS; i++) {
        dst[i] = cga_shadow[i];
    }
}

void con_sync_shadow(const uint16_t __far *src, uint8_t row, uint8_t col) {
    uint16_t i;
    if (src) {
        for (i = 0; i < CGA_CELLS; i++) {
            cga_shadow[i] = src[i];
        }
    }
    con_set_cursor(row, col);
}

void con_redraw(void) {
    cga_sync_copy(cga_shadow, CGA_CELLS);
}

static void putc_no_cursor(char c) {
    serial_putc(c);
    if (c == '\r') {
        cur_col = 0;
    } else if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            put_cell((uint16_t)cur_row * CGA_COLS + cur_col,
                     ((uint16_t)cur_attr << 8) | ' ');
        }
    } else if (c == '\t') {
        do {
            putc_no_cursor(' ');
        } while ((cur_col & 3) != 0);
        return;
    } else {
        put_cell((uint16_t)cur_row * CGA_COLS + cur_col,
                 ((uint16_t)cur_attr << 8) | (uint8_t)c);
        cur_col++;
        if (cur_col >= CGA_COLS) {
            cur_col = 0;
            cur_row++;
        }
    }

    if (cur_row >= scroll_bottom) {
        scroll_up();
        cur_row = scroll_bottom - 1;
    }
}

void con_putc(char c) {
    putc_no_cursor(c);
    update_hw_cursor();
}

void con_puts(const char *s) {
    while (*s) {
        putc_no_cursor(*s++);
    }
    update_hw_cursor();
}

void con_puts_far(const char __far *s) {
    while (*s) {
        putc_no_cursor(*s++);
    }
    update_hw_cursor();
}

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

void con_put_uint(uint32_t val) {
    char buf[12];
    int i = 0;

    if (val == 0) {
        con_putc('0');
        return;
    }
    while (val > 0 && i < 11) {
        buf[i++] = (char)('0' + u32_div10(&val));
    }
    while (i > 0) {
        putc_no_cursor(buf[--i]);
    }
    update_hw_cursor();
}

void con_put_hex16(uint16_t val) {
    static const char hex[] = "0123456789ABCDEF";
    putc_no_cursor(hex[(val >> 12) & 0x0F]);
    putc_no_cursor(hex[(val >> 8) & 0x0F]);
    putc_no_cursor(hex[(val >> 4) & 0x0F]);
    putc_no_cursor(hex[val & 0x0F]);
    update_hw_cursor();
}

int con_kbhit(void) {
    return kbd_kbhit();
}

uint16_t con_getc(void) {
    return kbd_getc();
}

uint16_t con_getline(char *buf, uint16_t max_len) {
    uint16_t len = 0;

    if (max_len == 0) {
        return 0;
    }

    for (;;) {
        uint16_t key = con_getc();
        uint8_t  u   = (uint8_t)(key & 0xFF);
        char     c   = (char)u;

        if (c == '\r' || c == '\n') {
            con_putc('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b' || u == 0x7F) {
            if (len > 0) {
                len--;
                con_putc('\b');
            }
        } else if (u >= 0x20 && u != 0x7F) {
            if (len + 1 < max_len) {
                buf[len++] = c;
                con_putc(c);
            }
        }
    }
}
