/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MEM.BIN - MS-DOS style memory usage utility for hobbyOS.
 * Reports Conventional Memory (MCB arena + system regions) and Lo-tech 2MB EMS.
 * Hidden easter egg: `mem -altman`
 */

#include "../include/hobbyos.h"

#define MEM_START_SEG    0x0060
#define MEM_END_SEG      0x9000
#define TOTAL_CONV_KB    640
#define TOTAL_EMS_PAGES  128
#define TOTAL_EMS_KB     2048

#pragma pack(push, 1)
typedef struct {
    uint8_t  type;
    uint16_t owner;
    uint16_t size;
    uint8_t  reserved[11];
} mcb_t;
#pragma pack(pop)

#define MK_FP(seg, off) ((void __far *)(((unsigned long)(seg) << 16) | (uint16_t)(off)))

static void print_uint16_pad(uint16_t val, int width) {
    char buf[8];
    int i = 0;
    int pad;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0 && i < 7) {
            buf[i++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }
    for (pad = i; pad < width; pad++) {
        os_putc(' ');
    }
    while (i > 0) {
        os_putc(buf[--i]);
    }
}

static void print_uint16_comma(uint16_t val) {
    if (val >= 1000) {
        uint16_t thousands = val / 1000;
        uint16_t rem = val % 1000;
        print_uint16_pad(thousands, 1);
        os_putc(',');
        os_putc((char)('0' + (rem / 100)));
        os_putc((char)('0' + ((rem / 10) % 10)));
        os_putc((char)('0' + (rem % 10)));
    } else {
        print_uint16_pad(val, 1);
    }
}

static void print_hex16(uint16_t val) {
    static const char hex[] = "0123456789ABCDEF";
    os_putc(hex[(val >> 12) & 0x0F]);
    os_putc(hex[(val >> 8) & 0x0F]);
    os_putc(hex[(val >> 4) & 0x0F]);
    os_putc(hex[val & 0x0F]);
}

static int is_altman_arg(const char *args) {
    static const char target[] = "altman";
    int i = 0;

    if (!args) {
        return 0;
    }
    while (*args == ' ') {
        args++;
    }
    if (*args != '-' && *args != '/') {
        return 0;
    }
    args++;
    while (target[i] != '\0') {
        char c = args[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        if (c != target[i]) {
            return 0;
        }
        i++;
    }
    return (args[i] == '\0' || args[i] == ' ');
}

static void print_altman_easter_egg(int ems_present, uint16_t free_ems_pages) {
    uint16_t pct;
    uint16_t free_kib;

    if (!ems_present) {
        return;
    }
    pct = (free_ems_pages * 100) >> 7;
    if (pct <= 10) {
        return;
    }
    free_kib = free_ems_pages << 4;

    os_puts("\nSubject: ");
    print_uint16_pad(pct, 1);
    os_puts("% of my Lo-tech EMS remains at large\n\n");

    os_puts("Hi Sam,\n\n");

    os_puts("I hear RAM is hard to come by these days, so it feels a bit rude to be\n");
    os_puts("sitting on ");
    print_uint16_comma(free_kib);
    os_puts(" KiB (");
    print_uint16_pad(free_ems_pages, 1);
    os_puts("/128 pages) of pristine 1980s-style SRAM doing\n");
    os_puts("absolutely nothing at 0xE000. It's not for sale, I just like knowing\n");
    os_puts("it's there.\n\n");

    os_puts("Hoarding responsibly,\n");
    os_puts("A concerned 8088 owner\n");
}

int app_main(void) {
    const char *args     = os_get_args();
    uint16_t free_paras  = os_mem_avail();
    uint16_t free_conv_kb = free_paras >> 6; /* 64 paragraphs = 1 KB */
    uint16_t used_conv_kb = TOTAL_CONV_KB - free_conv_kb;

    uint16_t total_ems_pages = os_ems_total();
    uint16_t free_ems_pages  = os_ems_avail();
    int      ems_present     = (total_ems_pages > 0);
    uint16_t used_ems_pages  = (free_ems_pages <= total_ems_pages)
                                   ? (total_ems_pages - free_ems_pages)
                                   : 0;
    uint16_t total_ems_kb    = total_ems_pages << 4; /* 16 KB per page */
    uint16_t free_ems_kb     = free_ems_pages << 4;
    uint16_t used_ems_kb     = used_ems_pages << 4;

    uint16_t total_xms_kb    = os_xms_total();
    uint16_t free_xms_kb     = os_xms_avail();
    int      xms_present     = (total_xms_kb > 0);
    uint16_t used_xms_kb     = (free_xms_kb <= total_xms_kb) ? (total_xms_kb - free_xms_kb) : 0;

    uint16_t total_kb = TOTAL_CONV_KB + total_ems_kb + total_xms_kb;
    uint16_t total_used_kb = used_conv_kb + used_ems_kb + used_xms_kb;
    uint16_t total_free_kb = free_conv_kb + free_ems_kb + free_xms_kb;

    uint16_t cur_seg = MEM_START_SEG;
    uint16_t largest_free_paras = 0;

    if (is_altman_arg(args)) {
        print_altman_easter_egg(ems_present, free_ems_pages);
        return 0;
    }

    os_puts("\nMemory Type        Total       Used       Free\n");
    os_puts("----------------  --------   --------   --------\n");

    os_puts("Conventional      ");
    print_uint16_pad(TOTAL_CONV_KB, 5);
    os_puts(" KB   ");
    print_uint16_pad(used_conv_kb, 5);
    os_puts(" KB   ");
    print_uint16_pad(free_conv_kb, 5);
    os_puts(" KB\n");

    os_puts("EMS (Lo-tech 2M)  ");
    print_uint16_pad(total_ems_kb, 5);
    os_puts(" KB   ");
    print_uint16_pad(used_ems_kb, 5);
    os_puts(" KB   ");
    print_uint16_pad(free_ems_kb, 5);
    os_puts(" KB\n");

    if (xms_present) {
        os_puts("XMS (Ext Mem)     ");
        print_uint16_pad(total_xms_kb, 5);
        os_puts(" KB   ");
        print_uint16_pad(used_xms_kb, 5);
        os_puts(" KB   ");
        print_uint16_pad(free_xms_kb, 5);
        os_puts(" KB\n");
    }

    os_puts("----------------  --------   --------   --------\n");
    os_puts("Total Memory      ");
    print_uint16_pad(total_kb, 5);
    os_puts(" KB   ");
    print_uint16_pad(total_used_kb, 5);
    os_puts(" KB   ");
    print_uint16_pad(total_free_kb, 5);
    os_puts(" KB\n\n");

    /* Walk MCB chain to show block layout and largest contiguous free block */
    os_puts("MCB Chain (0x0060..0x8FFF) & System Map:\n");
    os_puts("  Seg     Size (Para)    Size (KB)   Status / Owner\n");
    os_puts("  ------  -----------    ---------   --------------\n");
    os_puts("  0x0000       0060h         1 KB    IVT & BIOS Data\n");

    while (cur_seg < MEM_END_SEG) {
        mcb_t __far *mcb = (mcb_t __far *)MK_FP(cur_seg, 0);
        if (mcb->type != 'M' && mcb->type != 'Z') {
            break;
        }
        os_puts("  0x");
        print_hex16(cur_seg + 1);
        os_puts("       ");
        print_hex16(mcb->size);
        os_puts("h     ");
        print_uint16_pad(mcb->size >> 6, 5);
        os_puts(" KB    ");
        if (mcb->owner == 0) {
            os_puts("Free\n");
            if (mcb->size > largest_free_paras) {
                largest_free_paras = mcb->size;
            }
        } else {
            os_puts("Used (owner 0x");
            print_hex16(mcb->owner);
            os_puts(")\n");
        }
        if (mcb->type == 'Z') {
            break;
        }
        cur_seg = cur_seg + 1 + mcb->size;
    }

    os_puts("  0x9000       1000h        64 KB    Kernel (KERNEL.BIN)\n\n");

    os_puts("Largest executable program size: ");
    print_uint16_pad(largest_free_paras >> 6, 1);
    os_puts(" KB (");
    print_uint16_pad(largest_free_paras, 1);
    os_puts(" paragraphs)\n");

    os_puts("EMS Pages available:             ");
    print_uint16_pad(free_ems_pages, 1);
    os_puts(" / ");
    print_uint16_pad(total_ems_pages, 1);
    if (ems_present) {
        os_puts(" (16 KB/page at 0xE000)\n");
    } else {
        os_puts(" (not present)\n");
    }

    os_puts("XMS Extended Memory:             ");
    if (xms_present) {
        print_uint16_pad(free_xms_kb, 1);
        os_puts(" / ");
        print_uint16_pad(total_xms_kb, 1);
        os_puts(" KB free (A20 enabled)\n");
    } else if (os_cpu_type() < CPU_TYPE_80286) {
        os_puts("Not supported (requires 80286+ CPU)\n");
    } else {
        os_puts("Not present (0 KB extended memory)\n");
    }

    os_puts("CPU Architecture:                ");
    os_puts(os_cpu_name(os_cpu_type()));
    os_puts("\nMath Coprocessor:                ");
    if (os_fpu_present()) {
        os_puts("x87 FPU detected\n");
    } else {
        os_puts("Not present (emulation required)\n");
    }

    os_puts("Video Adapter:                   ");
    os_puts(os_video_name(os_video_type()));
    os_putc('\n');

    return 0;
}
