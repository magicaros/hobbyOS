/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * XMS (Extended Memory Specification 2.0) driver and A20 line manager.
 * Supports INT 0x2F (AX=0x4300 / AX=0x4310) and INT 0x21 extended memory services.
 */

#include "kernel.h"

#define XMS_MAX_HANDLES  16
#define XMS_EMB_START_KB 64  /* EMB blocks start above HMA (first 64 KB of extended memory) */

typedef struct {
    uint8_t  used;
    uint8_t  locked;
    uint16_t owner_seg;
    uint32_t base_kb;
    uint32_t size_kb;
} xms_handle_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t limit;      /* Segment limit (bytes - 1) */
    uint16_t base_low;   /* Base address bits 0..15 */
    uint8_t  base_mid;   /* Base address bits 16..23 */
    uint8_t  access;     /* Access rights byte (0x93 for R/W data) */
    uint16_t reserved;   /* Reserved / 0 */
} gdt_desc_t;

typedef struct {
    gdt_desc_t dummy;    /* 0x00: Null descriptor */
    gdt_desc_t gdt_self; /* 0x08: Points to GDT itself */
    gdt_desc_t src;      /* 0x10: Source descriptor */
    gdt_desc_t dst;      /* 0x18: Destination descriptor */
    gdt_desc_t bios_cs;  /* 0x20: BIOS CS */
    gdt_desc_t bios_ss;  /* 0x28: BIOS SS */
} xms_gdt_table_t;
#pragma pack(pop)

static uint8_t         xms_present   = 0;
static uint16_t        xms_total_mem = 0; /* KB above 1MB */
static uint8_t         hma_allocated = 0;
static xms_handle_t    xms_handles[XMS_MAX_HANDLES];
static xms_gdt_table_t xms_gdt;

int xms_init(void) {
    uint32_t gdt_phys;
    int i;

    /* XMS requires at least an Intel 80286 processor */
    if (cpu_get_type() < CPU_TYPE_80286) {
        xms_present = 0;
        return 0;
    }

    /* Query BIOS extended memory size in KB above 1 MB */
    xms_total_mem = bios_get_ext_mem_kb();
    if (xms_total_mem == 0) {
        xms_present = 0;
        return 0;
    }

    /* Enable A20 address line */
    if (!a20_enable()) {
        xms_present = 0;
        return 0;
    }

    /* Clear handles */
    for (i = 0; i < XMS_MAX_HANDLES; i++) {
        xms_handles[i].used = 0;
        xms_handles[i].locked = 0;
        xms_handles[i].owner_seg = 0;
        xms_handles[i].base_kb = 0;
        xms_handles[i].size_kb = 0;
    }
    hma_allocated = 0;

    /* Set up GDT for INT 0x15, AH=0x87 block move */
    gdt_phys = ((uint32_t)KERNEL_SEG << 4) + (uint16_t)&xms_gdt;

    xms_gdt.dummy.limit = 0;
    xms_gdt.dummy.base_low = 0;
    xms_gdt.dummy.base_mid = 0;
    xms_gdt.dummy.access = 0;
    xms_gdt.dummy.reserved = 0;

    xms_gdt.gdt_self.limit = sizeof(xms_gdt_table_t) - 1;
    xms_gdt.gdt_self.base_low = (uint16_t)(gdt_phys & 0xFFFF);
    xms_gdt.gdt_self.base_mid = (uint8_t)((gdt_phys >> 16) & 0xFF);
    xms_gdt.gdt_self.access = 0x93;
    xms_gdt.gdt_self.reserved = 0;

    xms_gdt.bios_cs.limit = 0;
    xms_gdt.bios_cs.base_low = 0;
    xms_gdt.bios_cs.base_mid = 0;
    xms_gdt.bios_cs.access = 0;
    xms_gdt.bios_cs.reserved = 0;

    xms_gdt.bios_ss.limit = 0;
    xms_gdt.bios_ss.base_low = 0;
    xms_gdt.bios_ss.base_mid = 0;
    xms_gdt.bios_ss.access = 0;
    xms_gdt.bios_ss.reserved = 0;

    /* Install INT 0x2F Multiplex ISR */
    xms_install_vector();

    xms_present = 1;
    return 1;
}

int xms_is_present(void) {
    return (int)xms_present;
}

uint16_t xms_total_kb(void) {
    return xms_present ? xms_total_mem : 0;
}

uint16_t xms_avail_kb(void) {
    uint16_t used_kb = 0;
    uint16_t pool_kb;
    int i;

    if (!xms_present) return 0;

    for (i = 0; i < XMS_MAX_HANDLES; i++) {
        if (xms_handles[i].used) {
            used_kb += (uint16_t)xms_handles[i].size_kb;
        }
    }
    pool_kb = (xms_total_mem > XMS_EMB_START_KB) ? (xms_total_mem - XMS_EMB_START_KB) : 0;
    return (used_kb < pool_kb) ? (pool_kb - used_kb) : 0;
}

uint16_t xms_largest_free_kb(void) {
    uint32_t top = XMS_EMB_START_KB;
    int i;

    if (!xms_present) return 0;

    for (i = 0; i < XMS_MAX_HANDLES; i++) {
        if (xms_handles[i].used) {
            uint32_t end_kb = xms_handles[i].base_kb + xms_handles[i].size_kb;
            if (end_kb > top) {
                top = end_kb;
            }
        }
    }
    if (top < (uint32_t)xms_total_mem) {
        return (uint16_t)(xms_total_mem - top);
    }
    return 0;
}

uint16_t xms_alloc(uint16_t size_kb, uint16_t owner_seg) {
    uint32_t top = XMS_EMB_START_KB;
    int free_idx = -1;
    int i;

    if (!xms_present || size_kb == 0) return 0;

    for (i = 0; i < XMS_MAX_HANDLES; i++) {
        if (!xms_handles[i].used) {
            if (free_idx < 0) free_idx = i;
        } else {
            uint32_t end_kb = xms_handles[i].base_kb + xms_handles[i].size_kb;
            if (end_kb > top) {
                top = end_kb;
            }
        }
    }

    if (free_idx < 0) return 0;
    if (top + size_kb > (uint32_t)xms_total_mem) return 0;

    xms_handles[free_idx].used = 1;
    xms_handles[free_idx].locked = 0;
    xms_handles[free_idx].owner_seg = owner_seg;
    xms_handles[free_idx].base_kb = top;
    xms_handles[free_idx].size_kb = size_kb;

    return (uint16_t)(free_idx + 1);
}

int xms_free(uint16_t handle) {
    int idx;
    if (!xms_present || handle < 1 || handle > XMS_MAX_HANDLES) return -1;
    idx = handle - 1;
    if (!xms_handles[idx].used) return -1;

    xms_handles[idx].used = 0;
    xms_handles[idx].locked = 0;
    xms_handles[idx].owner_seg = 0;
    xms_handles[idx].base_kb = 0;
    xms_handles[idx].size_kb = 0;
    return 0;
}

void xms_free_by_owner(uint16_t owner_seg) {
    int i;
    if (!xms_present) return;

    for (i = 0; i < XMS_MAX_HANDLES; i++) {
        if (xms_handles[i].used && xms_handles[i].owner_seg == owner_seg) {
            xms_handles[i].used = 0;
            xms_handles[i].locked = 0;
            xms_handles[i].owner_seg = 0;
            xms_handles[i].base_kb = 0;
            xms_handles[i].size_kb = 0;
        }
    }
}

int xms_move(const os_xms_move_t __far *req) {
    uint32_t src_phys, dst_phys;
    uint32_t remaining, cur_src, cur_dst;

    if (!xms_present || !req || req->length == 0) return -1;

    /* Resolve source physical address */
    if (req->src_handle == 0) {
        uint16_t seg = (uint16_t)(req->src_offset >> 16);
        uint16_t off = (uint16_t)(req->src_offset & 0xFFFF);
        src_phys = ((uint32_t)seg << 4) + off;
    } else {
        int idx = req->src_handle - 1;
        if (req->src_handle > XMS_MAX_HANDLES || !xms_handles[idx].used) return -1;
        if (req->src_offset + req->length > (xms_handles[idx].size_kb << 10)) return -1;
        src_phys = 0x100000UL + (xms_handles[idx].base_kb << 10) + req->src_offset;
    }

    /* Resolve destination physical address */
    if (req->dest_handle == 0) {
        uint16_t seg = (uint16_t)(req->dest_offset >> 16);
        uint16_t off = (uint16_t)(req->dest_offset & 0xFFFF);
        dst_phys = ((uint32_t)seg << 4) + off;
    } else {
        int idx = req->dest_handle - 1;
        if (req->dest_handle > XMS_MAX_HANDLES || !xms_handles[idx].used) return -1;
        if (req->dest_offset + req->length > (xms_handles[idx].size_kb << 10)) return -1;
        dst_phys = 0x100000UL + (xms_handles[idx].base_kb << 10) + req->dest_offset;
    }

    remaining = req->length;
    cur_src   = src_phys;
    cur_dst   = dst_phys;

    while (remaining > 0) {
        uint32_t chunk = remaining;
        uint16_t word_count;
        int rc;

        if (chunk > 65534UL) {
            chunk = 65534UL;
        }
        word_count = (uint16_t)((chunk + 1) >> 1);

        xms_gdt.src.limit = (uint16_t)(chunk - 1);
        xms_gdt.src.base_low = (uint16_t)(cur_src & 0xFFFF);
        xms_gdt.src.base_mid = (uint8_t)((cur_src >> 16) & 0xFF);
        xms_gdt.src.access = 0x93;
        xms_gdt.src.reserved = 0;

        xms_gdt.dst.limit = (uint16_t)(chunk - 1);
        xms_gdt.dst.base_low = (uint16_t)(cur_dst & 0xFFFF);
        xms_gdt.dst.base_mid = (uint8_t)((cur_dst >> 16) & 0xFF);
        xms_gdt.dst.access = 0x93;
        xms_gdt.dst.reserved = 0;

        rc = bios_move_ext_mem((void __far *)&xms_gdt, word_count);
        if (rc != 0) {
            return -1;
        }

        cur_src   += chunk;
        cur_dst   += chunk;
        remaining -= chunk;
    }

    return 0;
}

int xms_rw_sector(uint16_t handle, uint32_t byte_offset, uint8_t is_write, void __far *buf) {
    os_xms_move_t req;

    if (!xms_present || handle == 0 || !buf) {
        return -1;
    }

    req.length = 512;
    if (is_write) {
        req.src_handle = 0;
        req.src_offset = ((uint32_t)FP_SEG(buf) << 16) | FP_OFF(buf);
        req.dest_handle = handle;
        req.dest_offset = byte_offset;
    } else {
        req.src_handle = handle;
        req.src_offset = byte_offset;
        req.dest_handle = 0;
        req.dest_offset = ((uint32_t)FP_SEG(buf) << 16) | FP_OFF(buf);
    }
    return xms_move(&req);
}

void xms_dispatch(syscall_regs_t __far *regs) {
    uint8_t func = (uint8_t)(regs->ax >> 8);

    switch (func) {
    case 0x00: /* Get XMS Version */
        regs->ax = 0x0200; /* XMS 2.0 */
        regs->bx = 0x0100; /* Internal revision 1.00 */
        regs->dx = 1;      /* HMA present */
        break;

    case 0x01: /* Request HMA */
        if (!hma_allocated) {
            hma_allocated = 1;
            regs->ax = 1;
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0x91;
        }
        break;

    case 0x02: /* Release HMA */
        if (hma_allocated) {
            hma_allocated = 0;
            regs->ax = 1;
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0x93;
        }
        break;

    case 0x03: /* Global Enable A20 */
    case 0x05: /* Local Enable A20 */
        a20_enable();
        regs->ax = 1;
        break;

    case 0x04: /* Global Disable A20 */
    case 0x06: /* Local Disable A20 */
        a20_disable();
        regs->ax = 1;
        break;

    case 0x07: /* Query A20 */
        regs->ax = (uint16_t)a20_test();
        regs->bx = regs->bx & 0xFF00;
        break;

    case 0x08: /* Query Free Extended Memory */
        regs->ax = xms_largest_free_kb();
        regs->dx = xms_avail_kb();
        regs->bx = regs->bx & 0xFF00;
        break;

    case 0x09: { /* Allocate Extended Memory Block */
        uint16_t h = xms_alloc(regs->dx, regs->cs);
        if (h != 0) {
            regs->ax = 1;
            regs->dx = h;
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0xA0;
        }
        break;
    }

    case 0x0A: /* Free Extended Memory Block */
        if (xms_free(regs->dx) == 0) {
            regs->ax = 1;
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0xA2;
        }
        break;

    case 0x0B: { /* Move Extended Memory Block */
        const os_xms_move_t __far *req = (const os_xms_move_t __far *)MK_FP(regs->ds, regs->si);
        if (xms_move(req) == 0) {
            regs->ax = 1;
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0xA7;
        }
        break;
    }

    case 0x0C: { /* Lock Extended Memory Block */
        int idx = regs->dx - 1;
        if (regs->dx >= 1 && regs->dx <= XMS_MAX_HANDLES && xms_handles[idx].used) {
            uint32_t lin = 0x100000UL + (xms_handles[idx].base_kb << 10);
            xms_handles[idx].locked++;
            regs->ax = 1;
            regs->dx = (uint16_t)(lin >> 16);
            regs->bx = (uint16_t)(lin & 0xFFFF);
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0xA2;
        }
        break;
    }

    case 0x0D: { /* Unlock Extended Memory Block */
        int idx = regs->dx - 1;
        if (regs->dx >= 1 && regs->dx <= XMS_MAX_HANDLES && xms_handles[idx].used && xms_handles[idx].locked > 0) {
            xms_handles[idx].locked--;
            regs->ax = 1;
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0xAA;
        }
        break;
    }

    case 0x0E: { /* Get Handle Information */
        int idx = regs->dx - 1;
        if (regs->dx >= 1 && regs->dx <= XMS_MAX_HANDLES && xms_handles[idx].used) {
            regs->ax = 1;
            regs->dx = (uint16_t)xms_handles[idx].size_kb;
            regs->bx = (uint16_t)xms_handles[idx].locked << 8;
        } else {
            regs->ax = 0;
            regs->bx = (regs->bx & 0xFF00) | 0xA2;
        }
        break;
    }

    default:
        regs->ax = 0;
        regs->bx = (regs->bx & 0xFF00) | 0x80;
        break;
    }
}
