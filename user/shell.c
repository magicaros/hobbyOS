/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SHELL.BIN - User-space interactive command shell and .sh batch script runner for hobbyOS.
 */

#include "../include/hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

#define MAX_VARS       32
#define VAR_NAME_LEN   16
#define VAR_VAL_LEN    64
#define SCRIPT_BUF_SIZE 4096

typedef struct {
    char name[VAR_NAME_LEN];
    char val[VAR_VAL_LEN];
} shell_var_t;

static shell_var_t vars[MAX_VARS];
static uint16_t    var_count = 0;
static uint16_t    last_exit_code = 0;
static int         script_echo_on = 1;

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

static void print_uint32(uint32_t val) {
    char buf[12];
    int i = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0 && i < 11) {
        buf[i++] = (char)('0' + u32_div10(&val));
    }
    while (i > 0) {
        os_putc(buf[--i]);
    }
}

static void print_uint16(uint16_t val) {
    char buf[8];
    int i = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0 && i < 7) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) {
        os_putc(buf[--i]);
    }
}

static void print_hex_byte(uint8_t val) {
    static const char hex[] = "0123456789ABCDEF";
    os_putc(hex[(val >> 4) & 0x0F]);
    os_putc(hex[val & 0x0F]);
}

static uint32_t clusters_to_kb(uint16_t clusters, uint8_t spc) {
    uint32_t kb = clusters;
    uint8_t mult;
    if (spc == 0) spc = 1;
    mult = (uint8_t)(spc >> 1);
    if (spc >= 2) {
        while (mult > 1) {
            kb <<= 1;
            mult >>= 1;
        }
    } else {
        kb >>= 1;
    }
    return kb;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static uint16_t str_len(const char *s) {
    uint16_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void str_copy(char *dst, const char *src, uint16_t max_len) {
    uint16_t i = 0;
    if (max_len == 0) return;
    while (src[i] != '\0' && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static char *split_word(char *s, char **rest) {
    char *start = skip_spaces(s);
    char *p = start;
    while (*p && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p) {
        *p = '\0';
        p++;
    }
    if (rest) {
        *rest = skip_spaces(p);
    }
    return start;
}

/* Variable management */
static const char *get_var(const char *name) {
    static char special_buf[64];
    uint16_t i;

    if (str_eq(name, "?")) {
        special_buf[0] = '\0';
        {
            char t[8];
            int ti = 0;
            uint16_t v = last_exit_code;
            if (v == 0) {
                special_buf[0] = '0';
                special_buf[1] = '\0';
            } else {
                while (v > 0 && ti < 7) {
                    t[ti++] = (char)('0' + (v % 10));
                    v /= 10;
                }
                for (i = 0; i < (uint16_t)ti; i++) {
                    special_buf[i] = t[ti - 1 - i];
                }
                special_buf[ti] = '\0';
            }
        }
        return special_buf;
    }
    if (str_eq(name, "PATH")) {
        if (os_get_path((char __far *)FP_HERE(special_buf), sizeof(special_buf)) == 0) {
            return special_buf;
        }
    }
    if (str_eq(name, "CWD")) {
        if (os_getcwd((char __far *)FP_HERE(special_buf), sizeof(special_buf)) == 0) {
            return special_buf;
        }
    }

    for (i = 0; i < var_count; i++) {
        if (str_eq(vars[i].name, name)) {
            return vars[i].val;
        }
    }
    return "";
}

static void set_var(const char *name, const char *val) {
    uint16_t i;

    if (!name || name[0] == '\0') return;

    if (str_eq(name, "PATH")) {
        os_set_path((const char __far *)FP_HERE(val));
    }

    for (i = 0; i < var_count; i++) {
        if (str_eq(vars[i].name, name)) {
            str_copy(vars[i].val, val, VAR_VAL_LEN);
            return;
        }
    }

    if (var_count < MAX_VARS) {
        str_copy(vars[var_count].name, name, VAR_NAME_LEN);
        str_copy(vars[var_count].val, val, VAR_VAL_LEN);
        var_count++;
    }
}

static void cmd_set(char *args) {
    char *p = skip_spaces(args);
    char *eq;
    uint16_t i;

    if (*p == '\0') {
        char kpath[64];
        char kcwd[64];
        if (os_get_path((char __far *)FP_HERE(kpath), sizeof(kpath)) == 0) {
            os_puts("PATH=");
            os_puts(kpath);
            os_putc('\n');
        }
        if (os_getcwd((char __far *)FP_HERE(kcwd), sizeof(kcwd)) == 0) {
            os_puts("CWD=");
            os_puts(kcwd);
            os_putc('\n');
        }
        os_puts("?=");
        print_uint16(last_exit_code);
        os_putc('\n');

        for (i = 0; i < var_count; i++) {
            if (!str_eq(vars[i].name, "PATH") && !str_eq(vars[i].name, "CWD")) {
                os_puts(vars[i].name);
                os_putc('=');
                os_puts(vars[i].val);
                os_putc('\n');
            }
        }
        return;
    }

    eq = p;
    while (*eq && *eq != '=') {
        eq++;
    }
    if (*eq == '=') {
        *eq = '\0';
        set_var(p, eq + 1);
    }
}

/* Variable expansion: $VAR, ${VAR}, $1..$9 */
static void expand_variables(const char *in, char *out, uint16_t max_out, const char *script_args) {
    uint16_t i = 0, o = 0;

    while (in[i] != '\0' && o + 1 < max_out) {
        if (in[i] == '$') {
            i++;
            if (in[i] >= '1' && in[i] <= '9') {
                uint8_t target_idx = (uint8_t)(in[i] - '0');
                const char *ap = script_args ? script_args : "";
                uint8_t cur_idx = 1;
                while (*ap && cur_idx <= target_idx) {
                    while (*ap == ' ' || *ap == '\t') ap++;
                    if (*ap == '\0') break;
                    if (cur_idx == target_idx) {
                        while (*ap && *ap != ' ' && *ap != '\t' && o + 1 < max_out) {
                            out[o++] = *ap++;
                        }
                        break;
                    }
                    while (*ap && *ap != ' ' && *ap != '\t') ap++;
                    cur_idx++;
                }
                i++;
            } else if (in[i] == '?') {
                const char *val = get_var("?");
                while (*val && o + 1 < max_out) {
                    out[o++] = *val++;
                }
                i++;
            } else {
                char vname[VAR_NAME_LEN];
                uint16_t vn = 0;
                int in_brace = 0;

                if (in[i] == '{') {
                    in_brace = 1;
                    i++;
                }
                while (in[i] != '\0' && vn + 1 < VAR_NAME_LEN) {
                    char c = in[i];
                    if (in_brace) {
                        if (c == '}') {
                            i++;
                            break;
                        }
                    } else {
                        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '_')) {
                            break;
                        }
                    }
                    vname[vn++] = c;
                    i++;
                }
                vname[vn] = '\0';
                if (vn > 0) {
                    const char *val = get_var(vname);
                    while (*val && o + 1 < max_out) {
                        out[o++] = *val++;
                    }
                }
            }
        } else {
            out[o++] = in[i++];
        }
    }
    out[o] = '\0';
}

static uint16_t read_line(char *buf, uint16_t max_len) {
    uint16_t len = 0;
    for (;;) {
        uint16_t key = os_getc();
        uint8_t  u   = (uint8_t)(key & 0xFF);
        char     c   = (char)u;

        if (c == '\r' || c == '\n') {
            os_putc('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b' || u == 0x7F) {
            if (len > 0) {
                len--;
                os_putc('\b');
            }
        } else if (u >= 0x20 && u != 0x7F) {
            if (len + 1 < max_len) {
                buf[len++] = c;
                os_putc(c);
            }
        }
    }
}

static char char_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static int wildcard_match(const char *pattern, const char *text) {
    const char *p = pattern;
    const char *t = text;
    const char *p_star = 0;
    const char *t_star = 0;

    if (!pattern || !text) return 0;

    /* Special case: "*.*" matches everything in FAT environments */
    if (pattern[0] == '*' && pattern[1] == '.' && pattern[2] == '*' && pattern[3] == '\0') {
        return 1;
    }

    while (*t) {
        if (*p == '?') {
            p++;
            t++;
        } else if (*p == '*') {
            p_star = p++;
            t_star = t;
        } else if (char_upper(*p) == char_upper(*t)) {
            p++;
            t++;
        } else if (p_star) {
            p = p_star + 1;
            t = ++t_star;
        } else {
            return 0;
        }
    }
    while (*p == '*') {
        p++;
    }
    return (*p == '\0');
}

static int has_wildcards(const char *str) {
    if (!str) return 0;
    while (*str) {
        if (*str == '*' || *str == '?') return 1;
        str++;
    }
    return 0;
}

static void split_path_pattern(const char *src, char out_dir[64], char out_pattern[16]) {
    const char *last_sep = 0;
    const char *p = src;
    uint16_t i;

    while (*p) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            last_sep = p;
        }
        p++;
    }

    if (!last_sep) {
        out_dir[0] = '\0';
        i = 0;
        while (src[i] != '\0' && i < 15) {
            out_pattern[i] = src[i];
            i++;
        }
        out_pattern[i] = '\0';
    } else {
        uint16_t dir_len = (uint16_t)(last_sep - src);
        if (*last_sep == ':') {
            dir_len++;
        } else if (dir_len == 0 && (*last_sep == '/' || *last_sep == '\\')) {
            dir_len = 1;
        } else if (last_sep > src && *(last_sep - 1) == ':') {
            dir_len++;
        }
        for (i = 0; i < dir_len && i < 63; i++) {
            out_dir[i] = src[i];
        }
        out_dir[i] = '\0';

        p = last_sep + 1;
        i = 0;
        while (p[i] != '\0' && i < 15) {
            out_pattern[i] = p[i];
            i++;
        }
        out_pattern[i] = '\0';
    }
}

static void combine_dir_file(const char *dir, const char *file, char out_path[64]) {
    uint16_t i = 0;
    uint16_t f = 0;

    if (dir && dir[0] != '\0') {
        while (dir[i] != '\0' && i < 48) {
            out_path[i] = dir[i];
            i++;
        }
        if (i > 0 && out_path[i - 1] != '/' && out_path[i - 1] != '\\') {
            out_path[i++] = '/';
        }
    }
    while (file[f] != '\0' && i < 63) {
        out_path[i++] = file[f++];
    }
    out_path[i] = '\0';
}

static void cmd_ls(const char *path) {
    os_dir_entry_t dent;
    uint16_t idx = 0;
    char dir_buf[64];
    char pattern_buf[16];
    const char *dir_arg = 0;
    const char *pattern = 0;
    os_vol_info_t vinfo;
    uint16_t match_count = 0;

    if (path && path[0] != '\0') {
        if (has_wildcards(path)) {
            split_path_pattern(path, dir_buf, pattern_buf);
            dir_arg = (dir_buf[0] != '\0') ? dir_buf : 0;
            pattern = pattern_buf;
        } else {
            dir_arg = path;
        }
    }

    for (;;) {
        int rc = os_dir_entry_at(dir_arg, idx, (os_dir_entry_t __far *)FP_HERE(&dent));
        if (rc != 0) {
            if (idx == 0) {
                if (rc == -3) {
                    os_puts("Drive not ready / No disk in drive\n");
                    return;
                } else if (rc == -2) {
                    os_puts("Directory not found: ");
                    os_puts(path ? path : "");
                    os_putc('\n');
                    return;
                }
            }
            break;
        }
        idx++;

        if (pattern && !wildcard_match(pattern, dent.name)) {
            continue;
        }
        match_count++;

        os_puts("  ");
        os_puts(dent.name);
        if (dent.attr & OS_ATTR_DIRECTORY) {
            os_puts("\t<DIR>\n");
        } else {
            os_puts("\t");
            print_uint32(dent.size);
            os_puts(" B\n");
        }
    }

    /* Print Volume Total and Free Space */
    if (os_vol_info(0xFF, (os_vol_info_t __far *)FP_HERE(&vinfo)) == 0) {
        uint32_t total_kb = clusters_to_kb(vinfo.total_clusters, (uint8_t)vinfo.sectors_per_cluster);
        uint32_t free_kb  = clusters_to_kb(vinfo.free_clusters, (uint8_t)vinfo.sectors_per_cluster);
        os_puts("Volume ");
        os_puts(vinfo.name);
        os_puts(": ");
        print_uint16(vinfo.total_clusters);
        os_puts(" clusters (");
        print_uint32(total_kb);
        os_puts(" KB), ");
        print_uint16(vinfo.free_clusters);
        os_puts(" free (");
        print_uint32(free_kb);
        os_puts(" KB free)\n");
    }
}

static void cmd_lsdisk(void) {
    os_vol_info_t vinfo;
    uint8_t i = 0;
    os_puts("Discovered Storage Volumes:\n");
    while (os_vol_info(i++, (os_vol_info_t __far *)FP_HERE(&vinfo)) == 0) {
        os_puts("  ");
        os_puts(vinfo.name);
        os_puts(":\t");
        if (vinfo.is_alias) {
            os_puts("-> ");
            os_puts(vinfo.target_name);
            os_puts(": (Alias)\n");
        } else if (vinfo.bios_drive == 0xFF) {
            os_puts("RAM Disk");
            if (vinfo.is_mounted) {
                uint32_t total_kb = clusters_to_kb(vinfo.total_clusters, (uint8_t)vinfo.sectors_per_cluster);
                uint32_t free_kb  = clusters_to_kb(vinfo.free_clusters, (uint8_t)vinfo.sectors_per_cluster);
                os_puts(vinfo.fat_type == 1 ? " (FAT12) | " : " (FAT16) | ");
                print_uint32(total_kb);
                os_puts(" KB total (");
                print_uint32(free_kb);
                os_puts(" KB free)\n");
            } else {
                os_puts(" | [Unmounted]\n");
            }
        } else {
            os_puts("Drive 0x");
            print_hex_byte(vinfo.bios_drive);
            if (vinfo.is_removable) {
                os_puts(", Floppy");
            } else if (vinfo.part_idx != 0xFF) {
                os_puts(", Part ");
                print_uint16((uint16_t)vinfo.part_idx);
            } else {
                os_puts(", Raw");
            }

            if (vinfo.is_removable && !vinfo.is_mounted) {
                os_puts(" | [No disk inserted]\n");
            } else if (!vinfo.is_mounted) {
                os_puts(" | [Unformatted]\n");
            } else {
                uint32_t total_kb = clusters_to_kb(vinfo.total_clusters, (uint8_t)vinfo.sectors_per_cluster);
                uint32_t free_kb  = clusters_to_kb(vinfo.free_clusters, (uint8_t)vinfo.sectors_per_cluster);
                os_puts(vinfo.fat_type == 1 ? " (FAT12) | " : " (FAT16) | ");
                print_uint32(total_kb);
                os_puts(" KB total (");
                print_uint32(free_kb);
                os_puts(" KB free)\n");
            }
        }
    }
}

static void cmd_diskchange(const char *arg) {
    if (arg && arg[0] != '\0') {
        if (os_diskchange(arg) == 0) {
            uint16_t len = str_len(arg);
            os_puts(arg);
            if (len == 0 || arg[len - 1] != ':') {
                os_putc(':');
            }
            os_puts(" Remount pending / disk change signaled.\n");
        } else {
            os_puts("Device not found or not removable: ");
            os_puts(arg);
            os_putc('\n');
        }
    } else {
        os_diskchange(0);
        os_puts("Removable media reset / remount pending.\n");
    }
}

static void cmd_cat(const char *path) {
    static char file_buf[513];
    int n;

    if (!path || path[0] == '\0') {
        os_puts("Usage: cat <path>\n");
        return;
    }
    n = os_file_read(path, FP_HERE(file_buf), 512);
    if (n < 0) {
        if (n == -3) {
            os_puts("Drive not ready / No disk in drive: ");
        } else {
            os_puts("File not found: ");
        }
        os_puts(path);
        os_putc('\n');
        return;
    }
    file_buf[n] = '\0';
    os_puts(file_buf);
    if (n > 0 && file_buf[n - 1] != '\n') {
        os_putc('\n');
    }
}

static char *extract_quoted(char *s, uint16_t *out_len) {
    char *start;
    char *end;

    s = skip_spaces(s);
    if (*s == '"') {
        start = s + 1;
        end = start;
        while (*end && *end != '"') {
            end++;
        }
        *end = '\0';
        *out_len = (uint16_t)(end - start);
        return start;
    }
    *out_len = str_len(s);
    return s;
}

static void cmd_write(char *arg_line) {
    static char out_buf[82];
    char *p = skip_spaces(arg_line);
    char *rest = 0;
    char *fname = 0;
    char *text = 0;
    uint16_t len = 0;
    uint16_t i;
    int append_mode = 0;
    int res;

    if (*p == '\0') {
        os_puts("Usage: write \"<text>\" | write [-a] <file> \"<text>\"\n");
        return;
    }

    if (*p == '"') {
        text = extract_quoted(p, &len);
        os_puts(text);
        os_putc('\n');
        return;
    }

    {
        char *tok1 = split_word(p, &rest);
        if (str_eq(tok1, "-a")) {
            append_mode = 1;
            fname = split_word(rest, &rest);
        } else {
            fname = tok1;
            rest = skip_spaces(rest);
            if (rest[0] == '-' && rest[1] == 'a' && (rest[2] == ' ' || rest[2] == '\0')) {
                append_mode = 1;
                split_word(rest, &rest);
            }
        }
    }

    rest = skip_spaces(rest);
    if (fname[0] == '\0' || rest[0] == '\0') {
        os_puts("Usage: write \"<text>\" | write [-a] <file> \"<text>\"\n");
        return;
    }

    text = extract_quoted(rest, &len);
    if (len > 80) {
        len = 80;
    }
    for (i = 0; i < len; i++) {
        out_buf[i] = text[i];
    }
    out_buf[len] = '\n';
    out_buf[len + 1] = '\0';

    if (append_mode) {
        res = os_file_append(fname, FP_HERE(out_buf), len + 1);
    } else {
        res = os_file_write(fname, FP_HERE(out_buf), len + 1);
    }

    if (res < 0) {
        os_puts("Failed to write file: ");
        os_puts(fname);
        os_putc('\n');
    }
}

static void cmd_rm(const char *path) {
    if (!path || path[0] == '\0') {
        os_puts("Usage: rm <path>\n");
        return;
    }

    if (!has_wildcards(path)) {
        if (os_file_delete(path) == 0) {
            os_puts("Deleted ");
            os_puts(path);
            os_putc('\n');
        } else {
            os_puts("Cannot delete file: ");
            os_puts(path);
            os_putc('\n');
        }
        return;
    }

    {
        char dir_buf[64];
        char pattern_buf[16];
        const char *dir_arg;
        os_dir_entry_t dent;
        uint16_t idx = 0;
        uint16_t del_count = 0;

        split_path_pattern(path, dir_buf, pattern_buf);
        dir_arg = (dir_buf[0] != '\0') ? dir_buf : 0;

        while (os_dir_entry_at(dir_arg, idx, (os_dir_entry_t __far *)FP_HERE(&dent)) == 0) {
            char full_path[64];

            if (dent.attr & OS_ATTR_DIRECTORY) {
                idx++;
                continue;
            }

            if (wildcard_match(pattern_buf, dent.name)) {
                combine_dir_file(dir_buf, dent.name, full_path);
                if (os_file_delete(full_path) == 0) {
                    os_puts("Deleted ");
                    os_puts(full_path);
                    os_putc('\n');
                    del_count++;
                } else {
                    os_puts("Cannot delete: ");
                    os_puts(full_path);
                    os_putc('\n');
                    idx++;
                }
            } else {
                idx++;
            }
        }

        if (del_count == 0) {
            os_puts("No matching files found: ");
            os_puts(path);
            os_putc('\n');
        } else {
            print_uint16(del_count);
            os_puts(" file(s) deleted.\n");
        }
    }
}

static int is_existing_dir(const char *path) {
    os_dir_entry_t dent;
    if (!path || path[0] == '\0') {
        return 0;
    }
    if ((path[0] == '/' || path[0] == '\\') && path[1] == '\0') {
        return 1;
    }
    return os_dir_entry_at(path, 0, (os_dir_entry_t __far *)FP_HERE(&dent)) != -2;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    while (*path) {
        if (*path == '/' || *path == '\\' || *path == ':') {
            if (path[1] != '\0') {
                base = path + 1;
            }
        }
        path++;
    }
    return base;
}

static void resolve_dest_path(const char *first_src, const char *dst_arg, char out_dst[64]) {
    uint16_t i = 0;
    uint16_t b = 0;
    const char *base;

    if (!is_existing_dir(dst_arg)) {
        while (dst_arg[i] != '\0' && i < 63) {
            out_dst[i] = dst_arg[i];
            i++;
        }
        out_dst[i] = '\0';
        return;
    }

    while (dst_arg[i] != '\0' && i < 48) {
        out_dst[i] = dst_arg[i];
        i++;
    }
    if (i > 0 && out_dst[i - 1] != '/' && out_dst[i - 1] != '\\') {
        out_dst[i++] = '/';
    }
    base = path_basename(first_src);
    while (base[b] != '\0' && i < 63) {
        out_dst[i++] = base[b++];
    }
    out_dst[i] = '\0';
}

static int confirm_overwrite(const char *dst_path) {
    char ans[16];
    if (os_file_size(dst_path) < 0) {
        return 1;
    }
    os_puts("Overwrite ");
    os_puts(dst_path);
    os_puts("? (y/n): ");
    read_line(ans, sizeof(ans));
    if (ans[0] == 'y' || ans[0] == 'Y') {
        return 1;
    }
    os_puts("Canceled.\n");
    return 0;
}

static void cmd_cp(char *arg_line) {
    char *srcs[8];
    uint16_t num_srcs = 0;
    char *dst_arg = 0;
    char dst_resolved[64];
    char *p = skip_spaces(arg_line);
    char *last_space = 0;
    char *scan;
    uint16_t i;

    if (*p == '\0') {
        os_puts("Usage: cp <src>[+<src2>...] <dst>\n");
        return;
    }

    scan = p;
    while (*scan) {
        if (*scan == ' ') {
            char *next_tok = skip_spaces(scan);
            if (*next_tok != '\0' && *next_tok != '+') {
                char *prev = scan - 1;
                while (prev > p && *prev == ' ') {
                    prev--;
                }
                if (*prev != '+') {
                    last_space = scan;
                }
            }
            scan = next_tok;
        } else {
            scan++;
        }
    }

    if (!last_space) {
        os_puts("Usage: cp <src>[+<src2>...] <dst>\n");
        return;
    }

    *last_space = '\0';
    dst_arg = skip_spaces(last_space + 1);
    if (dst_arg[0] == '\0') {
        os_puts("Usage: cp <src>[+<src2>...] <dst>\n");
        return;
    }

    scan = p;
    while (*scan && num_srcs < 8) {
        char *start = skip_spaces(scan);
        char *end;
        while (*scan && *scan != '+') {
            scan++;
        }
        end = scan;
        if (*scan == '+') {
            *scan++ = '\0';
        }
        while (end > start && end[-1] == ' ') {
            *--end = '\0';
        }
        if (*start != '\0') {
            srcs[num_srcs++] = start;
        }
    }

    if (num_srcs == 0) {
        os_puts("Usage: cp <src>[+<src2>...] <dst>\n");
        return;
    }

    /* Wildcard source copy */
    if (num_srcs == 1 && has_wildcards(srcs[0])) {
        char dir_buf[64];
        char pattern_buf[16];
        const char *dir_arg;
        os_dir_entry_t dent;
        uint16_t idx = 0;
        uint16_t copied_count = 0;

        if (!is_existing_dir(dst_arg)) {
            os_puts("Destination must be an existing directory: ");
            os_puts(dst_arg);
            os_putc('\n');
            return;
        }

        split_path_pattern(srcs[0], dir_buf, pattern_buf);
        dir_arg = (dir_buf[0] != '\0') ? dir_buf : 0;

        while (os_dir_entry_at(dir_arg, idx++, (os_dir_entry_t __far *)FP_HERE(&dent)) == 0) {
            char full_src[64];
            char full_dst[64];

            if (dent.attr & OS_ATTR_DIRECTORY) {
                continue;
            }
            if (wildcard_match(pattern_buf, dent.name)) {
                combine_dir_file(dir_buf, dent.name, full_src);
                resolve_dest_path(dent.name, dst_arg, full_dst);

                if (!confirm_overwrite(full_dst)) {
                    continue;
                }
                if (os_file_copy(full_src, (const char __far *)FP_HERE(full_dst), 0) == 0) {
                    os_puts("  ");
                    os_puts(dent.name);
                    os_puts(" -> ");
                    os_puts(full_dst);
                    os_putc('\n');
                    copied_count++;
                } else {
                    os_puts("Failed to copy: ");
                    os_puts(full_src);
                    os_putc('\n');
                }
            }
        }

        if (copied_count == 0) {
            os_puts("No matching files found: ");
            os_puts(srcs[0]);
            os_putc('\n');
        } else {
            print_uint16(copied_count);
            os_puts(" file(s) copied.\n");
        }
        return;
    }

    for (i = 0; i < num_srcs; i++) {
        if (os_file_size(srcs[i]) < 0) {
            os_puts("File not found: ");
            os_puts(srcs[i]);
            os_putc('\n');
            return;
        }
    }

    resolve_dest_path(srcs[0], dst_arg, dst_resolved);
    if (!confirm_overwrite(dst_resolved)) {
        return;
    }

    for (i = 0; i < num_srcs; i++) {
        uint8_t append_flag = (uint8_t)(i > 0);
        if (os_file_copy(srcs[i], (const char __far *)FP_HERE(dst_resolved), append_flag) != 0) {
            os_puts("Failed to copy: ");
            os_puts(srcs[i]);
            os_putc('\n');
            return;
        }
    }

    os_puts("Copied to ");
    os_puts(dst_resolved);
    os_putc('\n');
}

static void cmd_mv(char *arg_line) {
    char *rest = 0;
    char *src = split_word(arg_line, &rest);
    char *dst = split_word(rest, &rest);
    char dst_resolved[64];

    if (src[0] == '\0' || dst[0] == '\0') {
        os_puts("Usage: mv <src> <dst>\n");
        return;
    }
    if (os_file_size(src) < 0) {
        os_puts("File not found: ");
        os_puts(src);
        os_putc('\n');
        return;
    }

    resolve_dest_path(src, dst, dst_resolved);
    if (!confirm_overwrite(dst_resolved)) {
        return;
    }

    if (os_file_rename(src, (const char __far *)FP_HERE(dst_resolved)) == 0) {
        os_puts("Moved ");
        os_puts(src);
        os_puts(" -> ");
        os_puts(dst_resolved);
        os_putc('\n');
    } else {
        os_puts("Failed to move file: ");
        os_puts(src);
        os_putc('\n');
    }
}

static void cmd_cd(const char *path) {
    int rc;
    if (!path || path[0] == '\0') {
        os_puts("Usage: cd <dir>\n");
        return;
    }
    rc = os_chdir(path);
    if (rc != 0) {
        if (rc == -3) {
            os_puts("Drive not ready / No disk in drive: ");
        } else {
            os_puts("Directory not found: ");
        }
        os_puts(path);
        os_putc('\n');
    }
}

static void cmd_pwd(void) {
    char cwd[64];
    if (os_getcwd((char __far *)FP_HERE(cwd), sizeof(cwd)) == 0) {
        os_puts(cwd);
        os_putc('\n');
    }
}

static void cmd_mkdir(const char *path) {
    if (!path || path[0] == '\0') {
        os_puts("Usage: mkdir <dir>\n");
        return;
    }
    if (os_mkdir(path) == 0) {
        os_puts("Created directory ");
        os_puts(path);
        os_putc('\n');
    } else {
        os_puts("Failed to create directory: ");
        os_puts(path);
        os_putc('\n');
    }
}

static void cmd_rmdir(const char *path) {
    if (!path || path[0] == '\0') {
        os_puts("Usage: rmdir <dir>\n");
        return;
    }
    if (os_rmdir(path) == 0) {
        os_puts("Removed directory ");
        os_puts(path);
        os_putc('\n');
    } else {
        os_puts("Failed to remove directory (not found or not empty): ");
        os_puts(path);
        os_putc('\n');
    }
}

/* Forward declaration for script runner */
static int execute_line(char *raw_line, const char *script_args, uint16_t *out_goto_label_pos);
static int run_script(const char *script_path, const char *script_args);

static void cmd_run(char *arg_line) {
    char *args = 0;
    char *fname = split_word(arg_line, &args);
    int rc;

    if (fname[0] == '\0') {
        os_puts("Usage: run <program> [args]\n");
        return;
    }
    rc = os_exec(fname, args);
    if (rc < 0) {
        os_puts("Cannot execute: ");
        os_puts(fname);
        os_putc('\n');
        last_exit_code = 1;
    } else {
        last_exit_code = (uint16_t)rc;
    }
}

/* Check if file is a script or binary and locate in PATH if needed */
static int find_script_or_bin(const char *cmd, char resolved[64], int *is_script) {
    long sz;
    char kpath[128];
    uint16_t p = 0;
    uint16_t cmd_len = str_len(cmd);
    int has_slash = 0;
    int has_dot = 0;
    uint16_t i;

    for (i = 0; i < cmd_len; i++) {
        if (cmd[i] == '/' || cmd[i] == '\\') has_slash = 1;
        if (cmd[i] == '.') has_dot = 1;
    }

    /* 1. Direct path */
    sz = os_file_size(cmd);
    if (sz >= 0) {
        str_copy(resolved, cmd, 64);
        *is_script = (cmd_len >= 3 && (cmd[cmd_len - 3] == '.' || cmd[cmd_len - 3] == '.') &&
                      (cmd[cmd_len - 2] == 's' || cmd[cmd_len - 2] == 'S') &&
                      (cmd[cmd_len - 1] == 'h' || cmd[cmd_len - 1] == 'H'));
        return 0;
    }

    if (!has_dot) {
        /* Try cmd.sh */
        str_copy(resolved, cmd, 64);
        if (cmd_len + 3 < 63) {
            resolved[cmd_len] = '.';
            resolved[cmd_len + 1] = 's';
            resolved[cmd_len + 2] = 'h';
            resolved[cmd_len + 3] = '\0';
            if (os_file_size(resolved) >= 0) {
                *is_script = 1;
                return 0;
            }
        }
    }

    /* If has slash, don't search PATH */
    if (has_slash) return -1;

    /* 2. Search along PATH for script */
    if (os_get_path((char __far *)FP_HERE(kpath), sizeof(kpath)) != 0) {
        return -1;
    }

    p = 0;
    while (kpath[p] != '\0') {
        uint16_t d_start = p;
        uint16_t d_len;
        uint16_t pos = 0;
        while (kpath[p] != '\0' && kpath[p] != ';') p++;
        d_len = p - d_start;
        if (kpath[p] == ';') p++;

        if (d_len > 0 && !(d_len == 1 && kpath[d_start] == '.')) {
            for (i = 0; i < d_len && pos < 40; i++) {
                resolved[pos++] = kpath[d_start + i];
            }
            if (pos > 0 && resolved[pos - 1] != '/' && resolved[pos - 1] != '\\') {
                resolved[pos++] = '/';
            }
        }
        for (i = 0; i < cmd_len && pos < 50; i++) {
            resolved[pos++] = cmd[i];
        }
        resolved[pos] = '\0';

        if (os_file_size(resolved) >= 0) {
            *is_script = (cmd_len >= 3 &&
                          (cmd[cmd_len - 2] == 's' || cmd[cmd_len - 2] == 'S') &&
                          (cmd[cmd_len - 1] == 'h' || cmd[cmd_len - 1] == 'H'));
            return 0;
        }

        if (!has_dot && pos + 3 < 63) {
            resolved[pos] = '.';
            resolved[pos + 1] = 's';
            resolved[pos + 2] = 'h';
            resolved[pos + 3] = '\0';
            if (os_file_size(resolved) >= 0) {
                *is_script = 1;
                return 0;
            }
        }
    }

    return -1;
}

/* Execute a single line inside the shell / script environment */
static int execute_line(char *raw_line, const char *script_args, uint16_t *out_goto_label_pos) {
    char expanded[128];
    char *p;
    char *rest = 0;
    char *cmd;
    int is_silent = 0;

    p = skip_spaces(raw_line);
    if (*p == '\0') {
        return 0;
    }

    /* Check @ prefix */
    if (*p == '@') {
        is_silent = 1;
        p = skip_spaces(p + 1);
    }

    /* Comments */
    if (*p == '#') {
        return 0;
    }

    /* Labels */
    if (*p == ':') {
        return 0;
    }

    /* Expand variables */
    expand_variables(p, expanded, sizeof(expanded), script_args);

    p = skip_spaces(expanded);
    if (*p == '\0') {
        return 0;
    }

    /* Echo line in scripts if echoing is enabled and not suppressed by @ */
    if (script_args && script_echo_on && !is_silent) {
        os_puts(p);
        os_putc('\n');
    }

    cmd = split_word(p, &rest);
    if (cmd[0] == '\0') {
        return 0;
    }

    /* echo command */
    if (str_eq(cmd, "echo")) {
        char *sub = skip_spaces(rest);
        if (str_eq(sub, "off") || str_eq(sub, "OFF")) {
            script_echo_on = 0;
        } else if (str_eq(sub, "on") || str_eq(sub, "ON")) {
            script_echo_on = 1;
        } else {
            os_puts(sub);
            os_putc('\n');
        }
        return 0;
    }

    /* goto command */
    if (str_eq(cmd, "goto")) {
        char *lbl = split_word(rest, &rest);
        if (out_goto_label_pos) {
            *out_goto_label_pos = 1; /* signal goto request */
            /* We will match the label in the caller loop */
        }
        return 0;
    }

    /* if command: if [not] <a> == <b> <cmd...> | if [not] exist <file> <cmd...> */
    if (str_eq(cmd, "if")) {
        int invert = 0;
        char *tok1 = split_word(rest, &rest);
        if (str_eq(tok1, "not") || str_eq(tok1, "NOT")) {
            invert = 1;
            tok1 = split_word(rest, &rest);
        }

        if (str_eq(tok1, "exist") || str_eq(tok1, "EXIST")) {
            char *target_file = split_word(rest, &rest);
            int exists = (os_file_size(target_file) >= 0);
            if (invert) exists = !exists;
            if (exists) {
                return execute_line(rest, script_args, out_goto_label_pos);
            }
            return 0;
        } else {
            /* <a> == <b> */
            char *op = split_word(rest, &rest);
            char *tok2 = split_word(rest, &rest);
            if (str_eq(op, "==")) {
                int match = str_eq(tok1, tok2);
                if (invert) match = !match;
                if (match) {
                    return execute_line(rest, script_args, out_goto_label_pos);
                }
            }
            return 0;
        }
    }

    /* exit command (exit script or subshell) */
    if (str_eq(cmd, "exit")) {
        return -999; /* special exit code */
    }

    /* Built-ins */
    if (str_eq(cmd, "help")) {
        os_puts("Commands: help, ls [dir|pat], lsdisk, cd <dir>, pwd, mkdir <dir>, rmdir <dir>,\n");
        os_puts("          cat <file>, write [-a] [file] \"<text>\", cp <src> <dst>, mv <s> <d>,\n");
        os_puts("          rm <path>, edit <file>, forth, set [VAR=VAL], echo [text|on|off],\n");
        os_puts("          keyb [us|fr|be], mem, cls, format [dev:] [/s], fdisk, chkdsk, ramdisk, har, sam, gfxdemo, diskchange, <DEV:>\n");
    } else if (str_eq(cmd, "set")) {
        cmd_set(rest);
    } else if (str_eq(cmd, "ls") || str_eq(cmd, "dir")) {
        cmd_ls(rest);
    } else if (str_eq(cmd, "lsdisk")) {
        cmd_lsdisk();
    } else if (str_eq(cmd, "diskchange")) {
        cmd_diskchange(rest);
    } else if (str_eq(cmd, "cd")) {
        cmd_cd(rest);
    } else if (str_eq(cmd, "pwd")) {
        cmd_pwd();
    } else if (str_eq(cmd, "mkdir")) {
        cmd_mkdir(rest);
    } else if (str_eq(cmd, "rmdir")) {
        cmd_rmdir(rest);
    } else if (str_eq(cmd, "cat")) {
        cmd_cat(rest);
    } else if (str_eq(cmd, "write")) {
        cmd_write(rest);
    } else if (str_eq(cmd, "cp") || str_eq(cmd, "copy")) {
        cmd_cp(rest);
    } else if (str_eq(cmd, "mv") || str_eq(cmd, "move") || str_eq(cmd, "ren")) {
        cmd_mv(rest);
    } else if (str_eq(cmd, "rm")) {
        cmd_rm(rest);
    } else if (str_eq(cmd, "run")) {
        cmd_run(rest);
    } else if (str_eq(cmd, "cls")) {
        os_cls(ATTR_DEFAULT);
    } else if (cmd[0] != '\0' && cmd[str_len(cmd) - 1] == ':') {
        cmd_cd(cmd);
    } else {
        /* Check for VAR=VAL shorthand */
        char *eq = cmd;
        int has_eq = 0;
        while (*eq) {
            if (*eq == '=') {
                has_eq = 1;
                *eq = '\0';
                set_var(cmd, eq + 1);
                break;
            }
            eq++;
        }
        if (!has_eq) {
            char resolved[64];
            int is_script = 0;
            if (find_script_or_bin(cmd, resolved, &is_script) == 0 && is_script) {
                last_exit_code = (uint16_t)run_script(resolved, rest);
            } else {
                int rc = os_exec(cmd, rest);
                if (rc < 0) {
                    os_puts("Unknown command: ");
                    os_puts(cmd);
                    os_putc('\n');
                    last_exit_code = 1;
                } else {
                    last_exit_code = (uint16_t)rc;
                }
            }
        }
    }
    return 0;
}

/* Script file runner */
static int run_script(const char *script_path, const char *script_args) {
    static char script_buf[SCRIPT_BUF_SIZE];
    int n = os_file_read(script_path, FP_HERE(script_buf), SCRIPT_BUF_SIZE - 1);
    uint16_t cur_pos = 0;
    int saved_echo = script_echo_on;

    if (n <= 0) {
        return -1;
    }
    script_buf[n] = '\0';
    script_echo_on = 1;

    while (cur_pos < (uint16_t)n) {
        char line[128];
        uint16_t lpos = 0;
        uint16_t line_start = cur_pos;
        uint16_t goto_flag = 0;
        int rc;

        while (cur_pos < (uint16_t)n && script_buf[cur_pos] != '\r' && script_buf[cur_pos] != '\n') {
            if (lpos + 1 < sizeof(line)) {
                line[lpos++] = script_buf[cur_pos];
            }
            cur_pos++;
        }
        line[lpos] = '\0';
        while (cur_pos < (uint16_t)n && (script_buf[cur_pos] == '\r' || script_buf[cur_pos] == '\n')) {
            cur_pos++;
        }

        rc = execute_line(line, script_args, &goto_flag);
        if (rc == -999) {
            break;
        }

        if (goto_flag) {
            /* Find target label in script */
            char *p = skip_spaces(line);
            char *rest = 0;
            char *cmd = split_word(p, &rest);
            char *target_label = 0;
            uint16_t scan = 0;
            int found = 0;

            if (str_eq(cmd, "goto")) {
                target_label = split_word(rest, &rest);
            } else {
                /* If nested in if, search for "goto" */
                char *w = split_word(line, &rest);
                while (w && *w) {
                    if (str_eq(w, "goto")) {
                        target_label = split_word(rest, &rest);
                        break;
                    }
                    w = split_word(rest, &rest);
                }
            }

            if (target_label && *target_label) {
                while (scan < (uint16_t)n) {
                    char sline[64];
                    uint16_t spos = 0;
                    char *lp;
                    while (scan < (uint16_t)n && script_buf[scan] != '\r' && script_buf[scan] != '\n') {
                        if (spos + 1 < sizeof(sline)) sline[spos++] = script_buf[scan];
                        scan++;
                    }
                    sline[spos] = '\0';
                    while (scan < (uint16_t)n && (script_buf[scan] == '\r' || script_buf[scan] == '\n')) {
                        scan++;
                    }

                    lp = skip_spaces(sline);
                    if (*lp == ':') {
                        lp++;
                        if (str_eq(lp, target_label)) {
                            cur_pos = scan;
                            found = 1;
                            break;
                        }
                    }
                }
                if (!found) {
                    os_puts("Label not found: ");
                    os_puts(target_label);
                    os_putc('\n');
                }
            }
        }
    }

    script_echo_on = saved_echo;
    return 0;
}

int app_main(void) {
    char line[128];
    char cwd[64];

    /* Set default variables */
    set_var("PROMPT", "hobbyOS:$CWD> ");

    /* Run /BOOT.SH if it exists on disk */
    if (os_file_size("/BOOT.SH") >= 0) {
        run_script("/BOOT.SH", "");
    }

    os_puts("\nhobbyOS User-Space Shell (/BIN/SHELL.BIN). Type 'help' for commands.\n");

    for (;;) {
        if (os_getcwd((char __far *)FP_HERE(cwd), sizeof(cwd)) != 0) {
            cwd[0] = 'S'; cwd[1] = 'Y'; cwd[2] = 'S'; cwd[3] = ':';
            cwd[4] = '/'; cwd[5] = '\0';
        }

        os_puts(cwd);
        os_puts("> ");

        read_line(line, sizeof(line));
        execute_line(line, 0, 0);
    }
}
