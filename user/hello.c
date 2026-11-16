/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Sample user-space executable (HELLO.BIN) demonstrating hobbyOS INT 0x21
 * system calls: screen output, command-line arguments, and FAT16 file read/write.
 */

#include "../include/hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

static void print_uint(uint32_t val) {
    char buf[12];
    int i = 0;
    uint16_t v = (uint16_t)val;
    if (v == 0) {
        os_putc('0');
        return;
    }
    while (v > 0 && i < 11) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        os_putc(buf[--i]);
    }
}

int app_main(void) {
    static const char msg1[] = "Line 1 from user-space app\n";
    static const char msg2[] = "Line 2 appended via SYS_FILE_APPEND\n";
    char read_buf[96];
    os_dir_entry_t dent;
    const char *args = os_get_args();
    int n, idx;

    os_puts("[USER APP] HELLO.BIN running in user segment via INT 0x21!\n");
    if (args && args[0] != '\0') {
        os_puts("[USER APP] Args: ");
        os_puts(args);
        os_putc('\n');
    }

    /* Test CPU detection via SYS_CPU_TYPE (0x0B) */
    {
        uint16_t ctype = os_cpu_type();
        os_puts("[USER APP] CPU detected via INT 0x21: ");
        os_puts(os_cpu_name(ctype));
        os_putc('\n');
    }

    /* Test MCB conventional memory allocation & deallocation via INT 0x21 */
    {
        uint16_t blk_seg = os_mem_alloc(64); /* 1 KB (64 paragraphs) */
        if (blk_seg != 0) {
            volatile uint16_t __far *p = (volatile uint16_t __far *)(((unsigned long)blk_seg << 16) | 0);
            *p = 0xCAFE;
            if (*p == 0xCAFE && os_mem_free(blk_seg) == 0) {
                os_puts("[USER APP] MCB alloc/free verified OK\n");
            }
        }
    }

    /* Test Lo-tech EMS page allocation, mapping, & deallocation via INT 0x21 */
    {
        int ems_pg = os_ems_alloc();
        if (ems_pg >= 0) {
            uint16_t slot_seg = os_ems_map(0, (uint16_t)ems_pg);
            volatile uint16_t __far *ep = (volatile uint16_t __far *)(((unsigned long)slot_seg << 16) | 0);
            *ep = 0xBEEF;
            if (*ep == 0xBEEF && os_ems_free((uint16_t)ems_pg) == 0) {
                os_puts("[USER APP] EMS alloc/map/free verified OK\n");
            }
        }
    }

    /* Test custom IRQ 1 keyboard layout syscalls (load FR.MAP, BE.MAP, restore US.MAP) */
    {
        static os_keymap_t kmap;
        char kname[8];
        if (os_keyb_get_name((char __far *)FP_HERE(kname)) == 0 &&
            os_file_read("/KEYMAPS/FR.MAP", FP_HERE(&kmap), sizeof(kmap)) == (int)sizeof(kmap) &&
            os_keyb_set_map((const os_keymap_t __far *)FP_HERE(&kmap)) == 0 &&
            os_file_read("/KEYMAPS/BE.MAP", FP_HERE(&kmap), sizeof(kmap)) == (int)sizeof(kmap) &&
            os_keyb_set_map((const os_keymap_t __far *)FP_HERE(&kmap)) == 0 &&
            os_file_read("/KEYMAPS/US.MAP", FP_HERE(&kmap), sizeof(kmap)) == (int)sizeof(kmap) &&
            os_keyb_set_map((const os_keymap_t __far *)FP_HERE(&kmap)) == 0 &&
            os_keyb_state(0x01) == 0) {
            os_puts("[USER APP] Keymaps (US/FR/BE) & IRQ1 state verified OK\n");
        }
    }

    /* Test creating a subdirectory and writing + appending a file inside it */
    os_mkdir("/DOCS");
    n = os_file_write("/DOCS/NOTES.TXT", FP_HERE(msg1), sizeof(msg1) - 1);
    if (n > 0 && os_file_append("/DOCS/NOTES.TXT", FP_HERE(msg2), sizeof(msg2) - 1) > 0) {
        os_puts("[USER APP] Wrote & appended /DOCS/NOTES.TXT OK\n");
    } else {
        os_puts("[USER APP] Failed to write/append /DOCS/NOTES.TXT\n");
        return 1;
    }

    /* Test reading the file back from /DOCS/NOTES.TXT via SYS_FILE_READ */
    n = os_file_read("/DOCS/NOTES.TXT", FP_HERE(read_buf), sizeof(read_buf) - 1);
    if (n >= 0) {
        read_buf[n] = '\0';
        os_puts("[USER APP] Read back /DOCS/NOTES.TXT: \"");
        os_puts(read_buf);
        os_puts("\"\n");
    } else {
        os_puts("[USER APP] Failed to read /DOCS/NOTES.TXT\n");
        return 2;
    }

    /* List FAT16 Root Directory contents via SYS_DIR_ENTRY */
    os_puts("[USER APP] FAT16 Root Directory:\n");
    for (idx = 0; os_dir_entry((uint16_t)idx, (os_dir_entry_t __far *)FP_HERE(&dent)) == 0; idx++) {
        os_puts("  - ");
        os_puts(dent.name);
        if (dent.attr & OS_ATTR_DIRECTORY) {
            os_puts("/ <DIR>\n");
        } else {
            os_puts(" (");
            print_uint(dent.size);
            os_puts(" bytes)\n");
        }
    }

    return 0;
}
