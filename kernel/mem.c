/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Conventional memory DOS-style MCB paragraph allocator (0x0060..0x8FFF).
 */

#include "kernel.h"

#define MCB_TYPE_CONT 'M'
#define MCB_TYPE_LAST 'Z'

#pragma pack(push, 1)
typedef struct {
    uint8_t  type;         /* 'M' = more blocks follow, 'Z' = last block */
    uint16_t owner;        /* 0x0000 = free, else owning segment */
    uint16_t size;         /* Block payload size in 16-byte paragraphs */
    uint8_t  reserved[11];
} mcb_t;
#pragma pack(pop)

static void mem_coalesce(void) {
    uint16_t cur_seg = MEM_START_SEG;

    while (cur_seg < MEM_END_SEG) {
        mcb_t __far *cur = (mcb_t __far *)MK_FP(cur_seg, 0);
        if (cur->type != MCB_TYPE_CONT) {
            break;
        }
        {
            uint16_t next_seg = cur_seg + 1 + cur->size;
            mcb_t __far *next;
            if (next_seg >= MEM_END_SEG) {
                break;
            }
            next = (mcb_t __far *)MK_FP(next_seg, 0);
            if (next->type != MCB_TYPE_CONT && next->type != MCB_TYPE_LAST) {
                break;
            }
            if (cur->owner == 0 && next->owner == 0) {
                cur->size += 1 + next->size;
                cur->type  = next->type;
                next->type = 0;
                continue;
            }
            cur_seg = next_seg;
        }
    }
}

void mem_init(void) {
    mcb_t __far *first = (mcb_t __far *)MK_FP(MEM_START_SEG, 0);
    int i;

    first->type  = MCB_TYPE_LAST;
    first->owner = 0x0000;
    first->size  = (MEM_END_SEG - MEM_START_SEG) - 1;
    for (i = 0; i < 11; i++) {
        first->reserved[i] = 0;
    }
}

uint16_t mem_alloc(uint16_t paragraphs, uint16_t owner) {
    uint16_t cur_seg = MEM_START_SEG;

    if (paragraphs == 0) {
        return 0;
    }

    while (cur_seg < MEM_END_SEG) {
        mcb_t __far *cur = (mcb_t __far *)MK_FP(cur_seg, 0);
        if (cur->type != MCB_TYPE_CONT && cur->type != MCB_TYPE_LAST) {
            return 0;
        }

        if (cur->owner == 0 && cur->size >= paragraphs) {
            if (cur->size > paragraphs + 1) {
                uint16_t split_seg = cur_seg + 1 + paragraphs;
                mcb_t __far *split = (mcb_t __far *)MK_FP(split_seg, 0);
                int i;

                split->type  = cur->type;
                split->owner = 0x0000;
                split->size  = cur->size - paragraphs - 1;
                for (i = 0; i < 11; i++) {
                    split->reserved[i] = 0;
                }

                cur->type = MCB_TYPE_CONT;
                cur->size = paragraphs;
            }

            cur->owner = (owner != 0) ? owner : (uint16_t)(cur_seg + 1);
            return cur_seg + 1;
        }

        if (cur->type == MCB_TYPE_LAST) {
            break;
        }
        cur_seg = cur_seg + 1 + cur->size;
    }

    return 0;
}

int mem_free(uint16_t seg) {
    mcb_t __far *mcb;

    if (seg <= MEM_START_SEG || seg >= MEM_END_SEG) {
        return -1;
    }
    mcb = (mcb_t __far *)MK_FP(seg - 1, 0);
    if (mcb->type != MCB_TYPE_CONT && mcb->type != MCB_TYPE_LAST) {
        return -1;
    }
    mcb->owner = 0x0000;
    mem_coalesce();
    return 0;
}

int mem_resize(uint16_t seg, uint16_t new_paragraphs) {
    uint16_t mcb_seg;
    mcb_t __far *cur;

    if (new_paragraphs == 0 || seg <= MEM_START_SEG || seg >= MEM_END_SEG) {
        return -1;
    }
    if (new_paragraphs > (MEM_END_SEG - seg)) {
        return -1;
    }

    mcb_seg = seg - 1;
    cur = (mcb_t __far *)MK_FP(mcb_seg, 0);
    if ((cur->type != MCB_TYPE_CONT && cur->type != MCB_TYPE_LAST) || cur->owner == 0) {
        return -1;
    }

    if (new_paragraphs == cur->size) {
        return 0;
    }

    if (new_paragraphs < cur->size) {
        /* Shrinking block: need at least 2 freed paragraphs (1 for MCB header + >=1 payload),
         * unless the immediately following MCB is already free, in which case even a 1-paragraph
         * shrink can be merged by first absorbing the free neighbor and re-splitting.
         */
        uint16_t next_seg = mcb_seg + 1 + cur->size;
        if (cur->type == MCB_TYPE_CONT && next_seg < MEM_END_SEG) {
            mcb_t __far *next = (mcb_t __far *)MK_FP(next_seg, 0);
            if ((next->type == MCB_TYPE_CONT || next->type == MCB_TYPE_LAST) && next->owner == 0) {
                cur->size += 1 + next->size;
                cur->type  = next->type;
                next->type = 0;
            }
        }

        if (cur->size > new_paragraphs + 1) {
            uint16_t split_seg = mcb_seg + 1 + new_paragraphs;
            mcb_t __far *split = (mcb_t __far *)MK_FP(split_seg, 0);
            int i;

            split->type  = cur->type;
            split->owner = 0x0000;
            split->size  = cur->size - new_paragraphs - 1;
            for (i = 0; i < 11; i++) {
                split->reserved[i] = 0;
            }

            cur->type = MCB_TYPE_CONT;
            cur->size = new_paragraphs;
            mem_coalesce();
        }
        return 0;
    }

    /* Growing block in place: check if immediately following MCB is free and large enough */
    if (cur->type == MCB_TYPE_CONT) {
        uint16_t next_seg = mcb_seg + 1 + cur->size;
        if (next_seg < MEM_END_SEG) {
            mcb_t __far *next = (mcb_t __far *)MK_FP(next_seg, 0);
            if ((next->type == MCB_TYPE_CONT || next->type == MCB_TYPE_LAST) && next->owner == 0) {
                uint16_t combined = cur->size + 1 + next->size;
                if (combined >= new_paragraphs) {
                    uint8_t next_type = next->type;
                    next->type = 0;

                    if (combined > new_paragraphs + 1) {
                        uint16_t split_seg = mcb_seg + 1 + new_paragraphs;
                        mcb_t __far *split = (mcb_t __far *)MK_FP(split_seg, 0);
                        int i;

                        split->type  = next_type;
                        split->owner = 0x0000;
                        split->size  = combined - new_paragraphs - 1;
                        for (i = 0; i < 11; i++) {
                            split->reserved[i] = 0;
                        }

                        cur->type = MCB_TYPE_CONT;
                        cur->size = new_paragraphs;
                    } else {
                        cur->type = next_type;
                        cur->size = combined;
                    }
                    return 0;
                }
            }
        }
    }

    return -1;
}

uint16_t mem_realloc(uint16_t seg, uint16_t new_paragraphs, uint16_t owner) {
    mcb_t __far *old_mcb;
    uint16_t old_size;
    uint16_t blk_owner;
    uint16_t new_seg;
    uint16_t p;

    if (seg == 0) {
        return mem_alloc(new_paragraphs, owner);
    }
    if (new_paragraphs == 0) {
        mem_free(seg);
        return 0;
    }
    if (seg <= MEM_START_SEG || seg >= MEM_END_SEG) {
        return 0;
    }

    /* Fast path: try in-place resize first */
    if (mem_resize(seg, new_paragraphs) == 0) {
        return seg;
    }

    old_mcb = (mcb_t __far *)MK_FP(seg - 1, 0);
    if ((old_mcb->type != MCB_TYPE_CONT && old_mcb->type != MCB_TYPE_LAST) || old_mcb->owner == 0) {
        return 0;
    }

    old_size  = old_mcb->size;
    blk_owner = (old_mcb->owner != seg) ? old_mcb->owner : owner;

    new_seg = mem_alloc(new_paragraphs, blk_owner);
    if (new_seg == 0) {
        return 0;
    }

    /* Copy existing payload paragraphs (8 words per paragraph) */
    {
        uint16_t copy_paras = (old_size < new_paragraphs) ? old_size : new_paragraphs;
        for (p = 0; p < copy_paras; p++) {
            const uint16_t __far *src = (const uint16_t __far *)MK_FP(seg + p, 0);
            uint16_t __far *dst       = (uint16_t __far *)MK_FP(new_seg + p, 0);
            uint8_t w;
            for (w = 0; w < 8; w++) {
                dst[w] = src[w];
            }
        }
    }

    mem_free(seg);
    return new_seg;
}

void mem_free_by_owner(uint16_t owner) {
    uint16_t cur_seg = MEM_START_SEG;

    if (owner == 0) {
        return;
    }

    while (cur_seg < MEM_END_SEG) {
        mcb_t __far *cur = (mcb_t __far *)MK_FP(cur_seg, 0);
        if (cur->type != MCB_TYPE_CONT && cur->type != MCB_TYPE_LAST) {
            break;
        }
        if (cur->owner == owner) {
            cur->owner = 0x0000;
        }
        if (cur->type == MCB_TYPE_LAST) {
            break;
        }
        cur_seg = cur_seg + 1 + cur->size;
    }

    mem_coalesce();
}

uint16_t mem_avail_paragraphs(void) {
    uint16_t cur_seg = MEM_START_SEG;
    uint16_t total   = 0;

    while (cur_seg < MEM_END_SEG) {
        mcb_t __far *cur = (mcb_t __far *)MK_FP(cur_seg, 0);
        if (cur->type != MCB_TYPE_CONT && cur->type != MCB_TYPE_LAST) {
            break;
        }
        if (cur->owner == 0) {
            total += cur->size;
        }
        if (cur->type == MCB_TYPE_LAST) {
            break;
        }
        cur_seg = cur_seg + 1 + cur->size;
    }

    return total;
}

uint16_t mem_max_avail_paragraphs(void) {
    uint16_t cur_seg = MEM_START_SEG;
    uint16_t largest = 0;

    while (cur_seg < MEM_END_SEG) {
        mcb_t __far *cur = (mcb_t __far *)MK_FP(cur_seg, 0);
        if (cur->type != MCB_TYPE_CONT && cur->type != MCB_TYPE_LAST) {
            break;
        }
        if (cur->owner == 0 && cur->size > largest) {
            largest = cur->size;
        }
        if (cur->type == MCB_TYPE_LAST) {
            break;
        }
        cur_seg = cur_seg + 1 + cur->size;
    }

    return largest;
}
