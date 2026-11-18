/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RAMDISK.BIN — User-space RAM Disk management utility for hobbyOS.
 * Supports mounting in EMS (Lo-tech 2MB) or XMS (286+), unmounting, and status.
 */

#include "hobbyos.h"

#define MK_FP(seg, off)   ((void __far *)(((uint32_t)(seg) << 16) | (uint16_t)(off)))
#define FP_HERE(ptr)      MK_FP(get_ds(), (uint16_t)(ptr))

static void print_u16(uint16_t val) {
    char buf[6];
    int idx = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0) {
        buf[idx++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (idx > 0) {
        os_putc(buf[--idx]);
    }
}

static void print_u32(uint32_t val) {
    char buf[11];
    int idx = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0) {
        /* Avoid 32-bit division library helpers: extract digits using 16-bit quotient/remainder */
        uint16_t rem;
        uint32_t q = 0;
        uint32_t r = 0;
        int bit;
        for (bit = 31; bit >= 0; bit--) {
            r = (r << 1) | ((val >> bit) & 1);
            if (r >= 10) {
                r -= 10;
                q |= ((uint32_t)1 << bit);
            }
        }
        rem = (uint16_t)r;
        buf[idx++] = (char)('0' + rem);
        val = q;
    }
    while (idx > 0) {
        os_putc(buf[--idx]);
    }
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static int str_equal_no_case(const char *a, const char *b) {
    while (*a && *b) {
        if (to_upper(*a) != to_upper(*b)) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void print_usage(void) {
    os_puts("hobbyOS RAM Disk Utility (/BIN/RAMDISK.BIN)\n");
    os_puts("Usage:\n");
    os_puts("  ramdisk /e <size_kb|max>    Mount RAM: in EMS memory (e.g. ramdisk /e 512)\n");
    os_puts("  ramdisk /x <size_kb|max>    Mount RAM: in XMS memory (286+ only)\n");
    os_puts("  ramdisk /u                  Unmount RAM: and release memory\n");
    os_puts("  ramdisk [/s]                Display current RAM: status and memory info\n");
}

static void show_status(void) {
    os_ramdisk_status_t st;
    int mounted = os_ramdisk_status((os_ramdisk_status_t __far *)FP_HERE(&st));

    if (mounted && st.active) {
        uint16_t free_kb;
        if (st.bytes_per_cluster == 512) {
            free_kb = st.free_clusters >> 1;
        } else if (st.bytes_per_cluster == 1024) {
            free_kb = st.free_clusters;
        } else if (st.bytes_per_cluster == 2048) {
            free_kb = st.free_clusters << 1;
        } else {
            free_kb = st.free_clusters << 2;
        }

        os_puts("hobbyOS RAM Disk Status:\n");
        os_puts("  Volume Name:        RAM:\n");
        os_puts("  Storage Medium:     ");
        if (st.type == RAMDISK_TYPE_EMS) {
            os_puts("EMS (Lo-tech 2MB Expanded Memory)\n");
        } else if (st.type == RAMDISK_TYPE_XMS) {
            os_puts("XMS (Extended Memory Block)\n");
        } else {
            os_puts("Unknown\n");
        }
        os_puts("  Total Capacity:     ");
        print_u16(st.size_kb);
        os_puts(" KB\n");
        os_puts("  Filesystem:         FAT12 (Cluster size: ");
        print_u16(st.bytes_per_cluster);
        os_puts(" bytes)\n");
        os_puts("  Clusters:           ");
        print_u16(st.total_clusters);
        os_puts(" total, ");
        print_u16(st.free_clusters);
        os_puts(" free (");
        print_u16(free_kb);
        os_puts(" KB free)\n");
    } else {
        uint16_t ems_free = os_ems_avail();
        uint16_t ems_tot = os_ems_total();
        uint16_t cpu = os_cpu_type();

        os_puts("RAM Disk is not currently mounted.\n\n");
        os_puts("Available Memory for RAM Disk:\n");
        os_puts("  EMS Memory: ");
        if (ems_tot > 0) {
            print_u16(ems_free * 16);
            os_puts(" KB free (");
            print_u16(ems_tot * 16);
            os_puts(" KB total, ");
            print_u16(ems_free);
            os_puts(" pages available)\n");
        } else {
            os_puts("Not detected\n");
        }

        os_puts("  XMS Memory: ");
        if (cpu >= CPU_TYPE_80286) {
            uint16_t xms_free = os_xms_avail();
            uint16_t xms_tot = os_xms_total();
            if (xms_tot > 0) {
                print_u16(xms_free);
                os_puts(" KB free (");
                print_u16(xms_tot);
                os_puts(" KB total above 1MB)\n");
            } else {
                os_puts("Not detected or 0 KB available\n");
            }
        } else {
            os_puts("Not supported (requires 80286+ CPU)\n");
        }
        os_puts("\nType 'ramdisk /?' for usage instructions.\n");
    }
}

static uint16_t parse_size(const char *str) {
    uint16_t val = 0;
    while (*str == ' ' || *str == '\t' || *str == ':') str++;
    if (*str == '\0') return 0;
    if (str_equal_no_case(str, "MAX") || str_equal_no_case(str, "ALL")) {
        return 0; /* 0 means MAX */
    }
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (uint16_t)(*str - '0');
        str++;
    }
    if (*str == 'M' || *str == 'm') {
        val <<= 10;
    }
    return val;
}

int app_main(void) {
    const char *args = os_get_args();
    char opt = '\0';
    uint16_t req_size = 0;
    uint16_t mounted_size = 0;
    int res;

    if (!args) {
        show_status();
        return 0;
    }

    while (*args == ' ' || *args == '\t') args++;
    if (*args == '\0') {
        show_status();
        return 0;
    }

    if (*args == '/' || *args == '-') {
        args++;
        opt = to_upper(*args);
        args++;
    } else {
        print_usage();
        return 1;
    }

    if (opt == '?' || opt == 'H') {
        print_usage();
        return 0;
    }

    if (opt == 'S') {
        show_status();
        return 0;
    }

    if (opt == 'U') {
        res = os_ramdisk_unmount();
        if (res == 0) {
            os_puts("RAM: unmounted and memory released successfully.\n");
            return 0;
        } else {
            os_puts("Error: RAM disk is not currently mounted.\n");
            return 1;
        }
    }

    if (opt == 'E' || opt == 'X') {
        uint8_t type = (opt == 'E') ? RAMDISK_TYPE_EMS : RAMDISK_TYPE_XMS;

        if (type == RAMDISK_TYPE_XMS && os_cpu_type() < CPU_TYPE_80286) {
            os_puts("Error: XMS memory requires an 80286 or higher CPU.\n");
            return 1;
        }

        while (*args == ' ' || *args == '\t' || *args == ':') args++;
        if (*args != '\0') {
            req_size = parse_size(args);
        } else {
            req_size = 0; /* default to MAX */
        }

        res = os_ramdisk_mount(type, req_size, (uint16_t __far *)FP_HERE(&mounted_size));
        if (res == 0) {
            os_puts("RAM: mounted successfully in ");
            os_puts(type == RAMDISK_TYPE_EMS ? "EMS memory (" : "XMS memory (");
            print_u16(mounted_size);
            os_puts(" KB, FAT12).\n");
            os_puts("Use 'RAM:' to access the disk, or 'cd RAM:' in the shell.\n");
            return 0;
        } else if (res == -2) {
            if (type == RAMDISK_TYPE_EMS) {
                os_puts("Error: EMS memory manager not detected or not present.\n");
            } else {
                os_puts("Error: XMS driver not detected or no extended memory available.\n");
            }
            return 1;
        } else if (res == -3) {
            os_puts("Error: Insufficient free memory to allocate requested RAM disk size.\n");
            return 1;
        } else if (res == -4) {
            os_puts("Error: RAM disk is already mounted. Use 'ramdisk /u' to unmount first.\n");
            return 1;
        } else if (res == -5) {
            os_puts("Error: Storage volume table is full.\n");
            return 1;
        } else {
            os_puts("Error: Failed to mount RAM disk.\n");
            return 1;
        }
    }

    print_usage();
    return 1;
}
