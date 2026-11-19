/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * TREE.BIN - Directory hierarchy visualizer for hobbyOS.
 */

#include "hobbyos.h"

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];
#define FP_HERE(ptr) ((void __far *)MK_FP(get_ds(), ptr))

#define MAX_DEPTH 16
#define MAX_PATH_LEN 128

static int s_flag_files = 0; /* /F: include files */
static int s_flag_ascii = 0; /* /A: use ASCII chars instead of CP437 */
static uint16_t s_total_dirs  = 0;
static uint16_t s_total_files = 0;
static int s_aborted = 0;

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

static int str_len(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

static void build_subpath(const char *parent, const char *child, char *out, int max_len) {
    int pos = 0;
    int plen = str_len(parent);
    int clen = str_len(child);
    int i;

    for (i = 0; i < plen && pos < max_len - 1; i++) {
        out[pos++] = parent[i];
    }
    if (pos > 0 && out[pos - 1] != '/' && out[pos - 1] != '\\' && out[pos - 1] != ':') {
        if (pos < max_len - 1) {
            out[pos++] = '/';
        }
    }
    for (i = 0; i < clen && pos < max_len - 1; i++) {
        out[pos++] = child[i];
    }
    out[pos] = '\0';
}

static uint16_t count_entries(const char *dir_path) {
    os_dir_entry_t dent;
    uint16_t idx = 0;
    uint16_t count = 0;

    for (;;) {
        if (os_dir_entry_at(dir_path, idx++, (os_dir_entry_t __far *)FP_HERE(&dent)) != 0) {
            break;
        }
        if (dent.name[0] == '.') {
            continue;
        }
        if (dent.attr & OS_ATTR_DIRECTORY) {
            count++;
        } else if (s_flag_files) {
            count++;
        }
    }
    return count;
}

static void traverse_dir(const char *dir_path, int depth, uint16_t prefix_mask) {
    os_dir_entry_t dent;
    uint16_t total_items;
    uint16_t item_idx = 0;
    uint16_t read_idx = 0;
    char sub_path[MAX_PATH_LEN];

    if (depth >= MAX_DEPTH || s_aborted) {
        return;
    }

    total_items = count_entries(dir_path);
    if (total_items == 0) {
        return;
    }

    for (;;) {
        int is_last;
        int d;
        uint16_t child_mask;

        if (os_kbhit()) {
            uint16_t k = os_getc();
            if ((k & 0xFF) == 0x03) {
                s_aborted = 1;
                os_puts("^C\n");
                return;
            }
        }

        if (os_dir_entry_at(dir_path, read_idx++, (os_dir_entry_t __far *)FP_HERE(&dent)) != 0) {
            break;
        }
        if (dent.name[0] == '.') {
            continue;
        }
        if (!(dent.attr & OS_ATTR_DIRECTORY) && !s_flag_files) {
            continue;
        }

        item_idx++;
        is_last = (item_idx >= total_items);

        /* 1. Print prefix columns for ancestor depths */
        for (d = 0; d < depth; d++) {
            if (prefix_mask & (1 << d)) {
                if (s_flag_ascii) {
                    os_puts("|   ");
                } else {
                    os_putc((char)0xB3); /* │ */
                    os_puts("   ");
                }
            } else {
                os_puts("    ");
            }
        }

        /* 2. Print branch marker for this item */
        if (is_last) {
            if (s_flag_ascii) {
                os_puts("\\---");
            } else {
                os_putc((char)0xC0); /* └ */
                os_putc((char)0xC4); /* ─ */
                os_putc((char)0xC4); /* ─ */
                os_putc((char)0xC4); /* ─ */
            }
            child_mask = prefix_mask;
        } else {
            if (s_flag_ascii) {
                os_puts("+---");
            } else {
                os_putc((char)0xC3); /* ├ */
                os_putc((char)0xC4); /* ─ */
                os_putc((char)0xC4); /* ─ */
                os_putc((char)0xC4); /* ─ */
            }
            child_mask = prefix_mask | (1 << depth);
        }

        /* 3. Print entry name */
        os_puts(dent.name);
        os_putc('\n');

        /* 4. Recurse into subdirectory */
        if (dent.attr & OS_ATTR_DIRECTORY) {
            s_total_dirs++;
            build_subpath(dir_path, dent.name, sub_path, sizeof(sub_path));
            traverse_dir(sub_path, depth + 1, child_mask);
            if (s_aborted) return;
        } else {
            s_total_files++;
        }
    }
}

int app_main(void) {
    const char *args = os_get_args();
    char root_path[64];
    int rlen = 0;

    s_flag_files = 0;
    s_flag_ascii = 0;
    s_total_dirs = 0;
    s_total_files = 0;
    s_aborted = 0;
    root_path[0] = '\0';

    if (args) {
        while (*args) {
            while (*args == ' ') args++;
            if (*args == '\0') break;

            if (*args == '/' || *args == '-') {
                char opt = *(args + 1);
                if (opt == 'f' || opt == 'F') {
                    s_flag_files = 1;
                    args += 2;
                } else if (opt == 'a' || opt == 'A') {
                    s_flag_ascii = 1;
                    args += 2;
                } else if (opt == '?' || opt == 'h' || opt == 'H') {
                    os_puts("Usage: tree [path] [/f] [/a]\n");
                    os_puts("  /f   Display names of the files in each directory\n");
                    os_puts("  /a   Use ASCII characters instead of extended box drawing characters\n");
                    return 0;
                } else {
                    args += 2;
                }
            } else {
                /* Root path argument */
                while (*args && *args != ' ' && rlen < 63) {
                    root_path[rlen++] = *args++;
                }
                root_path[rlen] = '\0';
            }
        }
    }

    if (root_path[0] == '\0') {
        char cwd[64];
        if (os_getcwd((char __far *)MK_FP(get_ds(), cwd), sizeof(cwd)) == 0 && cwd[0] != '\0') {
            int i = 0;
            while (cwd[i] && i < 63) {
                root_path[i] = cwd[i];
                i++;
            }
            root_path[i] = '\0';
        } else {
            root_path[0] = '.';
            root_path[1] = '\0';
        }
    }

    /* Print Header */
    os_puts("Directory PATH listing\n");
    os_puts(root_path);
    os_putc('\n');

    /* Begin recursive traversal */
    traverse_dir(root_path, 0, 0);

    /* Print Summary */
    os_putc('\n');
    print_uint(s_total_dirs);
    os_puts(" directories");
    if (s_flag_files) {
        os_puts(", ");
        print_uint(s_total_files);
        os_puts(" files");
    }
    os_putc('\n');

    return 0;
}
