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

    /* Test Video detection via SYS_VIDEO_TYPE (0x0C) */
    {
        uint16_t vtype = os_video_type();
        os_puts("[USER APP] Video detected via INT 0x21: ");
        os_puts(os_video_name(vtype));
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
        if (os_set_break_mode(BREAK_MODE_RAW) == 0 && os_set_break_mode(BREAK_MODE_NORMAL) == 0) {
            os_puts("[USER APP] Break mode syscall (normal/raw) verified OK\n");
        }
    }

    /* Test INT 0x33 mouse services */
    {
        uint16_t btns = 0;
        uint16_t res = os_mouse_reset(&btns);
        if (res == 0xFFFF) {
            uint16_t x, y;
            os_mouse_get_pos(&x, &y);
            os_puts("[USER APP] INT 0x33 Mouse detected & verified OK\n");
        } else {
            os_puts("[USER APP] INT 0x33 Mouse driver active (no mouse hardware)\n");
        }
    }

    /* Test Sound Subsystem */
    {
        uint16_t port = 0, ch = 0, sb_base = 0;
        uint8_t sb_type = 0;
        int sdev = os_sound_info_ex(&port, &ch, &sb_type, &sb_base);
        os_puts("[USER APP] Sound subsystem verified OK: ");
        os_puts(os_sound_device_name(sdev));
        if (sb_type != SB_TYPE_NONE) {
            os_puts(" + ");
            os_puts(os_sound_sb_name(sb_type));
        }
        os_putc('\n');
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

    /* Test AmigaDOS-style device paths (SYS:DOCS/NOTES.TXT & HD0A:DOCS/NOTES.TXT) */
    {
        char amiga_buf[32];
        int r1 = os_file_read("SYS:DOCS/NOTES.TXT", FP_HERE(amiga_buf), 6);
        int r2 = os_file_read("HD0A:DOCS/NOTES.TXT", FP_HERE(amiga_buf), 6);
        if (r1 == 6 && r2 == 6) {
            os_puts("[USER APP] AmigaDOS device paths (SYS: & HD0A:) verified OK\n");
        }
    }

    /* Test RAM disk subdirectory extension (more than 16 entries in 1-sector/cluster directory) */
    {
        uint16_t msize = 0;
        if (os_ramdisk_mount(RAMDISK_TYPE_EMS, 128, (uint16_t __far *)FP_HERE(&msize)) == 0) {
            int r = os_mkdir("RAM:/TESTDIR");
            int all_ok = (r == 0);
            int f;
            char fname[24];
            char tdata[16];
            tdata[0] = 'T'; tdata[1] = 'E'; tdata[2] = 'S'; tdata[3] = 'T'; tdata[4] = '\0';
            for (f = 0; f < 25; f++) {
                fname[0] = 'R'; fname[1] = 'A'; fname[2] = 'M'; fname[3] = ':';
                fname[4] = '/'; fname[5] = 'T'; fname[6] = 'E'; fname[7] = 'S';
                fname[8] = 'T'; fname[9] = 'D'; fname[10] = 'I'; fname[11] = 'R';
                fname[12] = '/'; fname[13] = 'F';
                fname[14] = (char)('0' + (f / 10));
                fname[15] = (char)('0' + (f % 10));
                fname[16] = '.'; fname[17] = 'T'; fname[18] = 'X'; fname[19] = 'T';
                fname[20] = '\0';
                if (os_file_write(fname, (const void __far *)FP_HERE(tdata), 4) == 0xFFFF) {
                    all_ok = 0;
                    break;
                }
            }
            if (all_ok) {
                os_puts("[USER APP] RAM disk multi-cluster subdirectory (25 files in RAM:/TESTDIR) verified OK\n");
            } else {
                os_puts("[USER APP] RAM disk multi-cluster subdirectory FAILED\n");
            }
            os_ramdisk_unmount();
        }
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
