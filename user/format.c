/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FORMAT.BIN - Disk & Partition Formatting Utility for hobbyOS.
 * Formats floppies and hard disk partitions with FAT12/FAT16 filesystems,
 * installs the HobbyOS bootblock, and optionally transfers system files (/S).
 */

#include "../include/hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

static uint16_t str_len(const char *s) {
    uint16_t len = 0;
    while (s && s[len] != '\0') {
        len++;
    }
    return len;
}

static void str_copy(char *dst, const char *src, uint16_t max_len) {
    uint16_t i = 0;
    while (src && src[i] != '\0' && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static int str_icmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (to_upper(*s1) != to_upper(*s2)) return 1;
        s1++;
        s2++;
    }
    return *s1 != *s2;
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
    char buf[6];
    int i = 0, j;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (j = i - 1; j >= 0; j--) {
        os_putc(buf[j]);
    }
}

static void make_path(char *dst, const char *dev, const char *rel, uint16_t max_len) {
    uint16_t i = 0, j = 0;
    while (dev[i] != '\0' && i + 1 < max_len) {
        dst[i] = dev[i];
        i++;
    }
    if (i > 0 && dst[i - 1] != ':' && i + 1 < max_len) {
        dst[i++] = ':';
    }
    while (rel[j] != '\0' && i + 1 < max_len) {
        dst[i++] = rel[j++];
    }
    dst[i] = '\0';
}

int app_main(void) {
    const char *args = os_get_args();
    char dev[16];
    char label[16];
    int sys_flag = 0;
    int yes_flag = 0;
    int quick_flag = 0;
    int has_label = 0;
    uint16_t fmt_type = FORMAT_TYPE_AUTO;
    uint16_t flags = 0;
    uint16_t i = 0, p = 0;
    uint16_t dev_len;
    char c;
    os_vol_info_t vinfo;
    uint8_t v_idx = 0;
    int found_vol = 0;

    dev[0]   = '\0';
    label[0] = '\0';

    /* Parse arguments */
    while (args && args[i] == ' ') i++;

    if (!args || args[i] == '\0') {
        os_puts("Usage: FORMAT <DEV:> [/S] [/V:label] [/F:size] [/Q] [/Y]\n");
        os_puts("Formats a floppy disk or hard disk partition with FAT12/FAT16.\n");
        os_puts("  /S        Transfer system files (bootblock, KERNEL.BIN, SHELL.BIN)\n");
        os_puts("  /V:label  Specify volume label (up to 11 characters)\n");
        os_puts("  /F:size   Floppy size in KB (1440, 720, 1200, 360)\n");
        os_puts("  /Q        Quick format (skip low-level track formatting)\n");
        os_puts("  /Y        Suppress confirmation prompt\n");
        return 1;
    }

    while (args[i] != '\0') {
        while (args[i] == ' ') i++;
        if (args[i] == '\0') break;

        if (args[i] == '/' || args[i] == '-') {
            i++;
            if (args[i] == 'S' || args[i] == 's') {
                sys_flag = 1;
                i++;
            } else if (args[i] == 'Y' || args[i] == 'y') {
                yes_flag = 1;
                i++;
            } else if (args[i] == 'Q' || args[i] == 'q') {
                quick_flag = 1;
                i++;
            } else if (args[i] == 'V' || args[i] == 'v') {
                i++;
                if (args[i] == ':' || args[i] == '=') i++;
                p = 0;
                while (args[i] != '\0' && args[i] != ' ' && p < 11) {
                    label[p++] = to_upper(args[i++]);
                }
                label[p] = '\0';
                has_label = 1;
            } else if (args[i] == 'F' || args[i] == 'f') {
                char fbuf[8];
                i++;
                if (args[i] == ':' || args[i] == '=') i++;
                p = 0;
                while (args[i] != '\0' && args[i] != ' ' && p < sizeof(fbuf) - 1) {
                    fbuf[p++] = args[i++];
                }
                fbuf[p] = '\0';
                if (fbuf[0] == '1' && fbuf[1] == '4') {
                    fmt_type = FORMAT_TYPE_1440;
                } else if (fbuf[0] == '7' && fbuf[1] == '2') {
                    fmt_type = FORMAT_TYPE_720;
                } else if (fbuf[0] == '1' && fbuf[1] == '2') {
                    fmt_type = FORMAT_TYPE_1200;
                } else if (fbuf[0] == '3' && fbuf[1] == '6') {
                    fmt_type = FORMAT_TYPE_360;
                }
            } else if (args[i] >= '0' && args[i] <= '9') {
                char fbuf[8];
                p = 0;
                while (args[i] != '\0' && args[i] != ' ' && p < sizeof(fbuf) - 1) {
                    fbuf[p++] = args[i++];
                }
                fbuf[p] = '\0';
                if (fbuf[0] == '1' && fbuf[1] == '4') {
                    fmt_type = FORMAT_TYPE_1440;
                } else if (fbuf[0] == '7' && fbuf[1] == '2') {
                    fmt_type = FORMAT_TYPE_720;
                } else if (fbuf[0] == '1' && fbuf[1] == '2') {
                    fmt_type = FORMAT_TYPE_1200;
                } else if (fbuf[0] == '3' && fbuf[1] == '6') {
                    fmt_type = FORMAT_TYPE_360;
                }
            } else {
                /* Unknown flag: skip token */
                while (args[i] != '\0' && args[i] != ' ') i++;
            }
        } else {
            /* Device name */
            p = 0;
            while (args[i] != '\0' && args[i] != ' ' && p < sizeof(dev) - 2) {
                dev[p++] = to_upper(args[i++]);
            }
            dev[p] = '\0';
        }
    }

    if (dev[0] == '\0') {
        os_puts("Error: Target volume not specified.\n");
        return 1;
    }

    dev_len = str_len(dev);
    if (dev_len > 0 && dev[dev_len - 1] != ':') {
        dev[dev_len++] = ':';
        dev[dev_len]   = '\0';
    }

    /* Safety Confirmation */
    if (!yes_flag) {
        os_puts("\nWARNING: ALL DATA ON DISK DRIVE ");
        os_puts(dev);
        os_puts(" WILL BE LOST!\nProceed with Format (Y/N)? ");

        c = (char)os_getc();
        os_putc(c);
        os_puts("\n");
        if (c != 'Y' && c != 'y') {
            os_puts("Format terminated.\n");
            return 0;
        }
    }

    os_puts("Formatting ");
    os_puts(dev);
    if (fmt_type == FORMAT_TYPE_1440) {
        os_puts(" (1.44 MB HD, 18 spt)...");
    } else if (fmt_type == FORMAT_TYPE_720) {
        os_puts(" (720 KB DD, 9 spt)...");
    } else if (fmt_type == FORMAT_TYPE_1200) {
        os_puts(" (1.2 MB HD, 15 spt)...");
    } else if (fmt_type == FORMAT_TYPE_360) {
        os_puts(" (360 KB DD, 40 cyls)...");
    } else {
        os_puts("...");
    }
    os_puts("\n");

    flags = fmt_type | (quick_flag ? FORMAT_FLAG_QUICK : 0);
    if (os_vol_format_ex(dev, has_label ? (const char __far *)FP_HERE(label) : (const char __far *)0, flags) != 0) {
        os_puts("Format failed! Device I/O error or invalid volume.\n");
        return 1;
    }
    os_puts("Format complete.\n");

    /* Transfer system files if /S requested */
    if (sys_flag) {
        char dst_path[48];
        os_puts("Transferring system files...\n");

        make_path(dst_path, dev, "KERNEL.BIN", sizeof(dst_path));
        if (os_file_copy("SYS:KERNEL.BIN", (const char __far *)FP_HERE(dst_path), 0) != 0 &&
            os_file_copy("/KERNEL.BIN", (const char __far *)FP_HERE(dst_path), 0) != 0) {
            os_puts("Warning: Unable to copy KERNEL.BIN\n");
        } else {
            os_puts("Copied KERNEL.BIN\n");
        }

        make_path(dst_path, dev, "BIN", sizeof(dst_path));
        os_mkdir(dst_path);

        make_path(dst_path, dev, "BIN/SHELL.BIN", sizeof(dst_path));
        if (os_file_copy("SYS:BIN/SHELL.BIN", (const char __far *)FP_HERE(dst_path), 0) != 0 &&
            os_file_copy("/BIN/SHELL.BIN", (const char __far *)FP_HERE(dst_path), 0) != 0) {
            os_puts("Warning: Unable to copy /BIN/SHELL.BIN\n");
        } else {
            os_puts("Copied /BIN/SHELL.BIN\n");
        }

        os_puts("System transferred.\n");
    }

    /* Query and report format statistics */
    v_idx = 0;
    while (os_vol_info(v_idx++, (os_vol_info_t __far *)FP_HERE(&vinfo)) == 0) {
        char v_cmp[16];
        uint16_t k = 0;
        str_copy(v_cmp, vinfo.name, sizeof(v_cmp));
        k = str_len(v_cmp);
        if (k > 0 && v_cmp[k - 1] != ':') {
            v_cmp[k++] = ':';
            v_cmp[k]   = '\0';
        }
        if (str_icmp(v_cmp, dev) == 0) {
            uint16_t spc = vinfo.sectors_per_cluster;
            uint32_t tot_kb, free_kb;
            uint8_t mult;
            if (spc == 0) spc = 1;
            mult = (uint8_t)(spc >> 1);
            tot_kb = vinfo.total_clusters;
            free_kb = vinfo.free_clusters;
            if (spc >= 2) {
                while (mult > 1) {
                    tot_kb <<= 1;
                    free_kb <<= 1;
                    mult >>= 1;
                }
            } else {
                tot_kb >>= 1;
                free_kb >>= 1;
            }
            os_puts("\nFormat successful.\n");
            os_puts("  Total disk space: ");
            print_uint32(tot_kb);
            os_puts(" KB\n  Available space:  ");
            print_uint32(free_kb);
            os_puts(" KB\n");
            found_vol = 1;
            break;
        }
    }
    if (!found_vol) {
        os_puts("\nFormat successful.\n");
    }

    return 0;
}
