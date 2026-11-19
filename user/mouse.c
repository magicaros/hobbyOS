/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mouse status and live interactive testing utility (/BIN/MOUSE.BIN).
 */

#include "hobbyos.h"

static void print_uint16(uint16_t val) {
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

static void print_uint16_pad(uint16_t val, uint16_t width) {
    char buf[8];
    int len = 0;
    uint16_t temp = val;
    uint16_t p;

    if (val == 0) {
        buf[len++] = '0';
    } else {
        while (temp > 0) {
            buf[len++] = (char)('0' + (temp % 10));
            temp /= 10;
        }
    }
    for (p = len; p < width; p++) {
        os_putc(' ');
    }
    while (len > 0) {
        os_putc(buf[--len]);
    }
}

int app_main(void) {
    const char *args = os_get_args();
    uint16_t buttons = 0;
    uint16_t status;
    int status_only = 0;

    if (args) {
        while (*args == ' ') args++;
        if (args[0] == '/' || args[0] == '-') {
            if (args[1] == 's' || args[1] == 'S') {
                status_only = 1;
            }
        }
    }

    status = os_mouse_reset(&buttons);

    if (status_only) {
        os_puts("hobbyOS Mouse Driver Status (INT 0x33):\n");
        os_puts("  Driver Status:  ");
        if (status == 0xFFFF) {
            os_puts("Installed & Active\n");
            os_puts("  Buttons:        ");
            print_uint16(buttons);
            os_putc('\n');
            {
                uint16_t x, y, b;
                b = os_mouse_get_pos(&x, &y);
                os_puts("  Position:       (");
                print_uint16(x);
                os_puts(", ");
                print_uint16(y);
                os_puts(")\n");
            }
        } else {
            os_puts("Not detected\n");
        }
        return 0;
    }

    if (status != 0xFFFF) {
        os_puts("mouse: No mouse detected via INT 0x33.\n");
        return 1;
    }

    os_puts("hobbyOS Mouse Test (/BIN/MOUSE.BIN)\n");
    os_puts("Move mouse and click buttons. Press any key to exit.\n\n");

    os_mouse_show();

    {
        uint16_t last_x = 0xFFFF;
        uint16_t last_y = 0xFFFF;
        uint16_t last_b = 0xFFFF;

        while (!os_kbhit()) {
            uint16_t x, y, b;
            b = os_mouse_get_pos(&x, &y);

            if (x != last_x || y != last_y || b != last_b) {
                last_x = x;
                last_y = y;
                last_b = b;

                os_puts("\r  X: ");
                print_uint16_pad(x, 4);
                os_puts("  Y: ");
                print_uint16_pad(y, 4);
                os_puts("  Buttons: [");
                os_puts((b & 0x01) ? "LEFT " : "     ");
                os_puts((b & 0x04) ? "MID " : "    ");
                os_puts((b & 0x02) ? "RIGHT" : "     ");
                os_puts("]   ");
            }
        }
        os_getc(); /* Consume exit key */
    }

    os_mouse_hide();
    os_puts("\n\nMouse test complete.\n");
    return 0;
}
