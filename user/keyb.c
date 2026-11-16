/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KEYB.BIN - User-space keyboard layout loader and status utility for hobbyOS.
 */

#include "../include/hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') {
        s++;
    }
    return s;
}

static void list_available_maps(void) {
    os_dir_entry_t dent;
    uint16_t idx = 0;
    uint16_t found = 0;

    os_puts("Available layouts in /KEYMAPS:");
    while (os_dir_entry_at("/KEYMAPS", idx, (os_dir_entry_t __far *)FP_HERE(&dent)) == 0) {
        if (!(dent.attr & OS_ATTR_DIRECTORY) && dent.name[0] != '.') {
            os_puts(" ");
            os_puts(dent.name);
            found++;
        }
        idx++;
    }
    if (found == 0) {
        os_puts(" (none)");
    }
    os_putc('\n');
}

static void build_map_path(const char *arg, char out_path[32]) {
    int has_slash = 0;
    int has_dot   = 0;
    uint16_t len  = 0;
    uint16_t i, pos = 0;

    while (arg[len] != '\0' && arg[len] != ' ' && len < 20) {
        if (arg[len] == '/' || arg[len] == '\\') {
            has_slash = 1;
            has_dot   = 0;
        } else if (arg[len] == '.') {
            has_dot = 1;
        }
        len++;
    }

    if (!has_slash) {
        const char prefix[] = "/KEYMAPS/";
        for (i = 0; prefix[i] != '\0'; i++) {
            out_path[pos++] = prefix[i];
        }
    }

    for (i = 0; i < len && pos < 26; i++) {
        out_path[pos++] = to_upper(arg[i]);
    }

    if (!has_dot && pos <= 27) {
        out_path[pos++] = '.';
        out_path[pos++] = 'M';
        out_path[pos++] = 'A';
        out_path[pos++] = 'P';
    }
    out_path[pos] = '\0';
}

int app_main(void) {
    static os_keymap_t map_buf;
    char active_name[8];
    char path[32];
    const char *args = os_get_args();
    int bytes_read;

    args = args ? skip_spaces(args) : "";

    if (args[0] == '\0') {
        if (os_keyb_get_name((char __far *)FP_HERE(active_name)) == 0) {
            os_puts("Active keyboard layout: ");
            os_puts(active_name);
            os_putc('\n');
        }
        list_available_maps();
        os_puts("Usage: keyb <us|fr|be|/path/to/file.map>\n");
        return 0;
    }

    build_map_path(args, path);
    bytes_read = os_file_read(path, FP_HERE(&map_buf), sizeof(os_keymap_t));
    if (bytes_read != (int)sizeof(os_keymap_t)) {
        os_puts("Failed to load keymap: ");
        os_puts(path);
        os_putc('\n');
        list_available_maps();
        return 1;
    }

    if (os_keyb_set_map((const os_keymap_t __far *)FP_HERE(&map_buf)) != 0) {
        os_puts("Invalid keymap format: ");
        os_puts(path);
        os_putc('\n');
        return 1;
    }

    os_puts("Keyboard layout set to ");
    os_puts(map_buf.name);
    os_puts(" (");
    os_puts(path);
    os_puts(")\n");
    return 0;
}
