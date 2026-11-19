/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * hobbyOS kernel main initialization.
 */

#include "kernel.h"

static uint8_t  fpu_detected = 0;
static uint16_t detected_cpu_type = CPU_TYPE_8088;

uint16_t cpu_get_type(void) {
    return detected_cpu_type;
}

const char *cpu_get_name(void) {
    return os_cpu_name(detected_cpu_type);
}

int fpu_is_present(void) {
    return (int)fpu_detected;
}

static int verify_fpu_math(void) {
    double a = 3.14159, b = 2.0, c;
    c = a * b; /* Compiles directly to 8087 FMUL + FWAIT */
    return (c > 6.28 && c < 6.29) ? 1 : 0;
}

void kmain(void) {
    int rc;

    serial_init();
    con_init();
    kbd_init();

    con_set_attr(ATTR_CYAN);
    con_puts("\n[BOOK8088] Booted hobbyOS kernel at segment 0x9000!\n");
    con_set_attr(ATTR_DEFAULT);
    con_puts("[IRQ1 KBD] Custom INT 0x09 driver active (default layout: US)\n");

    detected_cpu_type = cpu_detect_type();
    con_puts("[CPU] Detected: ");
    con_puts(cpu_get_name());
    if (detected_cpu_type == CPU_TYPE_8088 || detected_cpu_type == CPU_TYPE_V20 || detected_cpu_type == CPU_TYPE_80188) {
        con_puts(" (8-bit bus, 4-byte prefetch queue)\n");
    } else if (detected_cpu_type == CPU_TYPE_8086 || detected_cpu_type == CPU_TYPE_V30 || detected_cpu_type == CPU_TYPE_80186) {
        con_puts(" (16-bit bus, 6-byte prefetch queue)\n");
    } else {
        con_puts("\n");
    }

    con_puts("[VIDEO] Detected: ");
    con_puts(video_get_name());
    if (video_get_type() == VIDEO_TYPE_CGA) {
        con_puts(" (snow prevention active)\n");
    } else if (video_get_type() == VIDEO_TYPE_VGA || video_get_type() == VIDEO_TYPE_EGA) {
        con_puts(" (snow-free)\n");
    } else if (video_get_type() == VIDEO_TYPE_MDA) {
        con_puts(" (monochrome text at 0xB000)\n");
    } else {
        con_puts("\n");
    }

    mem_init();
    con_puts("[MEM MCB] Conventional arena 0x0060..0x8FFF initialized (");
    con_put_uint((uint32_t)(mem_avail_paragraphs() >> 6));
    con_puts(" KB free)\n");

    if (fpu_8087_init()) {
        fpu_detected = 1;
        if (verify_fpu_math()) {
            con_puts("[x87 FPU] Detected & math verified OK\n");
        }
    } else {
        fpu_detected = 0;
        con_puts("[x87 FPU] Not found\n");
    }

    if (ems_init()) {
        con_puts("[LOTECH EMS] 2MB manager ready (");
        con_put_uint((uint32_t)ems_avail_pages());
        con_puts(" x 16KB pages free)\n");
    } else {
        con_puts("[LOTECH EMS] Not found\n");
    }

    if (xms_init()) {
        con_puts("[XMS] 2.0 driver ready (");
        con_put_uint((uint32_t)xms_avail_kb());
        con_puts(" KB extended memory free)\n");
    } else {
        if (cpu_get_type() < CPU_TYPE_80286) {
            con_puts("[XMS] Not supported (requires 80286+ CPU)\n");
        } else {
            con_puts("[XMS] Not found (0 KB extended memory)\n");
        }
    }

    if (fat16_init() == 0) {
        int v, total_v = vol_get_count();
        con_puts("[STORAGE] Mounted volumes: ");
        for (v = 0; v < total_v; v++) {
            os_volume_t *vol = vol_get((uint8_t)v);
            if (vol && vol->active) {
                con_puts(vol->name);
                con_putc(':');
                if (vol->is_alias) {
                    con_puts("->");
                    if (vol->target_vol_idx < MAX_VOLUMES) {
                        os_volume_t *tgt = vol_get(vol->target_vol_idx);
                        if (tgt) {
                            con_puts(tgt->name);
                            con_putc(':');
                        }
                    }
                }
                if (v + 1 < total_v) {
                    con_puts(", ");
                }
            }
        }
        con_puts(" OK\n");
    } else {
        con_puts("[STORAGE] Mount FAILED\n");
    }

    syscall_init();
    con_puts("[SYSCALL] INT 0x21 API installed OK\n");

    if (mouse_init()) {
        con_puts("[MOUSE] ");
        con_puts(mouse_get_type_name());
        con_puts(" ready (INT 0x33, ");
        con_put_uint((uint32_t)mouse_get_buttons());
        con_puts(" buttons)\n");
    } else {
        con_puts("[MOUSE] Not detected (INT 0x33 stub active)\n");
    }

    sound_init();
    con_puts("[SOUND] ");
    con_puts(sound_get_device_name());
    con_puts(" ready\n");
    if (sound_get_sb_type() != SB_TYPE_NONE) {
        con_puts("[SOUND] ");
        con_puts(sound_get_sb_name());
        con_puts(" detected at port ");
        con_put_hex16(sound_get_sb_base());
        con_puts("\n");
    }

    /* Run boot self-test (/BIN/HELLO.BIN) then launch user-space shell (/BIN/SHELL.BIN) */
    con_puts("[EXEC] Running /BIN/HELLO.BIN...\n");
    rc = exec_program((const char __far *)MK_FP(KERNEL_SEG, "/BIN/HELLO.BIN"),
                      (const char __far *)MK_FP(KERNEL_SEG, "boot-self-test"));
    con_puts("[EXEC] /BIN/HELLO.BIN exited with code ");
    con_put_uint((uint32_t)rc);
    con_putc('\n');

    for (;;) {
        rc = exec_program((const char __far *)MK_FP(KERNEL_SEG, "/BIN/SHELL.BIN"),
                          (const char __far *)MK_FP(KERNEL_SEG, ""));
        if (rc < 0) {
            con_puts("[PANIC] Failed to load /BIN/SHELL.BIN\n");
            for (;;);
        }
    }
}
