/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Lo-tech 2MB EMS page allocator and bank-switching manager.
 * 128 physical 16 KB pages mapped into 4 slots at segment 0xE000 (I/O 0x260..0x263).
 */

#include "kernel.h"

static uint16_t ems_page_owner[EMS_TOTAL_PAGES];
static uint8_t  ems_slot_page[EMS_NUM_SLOTS];
static uint8_t  ems_present = 0;

static void ems_hw_map(uint8_t slot, uint8_t phys_page) {
    uint8_t s = (uint8_t)(slot & 0x03);
    uint8_t p = (uint8_t)(phys_page & 0x7F);
    ems_slot_page[s] = p;
    outb(EMS_IO_BASE + s, p);
}

int ems_init(void) {
    volatile uint16_t __far *slot0 = (volatile uint16_t __far *)MK_FP(EMS_FRAME_SEG, 0x0000);
    uint16_t i;
    int ok = 1;

    for (i = 0; i < EMS_TOTAL_PAGES; i++) {
        ems_page_owner[i] = 0;
    }

    /* Hardware bank-switching verification between physical page 0 and page 1 */
    ems_hw_map(0, 0);
    *slot0 = 0xAA55;
    ems_hw_map(0, 1);
    *slot0 = 0x1234;

    ems_hw_map(0, 0);
    if (*slot0 != 0xAA55) {
        ok = 0;
    }
    *slot0 = 0x0000;

    ems_hw_map(0, 1);
    if (*slot0 != 0x1234) {
        ok = 0;
    }
    *slot0 = 0x0000;

    if (!ok) {
        ems_present = 0;
        return 0;
    }

    for (i = 0; i < EMS_NUM_SLOTS; i++) {
        ems_hw_map((uint8_t)i, (uint8_t)i);
    }

    ems_present = 1;
    return 1;
}

int ems_alloc_page(uint16_t owner) {
    uint16_t i;

    if (!ems_present) {
        return -1;
    }
    if (owner == 0) {
        owner = KERNEL_SEG;
    }
    for (i = 0; i < EMS_TOTAL_PAGES; i++) {
        if (ems_page_owner[i] == 0) {
            ems_page_owner[i] = owner;
            return (int)i;
        }
    }
    return -1;
}

int ems_free_page(uint16_t page) {
    if (!ems_present || page >= EMS_TOTAL_PAGES || ems_page_owner[page] == 0) {
        return -1;
    }
    ems_page_owner[page] = 0;
    return 0;
}

void ems_free_by_owner(uint16_t owner) {
    uint16_t i;
    if (!ems_present || owner == 0) {
        return;
    }
    for (i = 0; i < EMS_TOTAL_PAGES; i++) {
        if (ems_page_owner[i] == owner) {
            ems_page_owner[i] = 0;
        }
    }
}

uint16_t ems_map_page(uint8_t slot, uint16_t page) {
    if (!ems_present || slot >= EMS_NUM_SLOTS || page >= EMS_TOTAL_PAGES ||
        ems_page_owner[page] == 0) {
        return 0xFFFF;
    }
    ems_hw_map(slot, (uint8_t)page);
    return EMS_FRAME_SEG + ((uint16_t)slot * 0x0400);
}

uint16_t ems_avail_pages(void) {
    uint16_t i, count = 0;
    if (!ems_present) {
        return 0;
    }
    for (i = 0; i < EMS_TOTAL_PAGES; i++) {
        if (ems_page_owner[i] == 0) {
            count++;
        }
    }
    return count;
}

uint16_t ems_total_pages(void) {
    return ems_present ? EMS_TOTAL_PAGES : 0;
}

void ems_save_slots(uint8_t out_slots[EMS_NUM_SLOTS]) {
    uint8_t i;
    for (i = 0; i < EMS_NUM_SLOTS; i++) {
        out_slots[i] = ems_slot_page[i];
    }
}

void ems_restore_slots(const uint8_t in_slots[EMS_NUM_SLOTS]) {
    uint8_t i;
    if (!ems_present) {
        return;
    }
    for (i = 0; i < EMS_NUM_SLOTS; i++) {
        ems_hw_map(i, in_slots[i]);
    }
}

int ems_rw_sector(uint8_t phys_page, uint16_t sec_offset, uint8_t is_write, void __far *buf) {
    uint8_t old_page;
    volatile uint16_t __far *dst;
    volatile uint16_t __far *src;
    uint16_t i;

    if (!ems_present || phys_page >= EMS_TOTAL_PAGES || sec_offset > 15872) {
        return -1;
    }

    old_page = ems_slot_page[0];
    ems_hw_map(0, phys_page);

    if (is_write) {
        dst = (volatile uint16_t __far *)MK_FP(EMS_FRAME_SEG, sec_offset);
        src = (volatile uint16_t __far *)buf;
    } else {
        dst = (volatile uint16_t __far *)buf;
        src = (volatile uint16_t __far *)MK_FP(EMS_FRAME_SEG, sec_offset);
    }
    for (i = 0; i < 256; i++) {
        dst[i] = src[i];
    }

    ems_hw_map(0, old_page);
    return 0;
}
