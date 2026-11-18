/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CHKDSK.BIN — Filesystem integrity verification and repair utility for hobbyOS.
 */

#include "hobbyos.h"

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];
#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))
#define MK_FP(seg, off) ((void __far *)(((unsigned long)(seg) << 16) | (uint16_t)(off)))

/* Inline hardware 16-bit division pragmas for 8086 */
uint16_t div_dx_ax(uint16_t hi, uint16_t lo, uint16_t den);
#pragma aux div_dx_ax = \
    "div bx" \
    parm [dx] [ax] [bx] value [ax];

uint16_t rem_dx_ax(uint16_t hi, uint16_t lo, uint16_t den);
#pragma aux rem_dx_ax = \
    "div bx" \
    parm [dx] [ax] [bx] value [dx];

static uint32_t u32_div_u16(uint32_t num, uint16_t den, uint16_t *rem_out) {
    uint16_t hi = (uint16_t)(num >> 16);
    uint16_t lo = (uint16_t)(num & 0xFFFF);
    uint16_t q_hi = hi / den;
    uint16_t r_hi = hi % den;
    uint16_t q_lo = div_dx_ax(r_hi, lo, den);
    if (rem_out) {
        *rem_out = rem_dx_ax(r_hi, lo, den);
    }
    return ((uint32_t)q_hi << 16) | q_lo;
}

static uint16_t u32_div10(uint32_t *val) {
    uint16_t rem;
    *val = u32_div_u16(*val, 10, &rem);
    return rem;
}

static uint32_t u32_mul10(uint32_t val) {
    return (val << 3) + (val << 1);
}

static uint32_t u32_mul_u16(uint32_t val, uint16_t mult) {
    uint32_t res = 0;
    while (mult > 0) {
        if (mult & 1) {
            res += val;
        }
        val <<= 1;
        mult >>= 1;
    }
    return res;
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

static void print_uint32_padded(uint32_t val, int width) {
    char buf[12];
    int i = 0;
    int pad;
    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0 && i < 11) {
            buf[i++] = (char)('0' + u32_div10(&val));
        }
    }
    pad = width - i;
    while (pad-- > 0) {
        os_putc(' ');
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

static void print_hex16(uint16_t val) {
    static const char hex[] = "0123456789ABCDEF";
    os_putc(hex[(val >> 12) & 0xF]);
    os_putc(hex[(val >> 8) & 0xF]);
    os_putc(hex[(val >> 4) & 0xF]);
    os_putc(hex[val & 0xF]);
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static int str_len(const char *s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

/* Volume BPB and geometry state */
static uint16_t bytes_per_sector;
static uint8_t  sectors_per_cluster;
static uint16_t reserved_sectors;
static uint8_t  num_fats;
static uint16_t root_entries;
static uint32_t total_sectors;
static uint16_t fat_sz16;
static uint32_t vol_id;
static char     vol_label[12];

static uint32_t fat1_lba;
static uint32_t fat2_lba;
static uint32_t root_dir_lba;
static uint32_t root_dir_sectors;
static uint32_t data_lba;
static uint16_t total_clusters;
static uint8_t  fat_type; /* 12 or 16 */
static uint16_t max_cluster;
static uint16_t cluster_bytes;

/* Dynamic FAT Sector Cache */
static uint32_t cur_fat_sec = 0xFFFFFFFFUL;
static int      cur_fat_dirty = 0;
static uint8_t  fat_sec_buf[512];
static uint8_t  next_fat_buf[512];

static void fat_flush_cache(const char *vol) {
    if (cur_fat_dirty && cur_fat_sec != 0xFFFFFFFFUL) {
        os_vol_write_sector(vol, cur_fat_sec, (const void __far *)FP_HERE(fat_sec_buf));
        if (num_fats >= 2) {
            os_vol_write_sector(vol, cur_fat_sec + fat_sz16, (const void __far *)FP_HERE(fat_sec_buf));
        }
        cur_fat_dirty = 0;
    }
}

static uint16_t get_fat_entry(const char *vol, uint16_t cluster) {
    if (fat_type == 16) {
        uint32_t sec = fat1_lba + (cluster >> 8);
        uint16_t off = (cluster & 0xFF) << 1;
        if (sec != cur_fat_sec) {
            fat_flush_cache(vol);
            if (os_vol_read_sector(vol, sec, (void __far *)FP_HERE(fat_sec_buf)) != 0) {
                return 0xFFFF;
            }
            cur_fat_sec = sec;
        }
        return *(uint16_t *)&fat_sec_buf[off];
    } else {
        /* FAT12 */
        uint16_t byte_off = cluster + (cluster >> 1);
        uint32_t sec = fat1_lba + (byte_off >> 9);
        uint16_t off = byte_off & 0x1FF;
        uint16_t val;
        if (sec != cur_fat_sec) {
            fat_flush_cache(vol);
            if (os_vol_read_sector(vol, sec, (void __far *)FP_HERE(fat_sec_buf)) != 0) {
                return 0x0FFF;
            }
            cur_fat_sec = sec;
        }
        if (off == 511) {
            uint8_t low_b = fat_sec_buf[511];
            if (os_vol_read_sector(vol, sec + 1, (void __far *)FP_HERE(next_fat_buf)) != 0) {
                return 0x0FFF;
            }
            val = (uint16_t)low_b | ((uint16_t)next_fat_buf[0] << 8);
        } else {
            val = *(uint16_t *)&fat_sec_buf[off];
        }
        if (cluster & 1) {
            return val >> 4;
        } else {
            return val & 0x0FFF;
        }
    }
}

static void set_fat_entry(const char *vol, uint16_t cluster, uint16_t val) {
    if (fat_type == 16) {
        uint32_t sec = fat1_lba + (cluster >> 8);
        uint16_t off = (cluster & 0xFF) << 1;
        if (sec != cur_fat_sec) {
            fat_flush_cache(vol);
            os_vol_read_sector(vol, sec, (void __far *)FP_HERE(fat_sec_buf));
            cur_fat_sec = sec;
        }
        *(uint16_t *)&fat_sec_buf[off] = val;
        cur_fat_dirty = 1;
    } else {
        /* FAT12 */
        uint16_t byte_off = cluster + (cluster >> 1);
        uint32_t sec = fat1_lba + (byte_off >> 9);
        uint16_t off = byte_off & 0x1FF;
        if (sec != cur_fat_sec) {
            fat_flush_cache(vol);
            os_vol_read_sector(vol, sec, (void __far *)FP_HERE(fat_sec_buf));
            cur_fat_sec = sec;
        }
        if (off == 511) {
            os_vol_read_sector(vol, sec + 1, (void __far *)FP_HERE(next_fat_buf));
            if (cluster & 1) {
                fat_sec_buf[511] = (uint8_t)((fat_sec_buf[511] & 0x0F) | ((val & 0x0F) << 4));
                next_fat_buf[0]  = (uint8_t)(val >> 4);
            } else {
                fat_sec_buf[511] = (uint8_t)(val & 0xFF);
                next_fat_buf[0]  = (uint8_t)((next_fat_buf[0] & 0xF0) | ((val >> 8) & 0x0F));
            }
            cur_fat_dirty = 1;
            fat_flush_cache(vol);
            os_vol_write_sector(vol, sec + 1, (const void __far *)FP_HERE(next_fat_buf));
            if (num_fats >= 2) {
                os_vol_write_sector(vol, sec + 1 + fat_sz16, (const void __far *)FP_HERE(next_fat_buf));
            }
        } else {
            uint16_t old = *(uint16_t *)&fat_sec_buf[off];
            if (cluster & 1) {
                old = (old & 0x000F) | (val << 4);
            } else {
                old = (old & 0xF000) | (val & 0x0FFF);
            }
            *(uint16_t *)&fat_sec_buf[off] = old;
            cur_fat_dirty = 1;
        }
    }
}

/* Cluster Bitmap State */
static uint8_t __far *bitmap_ptr = 0;

static void bitmap_set(uint16_t cluster) {
    if (bitmap_ptr && cluster < 65535U) {
        bitmap_ptr[cluster >> 3] |= (uint8_t)(1 << (cluster & 7));
    }
}

static int bitmap_test(uint16_t cluster) {
    if (!bitmap_ptr || cluster >= 65535U) return 0;
    return (bitmap_ptr[cluster >> 3] & (1 << (cluster & 7))) != 0;
}

/* Statistics */
static uint32_t num_files = 0;
static uint32_t bytes_files = 0;
static uint32_t num_hidden = 0;
static uint32_t bytes_hidden = 0;
static uint32_t num_dirs = 0;
static uint32_t bytes_dirs = 0;
static uint32_t num_bad_clusters = 0;
static uint32_t num_free_clusters = 0;
static uint32_t num_lost_clusters = 0;
static uint32_t num_lost_chains = 0;
static uint32_t num_cross_linked = 0;
static uint32_t num_alloc_errors = 0;

/* Directory Processing Queue */
#define DIR_QUEUE_MAX 128
static uint16_t dir_queue[DIR_QUEUE_MAX];
static uint16_t dir_queue_head = 0;
static uint16_t dir_queue_tail = 0;

static void dir_queue_push(uint16_t clus) {
    if ((uint16_t)((dir_queue_tail + 1) % DIR_QUEUE_MAX) != dir_queue_head) {
        dir_queue[dir_queue_tail] = clus;
        dir_queue_tail = (dir_queue_tail + 1) % DIR_QUEUE_MAX;
    }
}

static uint16_t dir_queue_pop(void) {
    uint16_t clus;
    if (dir_queue_head == dir_queue_tail) return 0;
    clus = dir_queue[dir_queue_head];
    dir_queue_head = (dir_queue_head + 1) % DIR_QUEUE_MAX;
    return clus;
}

static void print_entry_name(const uint8_t *entry) {
    int i;
    for (i = 0; i < 8 && entry[i] != ' '; i++) {
        os_putc((char)entry[i]);
    }
    if (entry[8] != ' ') {
        os_putc('.');
        for (i = 8; i < 11 && entry[i] != ' '; i++) {
            os_putc((char)entry[i]);
        }
    }
}

static void process_dir_sector(const char *vol, const uint8_t *buf, int verbose) {
    int e;
    for (e = 0; e < 16; e++) {
        const uint8_t *ent = &buf[e * 32];
        uint8_t attr;
        uint16_t start_clus;
        uint32_t fsize;

        if (ent[0] == 0x00) {
            /* End of directory */
            return;
        }
        if (ent[0] == 0xE5) {
            /* Deleted entry */
            continue;
        }
        attr = ent[11];
        if (attr == 0x0F) {
            /* Long filename entry: skip */
            continue;
        }
        if (attr & 0x08) {
            /* Volume label */
            continue;
        }

        start_clus = *(const uint16_t *)&ent[26];
        fsize      = *(const uint32_t *)&ent[28];

        if (attr & 0x10) {
            /* Subdirectory */
            if (ent[0] == '.') {
                /* Skip "." and ".." */
                continue;
            }
            num_dirs++;
            bytes_dirs += cluster_bytes;
            if (verbose) {
                os_puts("Directory: ");
                print_entry_name(ent);
                os_puts("\n");
            }
            if (start_clus >= 2 && start_clus <= max_cluster) {
                uint16_t clus = start_clus;
                uint16_t eof_val = (fat_type == 12) ? 0x0FF8 : 0xFFF8;
                uint32_t hops = 0;

                dir_queue_push(start_clus);

                while (clus >= 2 && clus <= max_cluster) {
                    hops++;
                    if (bitmap_test(clus)) {
                        num_cross_linked++;
                        if (verbose) {
                            os_puts("  Cross-linked cluster on directory: ");
                            print_uint16(clus);
                            os_puts("\n");
                        }
                    }
                    bitmap_set(clus);
                    clus = get_fat_entry(vol, clus);
                    if (hops > total_clusters) {
                        num_alloc_errors++;
                        os_puts("Error: Circular cluster chain on directory.\n");
                        break;
                    }
                }
            } else if (start_clus != 0) {
                num_alloc_errors++;
                os_puts("Error: Invalid starting cluster on directory.\n");
            }
        } else {
            /* Regular File */
            int is_hid = (attr & 0x06) != 0;
            if (is_hid) {
                num_hidden++;
                bytes_hidden += fsize;
            } else {
                num_files++;
                bytes_files += fsize;
            }

            if (verbose) {
                os_puts(is_hid ? "Hidden file: " : "File: ");
                print_entry_name(ent);
                os_puts(" (");
                print_uint32(fsize);
                os_puts(" bytes)\n");
            }

            if (fsize == 0) {
                if (start_clus != 0) {
                    num_alloc_errors++;
                }
            } else {
                if (start_clus >= 2 && start_clus <= max_cluster) {
                    uint16_t clus = start_clus;
                    uint16_t eof_val = (fat_type == 12) ? 0x0FF8 : 0xFFF8;
                    uint32_t hops = 0;

                    while (clus >= 2 && clus <= max_cluster) {
                        hops++;
                        if (bitmap_test(clus)) {
                            num_cross_linked++;
                            if (verbose) {
                                os_puts("  Cross-linked cluster ");
                                print_uint16(clus);
                                os_puts("\n");
                            }
                        }
                        bitmap_set(clus);
                        clus = get_fat_entry(vol, clus);
                        if (hops > total_clusters) {
                            num_alloc_errors++;
                            os_puts("Error: Circular cluster chain on file.\n");
                            break;
                        }
                    }
                    if (clus != 0 && clus < eof_val && clus != 0xFFF7 && clus != 0x0FF7) {
                        num_alloc_errors++;
                    }
                } else {
                    num_alloc_errors++;
                    os_puts("Error: Invalid starting cluster on file.\n");
                }
            }
        }
    }
}

int app_main(void) {
    const char *args = os_get_args();
    char target_vol[16];
    int fix_flag = 0;
    int verbose_flag = 0;
    int i = 0, p = 0;
    uint8_t sec_buf[512];
    uint8_t sec_buf2[512];
    uint16_t bitmap_seg = 0;
    uint32_t s;
    uint16_t fat_mismatch_count = 0;
    uint16_t c;
    int in_lost_chain = 0;
    uint32_t total_disk_bytes;
    uint32_t avail_disk_bytes;
    uint16_t avail_para;
    uint32_t avail_mem;

    target_vol[0] = '\0';

    /* Parse Arguments */
    while (args && args[i] == ' ') i++;

    while (args && args[i] != '\0') {
        if (args[i] == '/') {
            i++;
            if (args[i] == 'F' || args[i] == 'f') {
                fix_flag = 1;
                i++;
            } else if (args[i] == 'V' || args[i] == 'v') {
                verbose_flag = 1;
                i++;
            } else if (args[i] == '?' || args[i] == 'H' || args[i] == 'h') {
                os_puts("hobbyOS Disk Check & Repair Utility\n");
                os_puts("Syntax: CHKDSK [volume:] [/F] [/V]\n\n");
                os_puts("  volume:  Specifies drive to check (e.g. HD0A:, SYS:, FD0:)\n");
                os_puts("  /F       Fixes errors on the disk (frees lost clusters)\n");
                os_puts("  /V       Displays the full path and name of every file\n");
                return 0;
            } else {
                while (args[i] != '\0' && args[i] != ' ') i++;
            }
        } else {
            p = 0;
            while (args[i] != '\0' && args[i] != ' ' && p < sizeof(target_vol) - 2) {
                target_vol[p++] = to_upper(args[i++]);
            }
            target_vol[p] = '\0';
        }
        while (args[i] == ' ') i++;
    }

    if (target_vol[0] != '\0') {
        int len = str_len(target_vol);
        if (len > 0 && target_vol[len - 1] != ':') {
            target_vol[len++] = ':';
            target_vol[len]   = '\0';
        }
    }

    /* 1. Read Sector 0 (VBR / BPB) */
    if (os_vol_read_sector(target_vol, 0, (void __far *)FP_HERE(sec_buf)) != 0) {
        os_puts("Error: Unable to read boot sector from volume ");
        os_puts(target_vol[0] ? target_vol : "current");
        os_puts("\n");
        return 1;
    }

    if (sec_buf[510] != 0x55 || sec_buf[511] != 0xAA) {
        os_puts("Error: Invalid boot sector signature on volume.\n");
        return 1;
    }

    bytes_per_sector     = *(uint16_t *)&sec_buf[11];
    sectors_per_cluster  = sec_buf[13];
    reserved_sectors     = *(uint16_t *)&sec_buf[14];
    num_fats             = sec_buf[16];
    root_entries         = *(uint16_t *)&sec_buf[17];
    fat_sz16             = *(uint16_t *)&sec_buf[22];
    vol_id               = *(uint32_t *)&sec_buf[39];

    total_sectors = *(uint16_t *)&sec_buf[19];
    if (total_sectors == 0) {
        total_sectors = *(uint32_t *)&sec_buf[32];
    }

    for (i = 0; i < 11; i++) {
        vol_label[i] = (char)sec_buf[43 + i];
    }
    vol_label[11] = '\0';

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || reserved_sectors == 0 ||
        num_fats == 0 || fat_sz16 == 0 || total_sectors == 0) {
        os_puts("Error: Corrupted or invalid BPB parameters on volume.\n");
        return 1;
    }

    /* Calculate layout */
    fat1_lba         = reserved_sectors;
    fat2_lba         = fat1_lba + fat_sz16;
    root_dir_lba     = fat1_lba + u32_mul_u16((uint32_t)fat_sz16, num_fats);
    root_dir_sectors = (((uint32_t)root_entries << 5) + 511) >> 9;
    data_lba         = root_dir_lba + root_dir_sectors;
    total_clusters   = (uint16_t)u32_div_u16(total_sectors - data_lba, sectors_per_cluster, 0);
    fat_type         = (total_clusters < 4085) ? 12 : 16;
    max_cluster      = total_clusters + 1;
    cluster_bytes    = (uint16_t)sectors_per_cluster << 9;

    /* Display Volume Header */
    os_puts("Volume ");
    for (i = 0; i < 11 && vol_label[i] != ' '; i++) {
        os_putc(vol_label[i]);
    }
    if (i == 0) os_puts("has no label");
    os_puts("\nVolume Serial Number is ");
    print_hex16((uint16_t)(vol_id >> 16));
    os_putc('-');
    print_hex16((uint16_t)(vol_id & 0xFFFF));
    os_puts("\n\n");

    /* 2. Compare FAT1 and FAT2 */
    if (num_fats >= 2) {
        for (s = 0; s < fat_sz16; s++) {
            if (os_vol_read_sector(target_vol, fat1_lba + s, (void __far *)FP_HERE(sec_buf)) == 0 &&
                os_vol_read_sector(target_vol, fat2_lba + s, (void __far *)FP_HERE(sec_buf2)) == 0) {
                int diff = 0;
                int b;
                for (b = 0; b < 512; b++) {
                    if (sec_buf[b] != sec_buf2[b]) {
                        diff = 1;
                        break;
                    }
                }
                if (diff) {
                    fat_mismatch_count++;
                    if (fix_flag) {
                        os_vol_write_sector(target_vol, fat2_lba + s, (const void __far *)FP_HERE(sec_buf));
                    }
                }
            }
        }
        if (fat_mismatch_count > 0) {
            if (fix_flag) {
                os_puts("Synchronized secondary File Allocation Table.\n");
            } else {
                os_puts("Warning: Copies of File Allocation Table do not match.\n");
            }
        }
    }

    /* 3. Allocate Cluster Bitmap (8192 bytes = 512 paragraphs for 65536 clusters) */
    bitmap_seg = os_mem_alloc(512);
    if (bitmap_seg == 0) {
        os_puts("Error: Insufficient conventional memory for cluster bitmap.\n");
        return 1;
    }
    bitmap_ptr = (uint8_t __far *)MK_FP(bitmap_seg, 0);
    for (i = 0; i < 8192; i++) {
        bitmap_ptr[i] = 0;
    }

    /* 4. Traverse Root Directory */
    for (s = 0; s < root_dir_sectors; s++) {
        if (os_vol_read_sector(target_vol, root_dir_lba + s, (void __far *)FP_HERE(sec_buf)) != 0) {
            os_puts("Error reading root directory sector.\n");
            break;
        }
        process_dir_sector(target_vol, sec_buf, verbose_flag);
    }

    /* 5. Traverse Subdirectories in Queue */
    while (dir_queue_head != dir_queue_tail) {
        uint16_t dir_clus = dir_queue_pop();
        uint16_t cur_c = dir_clus;
        uint32_t depth = 0;

        while (cur_c >= 2 && cur_c <= max_cluster) {
            uint32_t clus_lba = data_lba + u32_mul_u16((uint32_t)(cur_c - 2), sectors_per_cluster);
            uint8_t sec_i;
            for (sec_i = 0; sec_i < sectors_per_cluster; sec_i++) {
                if (os_vol_read_sector(target_vol, clus_lba + sec_i, (void __far *)FP_HERE(sec_buf)) != 0) {
                    break;
                }
                process_dir_sector(target_vol, sec_buf, verbose_flag);
            }
            cur_c = get_fat_entry(target_vol, cur_c);
            if (++depth > total_clusters) break;
        }
    }

    /* 6. Scan FAT for Free, Bad, and Lost Clusters */
    for (c = 2; c <= max_cluster; c++) {
        uint16_t val = get_fat_entry(target_vol, c);
        if (val == 0x0000) {
            num_free_clusters++;
            in_lost_chain = 0;
        } else if (val == 0xFFF7 || (fat_type == 12 && val == 0x0FF7)) {
            num_bad_clusters++;
            in_lost_chain = 0;
        } else {
            /* Cluster is allocated in FAT */
            if (!bitmap_test(c)) {
                /* Not referenced by any directory! Lost cluster! */
                num_lost_clusters++;
                if (!in_lost_chain) {
                    num_lost_chains++;
                    in_lost_chain = 1;
                }
            } else {
                in_lost_chain = 0;
            }
        }
    }

    /* 7. Handle Lost Clusters */
    if (num_lost_clusters > 0) {
        uint32_t lost_bytes = u32_mul_u16((uint32_t)num_lost_clusters, cluster_bytes);
        os_puts("\n");
        print_uint32_padded(lost_bytes, 12);
        os_puts(" bytes in ");
        print_uint32(num_lost_clusters);
        os_puts(" lost allocation units\n\n");
        print_uint32(num_lost_clusters);
        os_puts(" lost clusters found in ");
        print_uint32(num_lost_chains);
        os_puts(" chains.\n");

        if (fix_flag) {
            char ans;
            os_puts("Fix lost clusters (Y/N)? ");
            ans = (char)os_getc();
            os_putc(ans);
            os_puts("\n");
            if (ans == 'Y' || ans == 'y') {
                for (c = 2; c <= max_cluster; c++) {
                    if (!bitmap_test(c)) {
                        uint16_t v = get_fat_entry(target_vol, c);
                        if (v != 0 && v != 0xFFF7 && !(fat_type == 12 && v == 0x0FF7)) {
                            set_fat_entry(target_vol, c, 0);
                        }
                    }
                }
                fat_flush_cache(target_vol);
                num_free_clusters += num_lost_clusters;
                num_lost_clusters = 0;
                os_puts("Lost clusters freed.\n\n");
            }
        } else {
            os_puts("Errors found. Run CHKDSK /F to fix.\n\n");
        }
    }

    /* 8. Print Summary Statistics */
    total_disk_bytes = u32_mul_u16(total_sectors, bytes_per_sector);
    avail_disk_bytes = u32_mul_u16((uint32_t)num_free_clusters, cluster_bytes);

    print_uint32_padded(total_disk_bytes, 12);
    os_puts(" bytes total disk space\n");

    if (num_hidden > 0) {
        print_uint32_padded(bytes_hidden, 12);
        os_puts(" bytes in ");
        print_uint32(num_hidden);
        os_puts(" hidden files\n");
    }

    if (num_dirs > 0) {
        print_uint32_padded(bytes_dirs, 12);
        os_puts(" bytes in ");
        print_uint32(num_dirs);
        os_puts(" directories\n");
    }

    print_uint32_padded(bytes_files, 12);
    os_puts(" bytes in ");
    print_uint32(num_files);
    os_puts(" user files\n");

    if (num_bad_clusters > 0) {
        uint32_t bad_bytes = u32_mul_u16((uint32_t)num_bad_clusters, cluster_bytes);
        print_uint32_padded(bad_bytes, 12);
        os_puts(" bytes in bad sectors\n");
    }

    print_uint32_padded(avail_disk_bytes, 12);
    os_puts(" bytes available on disk\n\n");

    print_uint32_padded((uint32_t)cluster_bytes, 12);
    os_puts(" bytes in each allocation unit\n");

    print_uint32_padded((uint32_t)total_clusters, 12);
    os_puts(" total allocation units on disk\n");

    print_uint32_padded((uint32_t)num_free_clusters, 12);
    os_puts(" available allocation units on disk\n\n");

    /* Memory reporting (matching classic MS-DOS CHKDSK) */
    avail_para = os_mem_avail();
    avail_mem  = (uint32_t)avail_para << 4;

    print_uint32_padded(655360UL, 12);
    os_puts(" total bytes memory\n");

    print_uint32_padded(avail_mem, 12);
    os_puts(" bytes free\n");

    /* Cleanup */
    fat_flush_cache(target_vol);
    if (bitmap_seg) {
        os_mem_free(bitmap_seg);
    }

    return 0;
}
