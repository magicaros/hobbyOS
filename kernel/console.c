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
#define CRTC_INDEX    0x3D4
#define CRTC_DATA     0x3D5

static uint8_t  cur_row  = 0;
static uint8_t  cur_col  = 0;
static uint8_t  cur_attr = ATTR_DEFAULT;
static uint8_t  cursor_visible = 1;
static uint16_t cga_shadow[CGA_CELLS];

void serial_init(void) {
    outb(COM1_BASE + 1, 0x00); /* Disable interrupts */
    outb(COM1_BASE + 3, 0x80); /* Enable DLAB */
    outb(COM1_BASE + 0, 0x0C); /* 9600 baud (divisor 12) */
    outb(COM1_BASE + 1, 0x00);
    outb(COM1_BASE + 3, 0x03); /* 8N1 */
    outb(COM1_BASE + 4, 0x03); /* DTR + RTS */
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

    outb(CRTC_INDEX, 0x0E);
    outb(CRTC_DATA, (uint8_t)(pos >> 8));
    outb(CRTC_INDEX, 0x0F);
    outb(CRTC_DATA, (uint8_t)(pos & 0xFF));
}

static void scroll_up(void) {
    uint16_t blank = ((uint16_t)cur_attr << 8) | ' ';
    uint16_t i;

    for (i = 0; i < (CGA_ROWS - 1) * CGA_COLS; i++) {
        uint16_t next_val = cga_shadow[i + CGA_COLS];
        if (cga_shadow[i] != next_val) {
            cga_shadow[i] = next_val;
            cga_write_cell(i, next_val);
        }
    }
    for (i = (CGA_ROWS - 1) * CGA_COLS; i < CGA_CELLS; i++) {
        if (cga_shadow[i] != blank) {
            cga_shadow[i] = blank;
            cga_write_cell(i, blank);
        }
    }
}

void con_clear(uint8_t attr) {
    uint16_t blank;
    uint16_t i;

    if (attr != 0) {
        cur_attr = attr;
    }
    blank = ((uint16_t)cur_attr << 8) | ' ';
    for (i = 0; i < CGA_CELLS; i++) {
        cga_shadow[i] = blank;
    }
    cga_sync_copy(cga_shadow, CGA_CELLS);
    cur_row = 0;
    cur_col = 0;
    update_hw_cursor();
}

void con_init(void) {
    cur_attr = ATTR_DEFAULT;
    cursor_visible = 1;
    con_clear(ATTR_DEFAULT);
}

void con_set_attr(uint8_t attr) {
    cur_attr = attr;
}

void con_set_cursor(uint8_t row, uint8_t col) {
    if (row >= CGA_ROWS) row = CGA_ROWS - 1;
    if (col >= CGA_COLS) col = CGA_COLS - 1;
    cur_row = row;
    cur_col = col;
    update_hw_cursor();
}

void con_get_cursor(uint8_t *row, uint8_t *col) {
    if (row) *row = cur_row;
    if (col) *col = cur_col;
}

void con_set_cursor_visible(uint8_t visible) {
    cursor_visible = visible ? 1 : 0;
    update_hw_cursor();
}

static void putc_no_cursor(char c) {
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

    if (cur_row >= CGA_ROWS) {
        scroll_up();
        cur_row = CGA_ROWS - 1;
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
