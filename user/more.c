/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MORE.BIN - Terminal pager utility for hobbyOS.
 */

#include "hobbyos.h"

#define SCREEN_ROWS 24
#define SCREEN_COLS 80
#define CHUNK_SIZE  512

static char s_read_buf[CHUNK_SIZE];

static uint8_t calc_pct(uint32_t current, uint32_t total) {
    if (total == 0 || current >= total) return 100;
    while (total > 600) {
        current >>= 2;
        total >>= 2;
    }
    if (total == 0) return 100;
    return (uint8_t)(((uint16_t)current * 100) / (uint16_t)total);
}

static void print_uint(uint16_t val) {
    char buf[8];
    int i = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) {
        os_putc(buf[--i]);
    }
}

static int wait_for_user(uint8_t pct, int show_pct) {
    uint16_t key;
    uint8_t ch;

    /* Display -- More -- prompt */
    os_puts("-- More");
    if (show_pct) {
        os_puts(" (");
        print_uint((uint16_t)pct);
        os_puts("%)");
    }
    os_puts(" --");

    /* Wait for direct keystroke, bypassing any redirected stdin */
    for (;;) {
        key = os_keyb_getc_direct();
        ch = (uint8_t)(key & 0xFF);
        if (ch == ' ' || ch == '\r' || ch == '\n' ||
            ch == 'q' || ch == 'Q' || ch == 0x1B || ch == 0x03) {
            break;
        }
    }

    /* Erase prompt by returning to start of line and overwriting with spaces */
    os_puts("\r                \r");

    if (ch == 'q' || ch == 'Q' || ch == 0x1B || ch == 0x03) {
        return -1; /* Abort */
    }
    if (ch == '\r' || ch == '\n') {
        return 1; /* Next line */
    }
    return 0; /* Next page */
}

int app_main(void) {
    const char *args = os_get_args();
    int row = 0;
    int col = 0;

    if (args) {
        while (*args == ' ') args++;
    }

    if (args && *args != '\0') {
        /* File mode: page specified file */
        char filename[64];
        int flen = 0;
        int32_t fsize;
        uint32_t offset = 0;

        while (*args && *args != ' ' && flen < 63) {
            filename[flen++] = *args++;
        }
        filename[flen] = '\0';

        if (filename[0] == '/' && (filename[1] == '?' || filename[1] == 'h' || filename[1] == 'H')) {
            os_puts("Usage: more [filename] | <command> | more\n");
            return 0;
        }

        fsize = os_file_size(filename);
        if (fsize < 0) {
            os_puts("more: Cannot open '");
            os_puts(filename);
            os_puts("'\n");
            return 1;
        }
        if (fsize == 0) {
            return 0;
        }

        while (offset < (uint32_t)fsize) {
            int32_t rd = os_file_read_offset(filename, (void __far *)MK_FP(get_ds(), s_read_buf), CHUNK_SIZE, offset);
            int i;
            if (rd <= 0) break;
            offset += (uint16_t)rd;

            for (i = 0; i < (int)rd; i++) {
                char c = s_read_buf[i];

                if (c == '\r') {
                    col = 0;
                    continue;
                }
                if (c == '\n') {
                    os_putc('\n');
                    col = 0;
                    row++;
                } else if (c == '\t') {
                    int next_tab = (col + 8) & ~7;
                    while (col < next_tab && col < SCREEN_COLS) {
                        os_putc(' ');
                        col++;
                    }
                } else if (c == '\b') {
                    if (col > 0) col--;
                    os_putc('\b');
                } else {
                    os_putc(c);
                    col++;
                    if (col >= SCREEN_COLS) {
                        col = 0;
                        row++;
                    }
                }

                if (row >= SCREEN_ROWS) {
                    uint8_t pct = 0;
                    int action;
                    if (fsize > 0) {
                        uint32_t cur = offset - (rd - i);
                        pct = calc_pct(cur, (uint32_t)fsize);
                    }
                    action = wait_for_user(pct, 1);
                    if (action < 0) {
                        return 0; /* Abort */
                    }
                    if (action == 1) {
                        row = SCREEN_ROWS - 1; /* advance 1 line */
                    } else {
                        row = 0; /* advance full page */
                    }
                }
            }
        }
        return 0;
    } else {
        /* Stream / pipe mode: read from standard input */
        for (;;) {
            uint16_t val = os_getc();
            char c;
            if (val == 0xFFFF) {
                break; /* EOF */
            }
            c = (char)(val & 0xFF);

            if (c == '\r') {
                col = 0;
                continue;
            }
            if (c == '\n') {
                os_putc('\n');
                col = 0;
                row++;
            } else if (c == '\t') {
                int next_tab = (col + 8) & ~7;
                while (col < next_tab && col < SCREEN_COLS) {
                    os_putc(' ');
                    col++;
                }
            } else if (c == '\b') {
                if (col > 0) col--;
                os_putc('\b');
            } else {
                os_putc(c);
                col++;
                if (col >= SCREEN_COLS) {
                    col = 0;
                    row++;
                }
            }

            if (row >= SCREEN_ROWS) {
                int action = wait_for_user(0, 0);
                if (action < 0) {
                    return 0; /* Abort */
                }
                if (action == 1) {
                    row = SCREEN_ROWS - 1;
                } else {
                    row = 0;
                }
            }
        }
        return 0;
    }
}
