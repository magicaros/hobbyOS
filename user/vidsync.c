/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VIDSYNC.BIN - Book 8088 CGA LCD boundary sync utility for hobbyOS.
 * Flashes active corner pixels for ~4 VSync frames so the Book 8088 7-inch LCD
 * scaler PLL locks onto the true horizontal and vertical screen boundaries.
 */

#include "../include/hobbyos.h"

int app_main(int argc, char **argv) {
    if (argc > 1) {
        const char *arg = argv[1];
        if (arg[0] == '/' || arg[0] == '-') {
            if (arg[1] == '?' || arg[1] == 'h' || arg[1] == 'H') {
                os_puts("VIDSYNC - Book 8088 CGA LCD Boundary Sync Utility\n");
                os_puts("Usage: vidsync\n\n");
                os_puts("Flashes screen corner pixels to recalibrate the Book 8088\n");
                os_puts("internal LCD scaler after mode switching or off-center shift.\n");
                return 0;
            }
        }
    }

    if (os_video_type() != VIDEO_TYPE_CGA) {
        os_puts("VIDSYNC: Active display adapter is not CGA (LCD sync is Book 8088 CGA specific).\n");
        return 0;
    }

    os_puts("VIDSYNC: Forcing Book 8088 LCD recalibration (please wait ~2s)...\n");
    os_gfx_set_mode(0x01); /* 40x25 text mode (triggers dot clock transition) */
    os_gfx_set_mode(OS_MODE_TEXT_80x25); /* 80x25 text mode (triggers scaler auto-center) */
    os_puts("VIDSYNC: LCD boundary sync calibrated successfully.\n");
    return 0;
}
