/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * AmigaDOS-style storage volume manager and FAT16 hierarchical file system
 * driver (dynamic BPB/MBR parsing, device paths DEV:PATH, subdirectories, CWD,
 * read, write, append, create, delete, mkdir, rmdir, chdir, copy, rename, and
 * executable loading).
 */

#include "kernel.h"
#include "bootcode.h"
#include "mbrcode.h"

#pragma pack(push, 1)
typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat16_dirent_t;

typedef struct {
    uint8_t  boot_indicator;
    uint8_t  start_head;
    uint8_t  start_sector;
    uint8_t  start_cylinder;
    uint8_t  part_type;
    uint8_t  end_head;
    uint8_t  end_sector;
    uint8_t  end_cylinder;
    uint32_t start_lba;
    uint32_t total_sectors;
} mbr_partition_t;
#pragma pack(pop)

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20

static os_volume_t volumes[MAX_VOLUMES];
static uint8_t     num_volumes     = 0;
static uint8_t     current_vol_idx = 0;
static uint8_t     fs_mounted      = 0;

static uint8_t     cached_fat_vol  = 0xFF;
static uint16_t    cached_fat_sec  = 0xFFFF;
static uint8_t     fat_dirty       = 0;

static uint8_t     io_buf[512]     = {0};
static uint8_t     fat_buf[512]    = {0};

/* Forward declarations */
static os_volume_t *resolve_vol_alias(os_volume_t *vol);
static int fat_flush(void);
static int fat_load_sector(os_volume_t *vol, uint16_t fat_sec_idx);
static uint16_t fat_get_cluster(os_volume_t *vol, uint16_t cluster);
static int fat_set_cluster(os_volume_t *vol, uint16_t cluster, uint16_t val);
static int dir_get_sector_lba(os_volume_t *vol, uint16_t dir_cluster, uint16_t sec_idx, uint32_t *out_lba);
static int parse_bpb(os_volume_t *vol, const uint8_t *buf);
static int vol_ensure_mounted(os_volume_t *vol);

static int is_eof_cluster(os_volume_t *vol, uint16_t cluster) {
    if (vol && vol->fat_type == FAT_TYPE_FAT12) {
        return (cluster >= 0x0FF8);
    }
    return (cluster >= 0xFFF8);
}

static uint16_t eof_cluster_marker(os_volume_t *vol) {
    if (vol && vol->fat_type == FAT_TYPE_FAT12) {
        return 0x0FFF;
    }
    return 0xFFFF;
}

/* --- Low-Level Disk I/O Helpers --- */

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int vol_unmount(os_volume_t *vol) {
    uint8_t v_idx;
    vol = resolve_vol_alias(vol);
    if (!vol) {
        return -1;
    }
    v_idx = (uint8_t)(vol - volumes);
    if (cached_fat_vol == v_idx) {
        fat_flush();
        cached_fat_sec = 0xFFFF;
        cached_fat_vol = 0xFF;
    }
    if (vol->is_removable) {
        vol->is_mounted = 0;
        vol->cwd_cluster = 0;
        vol->cwd_path[0] = '/';
        vol->cwd_path[1] = '\0';
    }
    return 0;
}

int vol_mount(os_volume_t *vol) {
    int retry;
    int rc = -1;
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol->is_mounted) {
        return 0;
    }
    if (!vol->is_removable) {
        vol->is_mounted = 1;
        return 0;
    }

    /* Try reading Sector 0 (VBR) with up to 3 retries (motor spin-up) */
    for (retry = 0; retry < 3; retry++) {
        rc = bios_disk_rw_lba(vol->bios_drive, 0, 0, vol->sectors_per_track, vol->num_heads, MK_FP(KERNEL_SEG, io_buf));
        if (rc == 0) {
            break;
        }
        bios_disk_reset(vol->bios_drive);
    }
    if (retry == 3) {
        return -2; /* Drive not ready / no media */
    }

    /* Check VBR signature */
    if (io_buf[510] != 0x55 || io_buf[511] != 0xAA) {
        return -1;
    }

    /* Parse BPB */
    if (parse_bpb(vol, io_buf) != 0) {
        return -1;
    }

    vol->is_mounted = 1;
    return 0;
}

static int vol_ensure_mounted(os_volume_t *vol) {
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol->is_removable && !vol->is_mounted) {
        int rc = vol_mount(vol);
        if (rc != 0) {
            return (rc == -2) ? -3 : -1;
        }
    }
    return 0;
}

int vol_diskchange(const char __far *dev_name) {
    if (dev_name && dev_name[0] != '\0') {
        os_volume_t *vol = 0;
        const char __far *sub = 0;
        if (vol_resolve_path(dev_name, &vol, &sub) == 0 && vol) {
            return vol_unmount(vol);
        }
        return -1;
    } else {
        uint8_t v;
        for (v = 0; v < num_volumes; v++) {
            if (volumes[v].active && volumes[v].is_removable) {
                vol_unmount(&volumes[v]);
            }
        }
        return 0;
    }
}

typedef struct {
    uint8_t  active;
    uint8_t  type;
    uint16_t size_kb;
    uint16_t total_sectors;
    uint8_t  ems_pages[128];
    uint16_t ems_num_pages;
    uint16_t xms_handle;
} ramdisk_info_t;

static ramdisk_info_t ramdisk_info = {0};

static int ramdisk_rw(uint8_t is_write, uint32_t rel_lba, void __far *buf) {
    if (!ramdisk_info.active || rel_lba >= (uint32_t)ramdisk_info.total_sectors) {
        return -1;
    }
    if (ramdisk_info.type == RAMDISK_TYPE_EMS) {
        uint16_t lba16 = (uint16_t)rel_lba;
        uint16_t page_idx = lba16 >> 5;          /* 32 sectors per 16KB page */
        uint16_t sec_in_page = lba16 & 31;
        uint16_t sec_offset = sec_in_page << 9;  /* 512 bytes per sector */

        if (page_idx >= ramdisk_info.ems_num_pages) {
            return -1;
        }
        return ems_rw_sector(ramdisk_info.ems_pages[page_idx], sec_offset, is_write, buf);
    } else if (ramdisk_info.type == RAMDISK_TYPE_XMS) {
        uint16_t lba16 = (uint16_t)rel_lba;
        uint32_t byte_offset = ((uint32_t)(lba16 >> 7) << 16) | (uint16_t)(lba16 << 9);
        return xms_rw_sector(ramdisk_info.xms_handle, byte_offset, is_write, buf);
    }
    return -1;
}

static int vol_rw_lba(os_volume_t *vol, uint8_t is_write, uint32_t rel_lba, void __far *buf) {
    int res;
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol->bios_drive == 0xFF) {
        return ramdisk_rw(is_write, rel_lba, buf);
    }
    res = bios_disk_rw_lba(vol->bios_drive, is_write, vol->start_lba + rel_lba,
                            vol->sectors_per_track, vol->num_heads, buf);
    if (res != 0 && vol->is_removable) {
        /* I/O error on removable drive: unmount to prevent corrupt operations */
        vol_unmount(vol);
    }
    return res;
}

/* --- Volume Management Functions --- */

int vol_get_count(void) {
    return (int)num_volumes;
}

os_volume_t *vol_get(uint8_t idx) {
    if (idx >= MAX_VOLUMES || !volumes[idx].active) {
        return 0;
    }
    return &volumes[idx];
}

os_volume_t *vol_get_current(void) {
    if (current_vol_idx >= MAX_VOLUMES || !volumes[current_vol_idx].active) {
        return 0;
    }
    return &volumes[current_vol_idx];
}

int vol_set_current(uint8_t idx) {
    if (idx >= MAX_VOLUMES || !volumes[idx].active) {
        return -1;
    }
    if (vol_ensure_mounted(&volumes[idx]) != 0) {
        return -3;
    }
    current_vol_idx = idx;
    return 0;
}

static os_volume_t *resolve_vol_alias(os_volume_t *vol) {
    int hops = 0;
    while (vol && vol->active && vol->is_alias && hops < 4) {
        if (vol->target_vol_idx >= MAX_VOLUMES) {
            break;
        }
        vol = &volumes[vol->target_vol_idx];
        hops++;
    }
    return vol;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

static int is_sep(char c) {
    return (c == '/' || c == '\\');
}

/* Compare two volume names ignoring case */
static int vol_name_match(const char *name_in_vol, const char *name_query) {
    int i = 0;
    while (name_in_vol[i] != '\0' && name_query[i] != '\0') {
        if (to_upper(name_in_vol[i]) != to_upper(name_query[i])) {
            return 0;
        }
        i++;
    }
    return (name_in_vol[i] == '\0' && name_query[i] == '\0');
}

/*
 * Resolve a path into its target volume and subpath.
 * Supports:
 *   "SYS:BIN/EDIT.BIN"   -> vol = SYS,   subpath = "/BIN/EDIT.BIN"
 *   "HD0A:DOCS/TEST"     -> vol = HD0A,  subpath = "/DOCS/TEST"
 *   "HD0A:"              -> vol = HD0A,  subpath = ""
 *   "/BIN/EDIT.BIN"      -> vol = current, subpath = "/BIN/EDIT.BIN"
 *   "EDIT.BIN"           -> vol = current, subpath = "EDIT.BIN"
 */
int vol_resolve_path(const char __far *path, os_volume_t **out_vol, const char __far **out_subpath) {
    uint16_t colon_pos = 0xFFFF;
    uint16_t i;
    char dev_name[8];
    uint8_t v;

    if (!fs_mounted || !out_vol || !out_subpath) {
        return -1;
    }
    if (!path || path[0] == '\0') {
        *out_vol     = vol_get_current();
        *out_subpath = "";
        return (*out_vol != 0) ? 0 : -1;
    }

    /* Check for colon ':' in the path */
    for (i = 0; i < 16 && path[i] != '\0'; i++) {
        if (path[i] == ':') {
            colon_pos = i;
            break;
        }
    }

    if (colon_pos != 0xFFFF) {
        if (colon_pos >= sizeof(dev_name)) {
            return -1;
        }
        for (i = 0; i < colon_pos; i++) {
            dev_name[i] = path[i];
        }
        dev_name[colon_pos] = '\0';

        /* Look up volume name */
        for (v = 0; v < MAX_VOLUMES; v++) {
            if (volumes[v].active && vol_name_match(volumes[v].name, dev_name)) {
                *out_vol     = &volumes[v];
                *out_subpath = path + colon_pos + 1;
                return 0;
            }
        }
        return -1; /* Unknown device */
    }

    /* No colon: use active volume */
    *out_vol     = vol_get_current();
    *out_subpath = path;
    return (*out_vol != 0) ? 0 : -1;
}

int vol_read_sector(const char __far *vol_name, uint32_t lba, void __far *buf) {
    os_volume_t *vol = 0;
    const char __far *subpath = 0;
    int rc;
    if (!buf) return -1;
    if (vol_resolve_path(vol_name, &vol, &subpath) != 0 || !vol) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) return -1;
    if (vol->is_removable && !vol->is_mounted) {
        vol_ensure_mounted(vol);
    }
    /* Bounce through KERNEL_SEG io_buf to guarantee no 64K DMA boundary violations */
    rc = vol_rw_lba(vol, 0, lba, MK_FP(KERNEL_SEG, io_buf));
    if (rc == 0) {
        uint8_t __far *dst = (uint8_t __far *)buf;
        uint16_t i;
        for (i = 0; i < 512; i++) {
            dst[i] = io_buf[i];
        }
    }
    return rc;
}

int vol_write_sector(const char __far *vol_name, uint32_t lba, const void __far *buf) {
    os_volume_t *vol = 0;
    const char __far *subpath = 0;
    const uint8_t __far *src = (const uint8_t __far *)buf;
    uint16_t i;
    if (!buf) return -1;
    if (vol_resolve_path(vol_name, &vol, &subpath) != 0 || !vol) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) return -1;
    if (vol->is_removable && !vol->is_mounted) {
        vol_ensure_mounted(vol);
    }
    fat_flush();
    cached_fat_vol = 0xFF;
    /* Bounce through KERNEL_SEG io_buf to guarantee no 64K DMA boundary violations */
    for (i = 0; i < 512; i++) {
        io_buf[i] = src[i];
    }
    return vol_rw_lba(vol, 1, lba, MK_FP(KERNEL_SEG, io_buf));
}

uint16_t vol_get_free_clusters(os_volume_t *vol) {
    uint16_t num_fat_secs;
    uint16_t sec;
    uint16_t free_count = 0;

    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active || !vol->is_mounted || vol->total_clusters == 0) {
        return 0;
    }

    if (vol->fat_type == FAT_TYPE_FAT12) {
        uint16_t c;
        for (c = 2; c < vol->total_clusters + 2; c++) {
            if (fat_get_cluster(vol, c) == 0x0000) {
                free_count++;
            }
        }
        return free_count;
    }

    num_fat_secs = (vol->total_clusters + 2 + 255) >> 8;
    for (sec = 0; sec < num_fat_secs; sec++) {
        uint16_t *entries;
        int i;
        int start_i = 0;
        int end_i = 256;

        if (fat_load_sector(vol, sec) != 0) {
            break;
        }
        entries = (uint16_t *)fat_buf;
        if (sec == 0) {
            start_i = 2;
        }
        if (sec == num_fat_secs - 1) {
            end_i = (vol->total_clusters + 2) & 0xFF;
            if (end_i == 0) end_i = 256;
        }
        for (i = start_i; i < end_i; i++) {
            if (entries[i] == 0x0000) {
                free_count++;
            }
        }
    }
    return free_count;
}

int vol_get_info(uint8_t idx, os_vol_info_t __far *out_info) {
    os_volume_t *vol;
    os_volume_t *target;
    int i;

    if (!out_info) {
        return -1;
    }

    if (idx == 0xFF) {
        vol = vol_get_current();
    } else if (idx < MAX_VOLUMES && volumes[idx].active) {
        vol = &volumes[idx];
    } else {
        return -1;
    }

    if (!vol || !vol->active) {
        return -1;
    }

    for (i = 0; i < 8; i++) {
        out_info->name[i] = vol->name[i];
    }
    out_info->bios_drive = vol->bios_drive;
    out_info->part_idx   = vol->part_idx;
    out_info->is_alias   = vol->is_alias;

    target = resolve_vol_alias(vol);
    if (vol->is_alias && target) {
        for (i = 0; i < 8; i++) {
            out_info->target_name[i] = target->name[i];
        }
    } else {
        out_info->target_name[0] = '\0';
    }

    out_info->is_removable = target ? target->is_removable : vol->is_removable;
    out_info->is_mounted   = target ? target->is_mounted   : vol->is_mounted;
    out_info->fat_type     = target ? target->fat_type     : vol->fat_type;

    if (target && target->is_mounted) {
        out_info->total_clusters      = target->total_clusters;
        out_info->free_clusters       = vol_get_free_clusters(target);
        out_info->sectors_per_cluster = target->sectors_per_cluster;
        out_info->bytes_per_sector    = target->bytes_per_sector;
    } else {
        out_info->total_clusters      = 0;
        out_info->free_clusters       = 0;
        out_info->sectors_per_cluster = target ? target->sectors_per_cluster : 0;
        out_info->bytes_per_sector    = target ? target->bytes_per_sector : 512;
    }

    return 0;
}

/* --- FAT Cache Management --- */

static int fat_flush(void) {
    uint8_t f;
    os_volume_t *vol;
    if (!fat_dirty || cached_fat_sec == 0xFFFF || cached_fat_vol >= MAX_VOLUMES) {
        return 0;
    }
    vol = resolve_vol_alias(&volumes[cached_fat_vol]);
    if (!vol || !vol->active) {
        fat_dirty = 0;
        return -1;
    }
    for (f = 0; f < vol->num_fats; f++) {
        uint32_t lba = (uint32_t)vol->fat_lba + (uint16_t)((uint16_t)f * vol->sectors_per_fat) + cached_fat_sec;
        if (vol_rw_lba(vol, 1, lba, MK_FP(KERNEL_SEG, fat_buf)) != 0) {
            return -1;
        }
    }
    fat_dirty = 0;
    return 0;
}

static int fat_load_sector(os_volume_t *vol, uint16_t fat_sec_idx) {
    uint8_t v_idx;
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    v_idx = (uint8_t)(vol - volumes);

    if (cached_fat_sec == fat_sec_idx && cached_fat_vol == v_idx) {
        return 0;
    }
    if (fat_flush() != 0) {
        return -1;
    }
    if (vol_rw_lba(vol, 0, (uint32_t)vol->fat_lba + fat_sec_idx, MK_FP(KERNEL_SEG, fat_buf)) != 0) {
        return -1;
    }
    cached_fat_sec = fat_sec_idx;
    cached_fat_vol = v_idx;
    return 0;
}

static uint16_t fat_get_cluster(os_volume_t *vol, uint16_t cluster) {
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return 0xFFFF;
    }
    if (vol->fat_type == FAT_TYPE_FAT12) {
        uint16_t byte_off = cluster + (cluster >> 1);
        uint16_t sec_idx  = byte_off >> 9;
        uint16_t off      = byte_off & 511;
        uint8_t  b0, b1;
        uint16_t val;

        if (fat_load_sector(vol, sec_idx) != 0) {
            return 0xFFFF;
        }
        b0 = fat_buf[off];
        if (off == 511) {
            if (fat_load_sector(vol, sec_idx + 1) != 0) {
                return 0xFFFF;
            }
            b1 = fat_buf[0];
        } else {
            b1 = fat_buf[off + 1];
        }
        val = (uint16_t)b0 | ((uint16_t)b1 << 8);
        if (cluster & 1) {
            val >>= 4;
        } else {
            val &= 0x0FFF;
        }
        return val;
    } else {
        uint16_t sec_idx = cluster >> 8;
        uint16_t off     = (cluster & 0xFF) << 1;
        if (fat_load_sector(vol, sec_idx) != 0) {
            return 0xFFFF;
        }
        return (uint16_t)fat_buf[off] | ((uint16_t)fat_buf[off + 1] << 8);
    }
}

static int fat_set_cluster(os_volume_t *vol, uint16_t cluster, uint16_t val) {
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol->fat_type == FAT_TYPE_FAT12) {
        uint16_t byte_off = cluster + (cluster >> 1);
        uint16_t sec_idx  = byte_off >> 9;
        uint16_t off      = byte_off & 511;
        uint8_t  b0, b1;

        if (off == 511) {
            if (fat_load_sector(vol, sec_idx) != 0) {
                return -1;
            }
            b0 = fat_buf[511];
            if (fat_load_sector(vol, sec_idx + 1) != 0) {
                return -1;
            }
            b1 = fat_buf[0];

            if (cluster & 1) {
                b0 = (b0 & 0x0F) | (uint8_t)((val & 0x0F) << 4);
                b1 = (uint8_t)((val >> 4) & 0xFF);
            } else {
                b0 = (uint8_t)(val & 0xFF);
                b1 = (b1 & 0xF0) | (uint8_t)((val >> 8) & 0x0F);
            }

            fat_buf[0] = b1;
            fat_dirty  = 1;
            if (fat_load_sector(vol, sec_idx) != 0) {
                return -1;
            }
            fat_buf[511] = b0;
            fat_dirty    = 1;
        } else {
            if (fat_load_sector(vol, sec_idx) != 0) {
                return -1;
            }
            b0 = fat_buf[off];
            b1 = fat_buf[off + 1];

            if (cluster & 1) {
                b0 = (b0 & 0x0F) | (uint8_t)((val & 0x0F) << 4);
                b1 = (uint8_t)((val >> 4) & 0xFF);
            } else {
                b0 = (uint8_t)(val & 0xFF);
                b1 = (b1 & 0xF0) | (uint8_t)((val >> 8) & 0x0F);
            }

            fat_buf[off]     = b0;
            fat_buf[off + 1] = b1;
            fat_dirty        = 1;
        }
        return 0;
    } else {
        uint16_t sec_idx = cluster >> 8;
        uint16_t off     = (cluster & 0xFF) << 1;
        if (fat_load_sector(vol, sec_idx) != 0) {
            return -1;
        }
        fat_buf[off]     = (uint8_t)(val & 0xFF);
        fat_buf[off + 1] = (uint8_t)(val >> 8);
        fat_dirty = 1;
        return 0;
    }
}

/* --- Directory & Path Resolution Helpers --- */

static int make_83_component(const char *comp, uint8_t out_name[11]) {
    int i;
    int pos = 0;

    if (!comp || comp[0] == '\0') {
        return -1;
    }
    for (i = 0; i < 11; i++) {
        out_name[i] = ' ';
    }

    if (comp[0] == '.' && comp[1] == '\0') {
        out_name[0] = '.';
        return 0;
    }
    if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
        out_name[0] = '.';
        out_name[1] = '.';
        return 0;
    }

    i = 0;
    while (comp[pos] != '\0' && comp[pos] != '.') {
        if (comp[pos] == ' ' || is_sep(comp[pos])) {
            return -1;
        }
        if (i < 8) {
            out_name[i++] = (uint8_t)to_upper(comp[pos]);
        }
        pos++;
    }
    if (i == 0) {
        return -1;
    }
    if (comp[pos] == '.') {
        pos++;
        i = 8;
        while (comp[pos] != '\0' && i < 11) {
            if (comp[pos] == '.' || comp[pos] == ' ' || is_sep(comp[pos])) {
                return -1;
            }
            out_name[i++] = (uint8_t)to_upper(comp[pos]);
            pos++;
        }
    }
    return 0;
}

static void format_display_name(const uint8_t raw[11], char out[13]) {
    int i, pos = 0;
    for (i = 0; i < 8 && raw[i] != ' '; i++) {
        out[pos++] = (char)raw[i];
    }
    if (raw[8] != ' ') {
        out[pos++] = '.';
        for (i = 8; i < 11 && raw[i] != ' '; i++) {
            out[pos++] = (char)raw[i];
        }
    }
    out[pos] = '\0';
}

static int name_equals(const uint8_t a[11], const uint8_t b[11]) {
    int i;
    for (i = 0; i < 11; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void fill_dirent_meta(fat16_dirent_t *ent, const uint8_t name83[11],
                             uint8_t attr, uint16_t first_cluster, uint32_t size) {
    int i;
    for (i = 0; i < 11; i++) {
        ent->name[i] = name83[i];
    }
    ent->attr           = attr;
    ent->nt_res         = 0;
    ent->crt_time_tenth = 0;
    ent->crt_time       = 0x5000;
    ent->crt_date       = 0x5D6E;
    ent->lst_acc_date   = 0x5D6E;
    ent->fst_clus_hi    = 0;
    ent->wrt_time       = 0x5000;
    ent->wrt_date       = 0x5D6E;
    ent->fst_clus_lo    = first_cluster;
    ent->file_size      = size;
}

static uint32_t cluster_to_lba(os_volume_t *vol, uint16_t cluster) {
    uint32_t clus_offset;
    uint8_t spc;
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return 0;
    }
    clus_offset = (uint32_t)(cluster - 2);
    spc = vol->sectors_per_cluster;
    while (spc > 1) {
        clus_offset <<= 1;
        spc >>= 1;
    }
    return (uint32_t)vol->data_lba + clus_offset;
}

static int dir_get_sector_lba(os_volume_t *vol, uint16_t dir_cluster, uint16_t sec_idx, uint32_t *out_lba) {
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (dir_cluster == 0) {
        if (sec_idx >= vol->root_sectors) {
            return -1;
        }
        *out_lba = (uint32_t)vol->root_lba + sec_idx;
        return 0;
    } else {
        uint16_t clus_steps = sec_idx / vol->sectors_per_cluster;
        uint16_t sec_offset = sec_idx % vol->sectors_per_cluster;
        uint16_t cur        = dir_cluster;
        while (clus_steps > 0) {
            if (cur < 2 || is_eof_cluster(vol, cur)) {
                return -1;
            }
            cur = fat_get_cluster(vol, cur);
            clus_steps--;
        }
        if (cur < 2 || is_eof_cluster(vol, cur)) {
            return -1;
        }
        *out_lba = cluster_to_lba(vol, cur) + sec_offset;
        return 0;
    }
}

static int find_dir_entry(os_volume_t *vol, uint16_t dir_cluster, const uint8_t name83[11],
                          fat16_dirent_t *out_entry, uint32_t *out_sec_lba, uint16_t *out_idx) {
    uint16_t s, i;
    uint32_t sec_lba;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }

    for (s = 0; dir_get_sector_lba(vol, dir_cluster, s, &sec_lba) == 0; s++) {
        if (vol_rw_lba(vol, 0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        for (i = 0; i < 16; i++) {
            uint8_t first = entries[i].name[0];
            if (first == 0x00) {
                return -1;
            }
            if (first == 0xE5 || (entries[i].attr & FAT_ATTR_VOLUME_ID)) {
                continue;
            }
            if (name_equals(entries[i].name, name83)) {
                if (out_entry)   *out_entry   = entries[i];
                if (out_sec_lba) *out_sec_lba = sec_lba;
                if (out_idx)     *out_idx     = i;
                return 0;
            }
        }
    }
    return -1;
}

static int find_free_dir_slot(os_volume_t *vol, uint16_t dir_cluster, uint32_t *out_sec_lba, uint16_t *out_idx) {
    uint16_t s, i;
    uint32_t sec_lba;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }

    for (s = 0; dir_get_sector_lba(vol, dir_cluster, s, &sec_lba) == 0; s++) {
        if (vol_rw_lba(vol, 0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        for (i = 0; i < 16; i++) {
            uint8_t first = entries[i].name[0];
            if (first == 0x00 || first == 0xE5) {
                *out_sec_lba = sec_lba;
                *out_idx     = i;
                return 0;
            }
        }
    }
    return -1;
}

static uint16_t alloc_free_cluster(os_volume_t *vol, uint16_t start_hint) {
    uint16_t c;
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active || vol->total_clusters == 0) {
        return 0;
    }
    if (start_hint < 2 || start_hint >= vol->total_clusters + 2) {
        start_hint = 2;
    }
    for (c = start_hint; c < vol->total_clusters + 2; c++) {
        if (fat_get_cluster(vol, c) == 0x0000) {
            if (fat_set_cluster(vol, c, eof_cluster_marker(vol)) != 0) {
                return 0;
            }
            return c;
        }
    }
    for (c = 2; c < start_hint; c++) {
        if (fat_get_cluster(vol, c) == 0x0000) {
            if (fat_set_cluster(vol, c, eof_cluster_marker(vol)) != 0) {
                return 0;
            }
            return c;
        }
    }
    return 0;
}

static int zero_cluster_sectors(os_volume_t *vol, uint16_t cluster) {
    uint32_t first_lba;
    uint8_t s;
    int j;
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active || cluster < 2) {
        return -1;
    }
    for (j = 0; j < 512; j++) {
        io_buf[j] = 0;
    }
    first_lba = cluster_to_lba(vol, cluster);
    for (s = 0; s < vol->sectors_per_cluster; s++) {
        if (vol_rw_lba(vol, 1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
    }
    return 0;
}

static int free_cluster_chain(os_volume_t *vol, uint16_t start_cluster) {
    uint16_t cur = start_cluster;
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    while (cur >= 2 && !is_eof_cluster(vol, cur)) {
        uint16_t next = fat_get_cluster(vol, cur);
        if (fat_set_cluster(vol, cur, 0x0000) != 0) {
            return -1;
        }
        cur = next;
    }
    return fat_flush();
}

static int step_into_dir(os_volume_t *vol, uint16_t cur_cluster, const char *comp, uint16_t *out_next_cluster) {
    uint8_t name83[11];
    fat16_dirent_t ent;

    if (comp[0] == '.' && comp[1] == '\0') {
        *out_next_cluster = cur_cluster;
        return 0;
    }
    if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
        if (cur_cluster == 0) {
            *out_next_cluster = 0;
            return 0;
        }
        make_83_component("..", name83);
        if (find_dir_entry(vol, cur_cluster, name83, &ent, 0, 0) != 0) {
            *out_next_cluster = 0;
            return 0;
        }
        *out_next_cluster = ent.fst_clus_lo;
        return 0;
    }

    if (make_83_component(comp, name83) != 0) {
        return -1;
    }
    if (find_dir_entry(vol, cur_cluster, name83, &ent, 0, 0) != 0) {
        return -1;
    }
    if ((ent.attr & FAT_ATTR_DIRECTORY) == 0) {
        return -1;
    }
    *out_next_cluster = ent.fst_clus_lo;
    return 0;
}

/*
 * Resolve a volume subpath into its parent directory cluster and 8.3 leaf name.
 * Handles:
 *   - Absolute paths (starting with '/' or '\') -> rooted at cluster 0
 *   - Volume paths (from "DEV:PATH") -> rooted at cluster 0
 *   - Relative paths -> relative to vol->cwd_cluster
 */
static int resolve_parent_and_leaf(os_volume_t *vol, const char __far *subpath,
                                   uint16_t *out_parent_clus, uint8_t out_leaf83[11],
                                   uint8_t is_volume_rooted) {
    uint16_t cur_clus;
    uint16_t pos = 0;
    char comp[16];

    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active || !subpath || subpath[0] == '\0') {
        return -1;
    }

    if (is_sep(subpath[0]) || is_volume_rooted) {
        cur_clus = 0;
        while (is_sep(subpath[pos])) {
            pos++;
        }
    } else {
        cur_clus = vol->cwd_cluster;
    }

    if (subpath[pos] == '\0') {
        return -1;
    }

    for (;;) {
        uint16_t k = 0;
        while (subpath[pos] != '\0' && !is_sep(subpath[pos])) {
            if (k >= 14) {
                return -1;
            }
            comp[k++] = subpath[pos++];
        }
        comp[k] = '\0';

        while (is_sep(subpath[pos])) {
            pos++;
        }

        if (subpath[pos] == '\0') {
            /* Leaf component */
            if (make_83_component(comp, out_leaf83) != 0) {
                return -1;
            }
            *out_parent_clus = cur_clus;
            return 0;
        }

        /* Intermediate directory component */
        if (step_into_dir(vol, cur_clus, comp, &cur_clus) != 0) {
            return -1;
        }
    }
}

static void canon_pop(char *canon) {
    int len = 0;
    while (canon[len] != '\0') {
        len++;
    }
    while (len > 1 && canon[len - 1] != '/') {
        len--;
    }
    if (len > 1) {
        len--;
    }
    canon[len] = '\0';
}

static void canon_push(char *canon, const char *comp) {
    int len = 0;
    int i = 0;
    while (canon[len] != '\0') {
        len++;
    }
    if (len > 1 && len < 62) {
        canon[len++] = '/';
    }
    while (comp[i] != '\0' && len < 62) {
        canon[len++] = to_upper(comp[i++]);
    }
    canon[len] = '\0';
}

/* --- Dynamic BPB & MBR Parsing / Initialization --- */

/* Safe cluster calculation avoiding 32-bit division in C */
static uint16_t calc_clusters(uint32_t total_sec, uint16_t data_lba, uint8_t spc) {
    uint32_t rem;
    uint16_t shift = 0;
    if (total_sec <= data_lba || spc == 0) {
        return 0;
    }
    rem = total_sec - data_lba;
    while ((1 << shift) < spc && shift < 8) {
        shift++;
    }
    rem = rem >> shift;
    if (rem > 65520UL) {
        rem = 65520UL;
    }
    return (uint16_t)rem;
}

static int parse_bpb(os_volume_t *vol, const uint8_t *buf) {
    vol->bytes_per_sector    = rd_le16(&buf[11]);
    vol->sectors_per_cluster = buf[13];
    vol->reserved_sectors    = rd_le16(&buf[14]);
    vol->num_fats            = buf[16];
    vol->root_entries        = rd_le16(&buf[17]);
    vol->total_sectors       = rd_le16(&buf[19]);
    if (vol->total_sectors == 0) {
        vol->total_sectors   = rd_le32(&buf[32]);
    }
    vol->sectors_per_fat     = rd_le16(&buf[22]);

    if (vol->is_removable || vol->sectors_per_track == 0) {
        if (rd_le16(&buf[24]) != 0) {
            vol->sectors_per_track = rd_le16(&buf[24]);
        }
        if (rd_le16(&buf[26]) != 0) {
            vol->num_heads = rd_le16(&buf[26]);
        }
    }

    if (vol->bytes_per_sector != 512 || vol->sectors_per_cluster == 0) {
        return -1;
    }

    vol->fat_lba        = vol->reserved_sectors;
    vol->root_lba       = vol->fat_lba + (uint16_t)vol->num_fats * vol->sectors_per_fat;
    vol->root_sectors   = (vol->root_entries * 32) / 512;
    vol->data_lba       = vol->root_lba + vol->root_sectors;
    vol->total_clusters = calc_clusters(vol->total_sectors, vol->data_lba, vol->sectors_per_cluster);

    if (vol->total_clusters < 4085) {
        vol->fat_type = FAT_TYPE_FAT12;
    } else {
        vol->fat_type = FAT_TYPE_FAT16;
    }

    vol->cwd_cluster    = 0;
    vol->cwd_path[0]    = '/';
    vol->cwd_path[1]    = '\0';
    vol->active         = 1;
    vol->is_mounted     = 1;
    return 0;
}

static void register_floppy_drive(uint8_t drive_num, const char *name) {
    os_volume_t *vol;
    int i;
    if (num_volumes >= MAX_VOLUMES) {
        return;
    }
    vol = &volumes[num_volumes++];
    for (i = 0; i < 7 && name[i] != '\0'; i++) {
        vol->name[i] = name[i];
    }
    vol->name[i] = '\0';

    vol->active              = 1;
    vol->is_alias            = 0;
    vol->target_vol_idx      = 0;
    vol->bios_drive          = drive_num;
    vol->part_idx            = 0xFF;
    vol->is_removable        = 1;
    vol->is_mounted          = 0;
    vol->fat_type            = FAT_TYPE_FAT12;
    vol->start_lba           = 0;
    vol->sectors_per_track   = 9;  /* default 720KB PC/XT fallback */
    vol->num_heads           = 2;
    vol->total_sectors       = 1440;
    vol->bytes_per_sector    = 512;
    vol->sectors_per_cluster = 2;
    vol->reserved_sectors    = 1;
    vol->num_fats            = 2;
    vol->root_entries        = 112;
    vol->sectors_per_fat     = 3;
    vol->fat_lba             = 1;
    vol->root_lba            = 7;
    vol->root_sectors        = 7;
    vol->data_lba            = 14;
    vol->total_clusters      = 713;
    vol->cwd_cluster         = 0;
    vol->cwd_path[0]         = '/';
    vol->cwd_path[1]         = '\0';
}

static void probe_drive(uint8_t drive_num, const char *base_name) {
    uint16_t cyls = 0;
    uint8_t  heads = 0;
    uint8_t  spt = 0;
    uint8_t  drives = 0;
    mbr_partition_t *parts;
    mbr_partition_t parts_copy[4];
    int is_mbr = 0;
    int p;
    int valid_parts = 0;
    uint8_t first_part_idx = 0xFF;

    if (bios_get_drive_params(drive_num, &cyls, &heads, &spt, &drives) != 0) {
        return;
    }
    if (spt == 0 || heads == 0) {
        return;
    }

    /* Read Sector 0 (LBA 0) */
    if (bios_disk_rw_lba(drive_num, 0, 0, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return;
    }
    if (io_buf[510] != 0x55 || io_buf[511] != 0xAA) {
        return;
    }

    /* Check if MBR: inspect partition table at 0x01BE */
    parts = (mbr_partition_t *)&io_buf[0x01BE];
    for (p = 0; p < 4; p++) {
        parts_copy[p] = parts[p];
            if ((parts[p].part_type == 0x04 || parts[p].part_type == 0x06 ||
                 parts[p].part_type == 0x0E || parts[p].part_type == 0x01) &&
                parts[p].start_lba > 0 && parts[p].total_sectors > 0) {
                valid_parts++;
            }
        }
        /* If offset 0x36 contains "FAT", this is a raw superfloppy VBR, not an MBR */
        if (io_buf[0x36] == 'F' && io_buf[0x37] == 'A' && io_buf[0x38] == 'T') {
            is_mbr = 0;
        } else if (valid_parts > 0) {
            is_mbr = 1;
        }

        if (is_mbr) {
            for (p = 0; p < 4 && num_volumes < MAX_VOLUMES; p++) {
                uint8_t pt = parts_copy[p].part_type;
                if (pt == 0x04 || pt == 0x06 || pt == 0x0E || pt == 0x01) {
                    uint8_t v_idx = num_volumes;
                    os_volume_t *vol = &volumes[v_idx];
                    int i;

                    vol->name[0] = base_name[0];
                    vol->name[1] = base_name[1];
                    vol->name[2] = base_name[2];
                    vol->name[3] = (char)('A' + p);
                    vol->name[4] = '\0';

                    vol->bios_drive        = drive_num;
                    vol->part_idx          = (uint8_t)p;
                    vol->start_lba         = parts_copy[p].start_lba;
                    vol->total_sectors     = parts_copy[p].total_sectors;
                    vol->sectors_per_track = spt;
                    vol->num_heads         = heads;
                    vol->is_alias          = 0;
                    vol->target_vol_idx    = 0;
                    vol->is_removable      = 0;
                    vol->is_mounted        = 0;
                    vol->active            = 1;
                    vol->cwd_cluster       = 0;
                    vol->cwd_path[0]       = '/';
                    vol->cwd_path[1]       = '\0';

                    /* Read VBR of partition */
                    if (bios_disk_rw_lba(drive_num, 0, vol->start_lba, spt, heads, MK_FP(KERNEL_SEG, io_buf)) == 0 &&
                        io_buf[510] == 0x55 && io_buf[511] == 0xAA) {
                        if (parse_bpb(vol, io_buf) == 0) {
                            vol->is_mounted = 1;
                        }
                    }
                    if (first_part_idx == 0xFF && vol->is_mounted) {
                        first_part_idx = v_idx;
                    }
                    num_volumes++;
                }
            }
        /* Add whole-drive alias e.g. "HD0" -> "HD0A" */
        if (first_part_idx != 0xFF && num_volumes < MAX_VOLUMES) {
            os_volume_t *alias = &volumes[num_volumes];
            *alias = volumes[first_part_idx];
            alias->name[0]        = base_name[0];
            alias->name[1]        = base_name[1];
            alias->name[2]        = base_name[2];
            alias->name[3]        = '\0';
            alias->part_idx       = 0xFF;
            alias->is_alias       = 1;
            alias->target_vol_idx = first_part_idx;
            alias->active         = 1;
            alias->is_removable   = volumes[first_part_idx].is_removable;
            alias->is_mounted     = volumes[first_part_idx].is_mounted;
            alias->fat_type       = volumes[first_part_idx].fat_type;
            alias->cwd_cluster    = 0;
            alias->cwd_path[0]    = '/';
            alias->cwd_path[1]    = '\0';
            num_volumes++;
        }
    } else {
        /* Raw disk (superfloppy): mount LBA 0 */
        if (num_volumes < MAX_VOLUMES) {
            uint8_t raw_idx = num_volumes;
            os_volume_t *vol = &volumes[raw_idx];

            vol->name[0]           = base_name[0];
            vol->name[1]           = base_name[1];
            vol->name[2]           = base_name[2];
            vol->name[3]           = '\0';
            vol->bios_drive        = drive_num;
            vol->part_idx          = 0xFF;
            vol->start_lba         = 0;
            vol->sectors_per_track = spt;
            vol->num_heads         = heads;
            vol->is_alias          = 0;
            vol->is_removable      = 0;
            vol->is_mounted        = 1;

            if (parse_bpb(vol, io_buf) == 0) {
                num_volumes++;

                /* Add alias e.g. "HD0A" -> "HD0" */
                if (num_volumes < MAX_VOLUMES) {
                    os_volume_t *alias = &volumes[num_volumes];
                    *alias = volumes[raw_idx];
                    alias->name[0]        = base_name[0];
                    alias->name[1]        = base_name[1];
                    alias->name[2]        = base_name[2];
                    alias->name[3]        = 'A';
                    alias->name[4]        = '\0';
                    alias->part_idx       = 0;
                    alias->is_alias       = 1;
                    alias->target_vol_idx = raw_idx;
                    alias->active         = 1;
                    alias->is_removable   = volumes[raw_idx].is_removable;
                    alias->is_mounted     = volumes[raw_idx].is_mounted;
                    alias->fat_type       = volumes[raw_idx].fat_type;
                    alias->cwd_cluster    = 0;
                    alias->cwd_path[0]    = '/';
                    alias->cwd_path[1]    = '\0';
                    num_volumes++;
                }
            }
        }
    }
}

int fat16_rescan_drives(void) {
    uint8_t boot_drive;
    uint8_t v;
    uint8_t boot_vol_idx = 0xFF;
    uint16_t bda_equip;
    uint8_t num_floppies = 0;

    /* Flush cached FAT */
    fat_flush();
    cached_fat_vol  = 0xFF;
    cached_fat_sec  = 0xFFFF;
    fat_dirty       = 0;

    num_volumes     = 0;
    current_vol_idx = 0;

    boot_drive = bios_get_boot_drive();

    /* 1. Register Floppy Drives from BDA Equipment Word */
    bda_equip = *((uint16_t __far *)MK_FP(0x0040, 0x0010));
    if (bda_equip & 1) {
        num_floppies = (uint8_t)(((bda_equip >> 6) & 3) + 1);
        if (num_floppies > 2) {
            num_floppies = 2;
        }
    }
    if (num_floppies >= 1 && num_volumes < MAX_VOLUMES) {
        register_floppy_drive(0x00, "FD0");
    }
    if (num_floppies >= 2 && num_volumes < MAX_VOLUMES) {
        register_floppy_drive(0x01, "FD1");
    }

    /* 2. Probe Hard Disk 0 (0x80) */
    probe_drive(0x80, "HD0");

    /* 3. Probe Hard Disk 1 (0x81) */
    probe_drive(0x81, "HD1");

    if (num_volumes == 0) {
        fs_mounted = 0;
        return -1;
    }

    /* Find volume matching boot_drive */
    for (v = 0; v < num_volumes; v++) {
        if (volumes[v].active && !volumes[v].is_alias && volumes[v].bios_drive == boot_drive) {
            boot_vol_idx = v;
            break;
        }
    }
    if (boot_vol_idx == 0xFF) {
        boot_vol_idx = 0;
    }

    /* Register SYS: as an alias pointing to boot volume */
    if (num_volumes < MAX_VOLUMES) {
        os_volume_t *sys = &volumes[num_volumes];
        *sys = volumes[boot_vol_idx];
        sys->name[0]        = 'S';
        sys->name[1]        = 'Y';
        sys->name[2]        = 'S';
        sys->name[3]        = '\0';
        sys->is_alias       = 1;
        sys->target_vol_idx = boot_vol_idx;
        sys->active         = 1;
        sys->is_removable   = volumes[boot_vol_idx].is_removable;
        sys->is_mounted     = volumes[boot_vol_idx].is_mounted;
        sys->fat_type       = volumes[boot_vol_idx].fat_type;
        sys->cwd_cluster    = 0;
        sys->cwd_path[0]    = '/';
        sys->cwd_path[1]    = '\0';
        current_vol_idx     = num_volumes;
        num_volumes++;
    } else {
        current_vol_idx = boot_vol_idx;
    }

    /* Restore RAM disk volume if active */
    if (ramdisk_info.active && num_volumes < MAX_VOLUMES) {
        os_volume_t *vol = &volumes[num_volumes++];
        uint16_t total_sectors = ramdisk_info.total_sectors;
        uint8_t spc = (ramdisk_info.size_kb <= 2048) ? 1 : ((ramdisk_info.size_kb <= 4096) ? 2 : 4);
        uint16_t root_entries = (ramdisk_info.size_kb <= 256) ? 64 : 128;
        uint16_t root_sectors = (root_entries * 32) / 512;
        uint16_t fat_bytes = (total_sectors + 2) + ((total_sectors + 2) >> 1);
        uint16_t sectors_per_fat = (fat_bytes + 511) >> 9;

        vol->name[0] = 'R'; vol->name[1] = 'A'; vol->name[2] = 'M'; vol->name[3] = '\0';
        vol->active = 1;
        vol->is_alias = 0;
        vol->target_vol_idx = 0;
        vol->bios_drive = 0xFF;
        vol->part_idx = 0xFF;
        vol->is_removable = 0;
        vol->is_mounted = 1;
        vol->fat_type = FAT_TYPE_FAT12;
        vol->sectors_per_track = 32;
        vol->num_heads = 1;
        vol->start_lba = 0;
        vol->total_sectors = total_sectors;
        vol->bytes_per_sector = 512;
        vol->sectors_per_cluster = spc;
        vol->reserved_sectors = 1;
        vol->num_fats = 1;
        vol->root_entries = root_entries;
        vol->sectors_per_fat = sectors_per_fat;
        vol->fat_lba = 1;
        vol->root_lba = 1 + sectors_per_fat;
        vol->root_sectors = root_sectors;
        vol->data_lba = 1 + sectors_per_fat + root_sectors;
        vol->total_clusters = (total_sectors - vol->data_lba) / spc;
        vol->cwd_cluster = 0;
        vol->cwd_path[0] = '/'; vol->cwd_path[1] = '\0';
    }

    fs_mounted = 1;
    return 0;
}

int fat16_init(void) {
    return fat16_rescan_drives();
}

/* --- Public Syscall Implementations --- */

int fat16_chdir(const char __far *path) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t cur_clus;
    uint16_t pos = 0;
    char new_path[64];
    char comp[16];
    int i;
    uint8_t is_vol_root;

    if (!fs_mounted || !path || path[0] == '\0') {
        return -1;
    }

    if (vol_resolve_path(path, &vol, &subpath) != 0) {
        return -1;
    }

    /* If switching volume alone (e.g. "cd HD0A:" or "cd SYS:") */
    if (subpath[0] == '\0') {
        uint8_t v_idx = (uint8_t)(vol - volumes);
        return vol_set_current(v_idx);
    }

    /* Also switch active volume to the specified volume */
    {
        uint8_t v_idx = (uint8_t)(vol - volumes);
        int rc = vol_set_current(v_idx);
        if (rc != 0) {
            return rc;
        }
    }

    is_vol_root = (uint8_t)(path != subpath);

    if (is_sep(subpath[0]) || is_vol_root) {
        cur_clus    = 0;
        new_path[0] = '/';
        new_path[1] = '\0';
        while (is_sep(subpath[pos])) {
            pos++;
        }
    } else {
        cur_clus = vol->cwd_cluster;
        for (i = 0; i < 64; i++) {
            new_path[i] = vol->cwd_path[i];
            if (vol->cwd_path[i] == '\0') break;
        }
    }

    while (subpath[pos] != '\0') {
        uint16_t k = 0;
        while (subpath[pos] != '\0' && !is_sep(subpath[pos])) {
            if (k >= 14) {
                return -1;
            }
            comp[k++] = subpath[pos++];
        }
        comp[k] = '\0';

        while (is_sep(subpath[pos])) {
            pos++;
        }

        if (comp[0] == '.' && comp[1] == '\0') {
            continue;
        }
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (step_into_dir(vol, cur_clus, "..", &cur_clus) != 0) {
                return -1;
            }
            canon_pop(new_path);
            continue;
        }

        if (step_into_dir(vol, cur_clus, comp, &cur_clus) != 0) {
            return -1;
        }
        canon_push(new_path, comp);
    }

    vol->cwd_cluster = cur_clus;
    for (i = 0; i < 64; i++) {
        vol->cwd_path[i] = new_path[i];
        if (new_path[i] == '\0') break;
    }
    if (vol->is_alias) {
        os_volume_t *target = resolve_vol_alias(vol);
        if (target) {
            target->cwd_cluster = cur_clus;
            for (i = 0; i < 64; i++) {
                target->cwd_path[i] = new_path[i];
                if (new_path[i] == '\0') break;
            }
        }
    }
    return 0;
}

int fat16_getcwd(char __far *buf, uint16_t max_len) {
    os_volume_t *vol = vol_get_current();
    uint16_t pos = 0;
    int i;

    if (!vol || !buf || max_len < 8) {
        return -1;
    }

    /* Prefix with volume name + colon, e.g. "SYS:" or "HD0A:" */
    for (i = 0; vol->name[i] != '\0' && pos + 1 < max_len; i++) {
        buf[pos++] = vol->name[i];
    }
    if (pos + 1 < max_len) {
        buf[pos++] = ':';
    }

    /* Append cwd_path */
    for (i = 0; vol->cwd_path[i] != '\0' && pos + 1 < max_len; i++) {
        buf[pos++] = vol->cwd_path[i];
    }
    buf[pos] = '\0';
    return 0;
}

int fat16_mkdir(const char __far *path) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    uint8_t dot83[11];
    uint32_t dir_sec_lba;
    uint16_t dir_idx;
    uint16_t new_clus;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;
    uint32_t first_lba;

    if (vol_resolve_path(path, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(path != subpath)) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }
    if (find_dir_entry(vol, parent_clus, name83, 0, 0, 0) == 0) {
        return -1;
    }
    if (find_free_dir_slot(vol, parent_clus, &dir_sec_lba, &dir_idx) != 0) {
        return -1;
    }

    new_clus = alloc_free_cluster(vol, 2);
    if (new_clus == 0) {
        return -1;
    }
    if (fat_flush() != 0) {
        return -1;
    }
    if (zero_cluster_sectors(vol, new_clus) != 0) {
        return -1;
    }

    /* Populate '.' and '..' */
    make_83_component(".", dot83);
    fill_dirent_meta(&entries[0], dot83, FAT_ATTR_DIRECTORY, new_clus, 0);
    make_83_component("..", dot83);
    fill_dirent_meta(&entries[1], dot83, FAT_ATTR_DIRECTORY, parent_clus, 0);
    first_lba = cluster_to_lba(vol, new_clus);
    if (vol_rw_lba(vol, 1, first_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    /* Write entry in parent directory */
    if (vol_rw_lba(vol, 0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    fill_dirent_meta(&entries[dir_idx], name83, FAT_ATTR_DIRECTORY, new_clus, 0);
    if (vol_rw_lba(vol, 1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    return 0;
}

static int is_dir_empty(os_volume_t *vol, uint16_t dir_cluster) {
    uint16_t s, i;
    uint32_t sec_lba;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    for (s = 0; dir_get_sector_lba(vol, dir_cluster, s, &sec_lba) == 0; s++) {
        if (vol_rw_lba(vol, 0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return 0;
        }
        for (i = 0; i < 16; i++) {
            uint8_t first = entries[i].name[0];
            if (first == 0x00) {
                return 1;
            }
            if (first == 0xE5 || first == '.') {
                continue;
            }
            return 0;
        }
    }
    return 1;
}

int fat16_rmdir(const char __far *path) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint32_t dir_sec_lba = 0;
    uint16_t dir_idx     = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (vol_resolve_path(path, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(path != subpath)) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }
    if (find_dir_entry(vol, parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) != 0) {
        return -1;
    }
    if ((entry.attr & FAT_ATTR_DIRECTORY) == 0) {
        return -1;
    }
    if (entry.fst_clus_lo == 0 || entry.fst_clus_lo == vol->cwd_cluster) {
        return -1;
    }
    if (!is_dir_empty(vol, entry.fst_clus_lo)) {
        return -1;
    }
    if (free_cluster_chain(vol, entry.fst_clus_lo) != 0) {
        return -1;
    }
    if (vol_rw_lba(vol, 0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    entries[dir_idx].name[0] = 0xE5;
    if (vol_rw_lba(vol, 1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    return 0;
}

int32_t fat16_file_size(const char __far *filename) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;

    if (vol_resolve_path(filename, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(filename != subpath)) != 0) {
        return -1;
    }
    if (find_dir_entry(vol, parent_clus, name83, &entry, 0, 0) != 0) {
        return -1;
    }
    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }
    return (int32_t)entry.file_size;
}

int32_t fat16_read_file_offset(const char __far *filename, void __far *dest, uint16_t max_bytes, uint32_t offset) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t cluster;
    uint16_t target_bytes;
    uint16_t bytes_read = 0;
    uint8_t __far *out_ptr = (uint8_t __far *)dest;
    uint8_t clus_shift;
    uint8_t spc;
    uint16_t clus_to_skip;
    uint16_t off_in_clus;
    uint16_t cluster_mask;
    uint16_t i;

    if (vol_resolve_path(filename, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(filename != subpath)) != 0) {
        return -1;
    }
    if (find_dir_entry(vol, parent_clus, name83, &entry, 0, 0) != 0) {
        return -1;
    }
    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }
    if (offset >= entry.file_size) {
        return 0; /* EOF */
    }

    target_bytes = (entry.file_size - offset < (uint32_t)max_bytes)
                       ? (uint16_t)(entry.file_size - offset)
                       : max_bytes;
    cluster = entry.fst_clus_lo;

    /* Calculate cluster shift: sectors_per_cluster * 512 */
    clus_shift = 9;
    spc = vol->sectors_per_cluster;
    while (spc > 1) {
        clus_shift++;
        spc >>= 1;
    }

    clus_to_skip = (uint16_t)(offset >> clus_shift);
    cluster_mask = ((uint16_t)vol->sectors_per_cluster << 9) - 1;
    off_in_clus  = (uint16_t)(offset & cluster_mask);

    for (i = 0; i < clus_to_skip; i++) {
        if (cluster < 2 || is_eof_cluster(vol, cluster)) {
            return 0;
        }
        cluster = fat_get_cluster(vol, cluster);
    }

    while (cluster >= 2 && !is_eof_cluster(vol, cluster) && bytes_read < target_bytes) {
        uint32_t first_lba = cluster_to_lba(vol, cluster);
        uint8_t s = (uint8_t)(off_in_clus >> 9);
        uint16_t sec_off = off_in_clus & 511;

        for (; s < vol->sectors_per_cluster && bytes_read < target_bytes; s++) {
            uint16_t avail_in_sec = 512 - sec_off;
            uint16_t chunk = target_bytes - bytes_read;
            uint16_t j;
            if (chunk > avail_in_sec) {
                chunk = avail_in_sec;
            }
            if (vol_rw_lba(vol, 0, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
            for (j = 0; j < chunk; j++) {
                out_ptr[bytes_read + j] = io_buf[sec_off + j];
            }
            bytes_read += chunk;
            sec_off = 0;
        }
        off_in_clus = 0;
        cluster = fat_get_cluster(vol, cluster);
    }

    return (int32_t)bytes_read;
}

int32_t fat16_read_file(const char __far *filename, void __far *dest, uint16_t max_bytes) {
    return fat16_read_file_offset(filename, dest, max_bytes, 0);
}

int fat16_load_executable(const char __far *filename, uint16_t dest_seg, uint32_t *out_size) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t cluster;
    uint16_t cur_seg = dest_seg;
    uint16_t cur_off = 0x0000;
    uint32_t remaining;

    if (vol_resolve_path(filename, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(filename != subpath)) != 0) {
        return -1;
    }
    if (find_dir_entry(vol, parent_clus, name83, &entry, 0, 0) != 0) {
        return -1;
    }
    if ((entry.attr & FAT_ATTR_DIRECTORY) || entry.file_size == 0) {
        return -1;
    }

    remaining = entry.file_size;
    cluster   = entry.fst_clus_lo;

    while (cluster >= 2 && !is_eof_cluster(vol, cluster) && remaining > 0) {
        uint32_t first_lba = cluster_to_lba(vol, cluster);
        uint8_t s;
        for (s = 0; s < vol->sectors_per_cluster && remaining > 0; s++) {
            if (vol_rw_lba(vol, 0, first_lba + s, MK_FP(cur_seg, cur_off)) != 0) {
                return -1;
            }
            if (remaining >= 512) {
                remaining -= 512;
            } else {
                remaining = 0;
            }
            cur_off += 512;
            if (cur_off == 0x0000) {
                cur_seg += 0x1000;
            }
        }
        cluster = fat_get_cluster(vol, cluster);
    }

    if (out_size) {
        *out_size = entry.file_size;
    }
    return 0;
}

int32_t fat16_write_file(const char __far *filename, const void __far *src, uint16_t size) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint32_t dir_sec_lba   = 0;
    uint16_t dir_idx       = 0;
    uint16_t first_cluster = 0;
    uint16_t prev_cluster  = 0;
    uint16_t search_hint   = 2;
    uint16_t written       = 0;
    const uint8_t __far *in_ptr = (const uint8_t __far *)src;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (vol_resolve_path(filename, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(filename != subpath)) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }

    if (find_dir_entry(vol, parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) == 0) {
        if (entry.attr & FAT_ATTR_DIRECTORY) {
            return -1;
        }
        if (entry.fst_clus_lo >= 2) {
            if (free_cluster_chain(vol, entry.fst_clus_lo) != 0) {
                return -1;
            }
        }
    } else {
        if (find_free_dir_slot(vol, parent_clus, &dir_sec_lba, &dir_idx) != 0) {
            return -1;
        }
    }

    while (written < size) {
        uint16_t clus = alloc_free_cluster(vol, search_hint);
        uint32_t first_lba;
        uint8_t s;

        if (clus == 0) {
            return -1;
        }
        search_hint = clus + 1;

        if (first_cluster == 0) {
            first_cluster = clus;
        } else {
            if (fat_set_cluster(vol, prev_cluster, clus) != 0) {
                return -1;
            }
        }
        prev_cluster = clus;

        first_lba = cluster_to_lba(vol, clus);
        for (s = 0; s < vol->sectors_per_cluster; s++) {
            uint16_t j;
            for (j = 0; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (vol_rw_lba(vol, 1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
        }
    }

    if (fat_flush() != 0) {
        return -1;
    }

    if (vol_rw_lba(vol, 0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    fill_dirent_meta(&entries[dir_idx], name83, FAT_ATTR_ARCHIVE, first_cluster, (uint32_t)size);
    if (vol_rw_lba(vol, 1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    return (int32_t)size;
}

int32_t fat16_append_file(const char __far *filename, const void __far *src, uint16_t size) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint32_t dir_sec_lba = 0;
    uint16_t dir_idx     = 0;
    uint16_t last_clus;
    uint16_t cluster_bytes;
    uint16_t off_in_clus;
    uint16_t written = 0;
    uint16_t search_hint = 2;
    const uint8_t __far *in_ptr = (const uint8_t __far *)src;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (vol_resolve_path(filename, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(filename != subpath)) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }

    if (find_dir_entry(vol, parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) != 0 ||
        entry.file_size == 0 || entry.fst_clus_lo < 2) {
        return fat16_write_file(filename, src, size);
    }

    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    last_clus = entry.fst_clus_lo;
    while (last_clus >= 2 && !is_eof_cluster(vol, last_clus)) {
        uint16_t next = fat_get_cluster(vol, last_clus);
        if (is_eof_cluster(vol, next) || next < 2) {
            break;
        }
        last_clus = next;
    }

    cluster_bytes = (uint16_t)vol->sectors_per_cluster * 512;
    off_in_clus   = (uint16_t)(entry.file_size & (cluster_bytes - 1));

    if (off_in_clus != 0) {
        uint32_t first_lba   = cluster_to_lba(vol, last_clus);
        uint8_t  sec_in_clus = (uint8_t)(off_in_clus >> 9);
        uint16_t off_in_sec  = off_in_clus & 511;
        uint8_t  s;

        if (off_in_sec != 0) {
            uint16_t j;
            if (vol_rw_lba(vol, 0, first_lba + sec_in_clus, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
            for (j = off_in_sec; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (vol_rw_lba(vol, 1, first_lba + sec_in_clus, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
            sec_in_clus++;
        }

        for (s = sec_in_clus; s < vol->sectors_per_cluster && written < size; s++) {
            uint16_t j;
            for (j = 0; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (vol_rw_lba(vol, 1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
        }
    }

    while (written < size) {
        uint16_t clus = alloc_free_cluster(vol, search_hint);
        uint32_t first_lba;
        uint8_t  s;

        if (clus == 0) {
            return -1;
        }
        search_hint = clus + 1;
        if (fat_set_cluster(vol, last_clus, clus) != 0) {
            return -1;
        }
        last_clus = clus;

        first_lba = cluster_to_lba(vol, clus);
        for (s = 0; s < vol->sectors_per_cluster; s++) {
            uint16_t j;
            for (j = 0; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (vol_rw_lba(vol, 1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
        }
    }

    if (fat_flush() != 0) {
        return -1;
    }

    if (vol_rw_lba(vol, 0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    entries[dir_idx].file_size = entry.file_size + (uint32_t)size;
    if (vol_rw_lba(vol, 1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    return (int32_t)size;
}

int fat16_delete_file(const char __far *filename) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint32_t dir_sec_lba = 0;
    uint16_t dir_idx     = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (vol_resolve_path(filename, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(vol, subpath, &parent_clus, name83, (uint8_t)(filename != subpath)) != 0) {
        return -1;
    }
    if (find_dir_entry(vol, parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) != 0) {
        return -1;
    }
    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }
    if (entry.fst_clus_lo >= 2) {
        if (free_cluster_chain(vol, entry.fst_clus_lo) != 0) {
            return -1;
        }
    }
    if (vol_rw_lba(vol, 0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    entries[dir_idx].name[0] = 0xE5;
    if (vol_rw_lba(vol, 1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    return 0;
}

int fat16_copy_file(const char __far *src_path, const char __far *dst_path, uint8_t append) {
    static uint8_t copy_buf[2048];
    os_volume_t *src_vol, *dst_vol;
    const char __far *src_sub;
    const char __far *dst_sub;
    uint16_t src_parent, dst_parent;
    uint8_t  src_name83[11], dst_name83[11];
    fat16_dirent_t src_entry;
    uint32_t remaining;
    uint16_t cur_clus;
    uint8_t  first_chunk = (uint8_t)(!append);

    if (vol_resolve_path(src_path, &src_vol, &src_sub) != 0) {
        return -1;
    }
    if (vol_ensure_mounted(src_vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(src_vol, src_sub, &src_parent, src_name83, (uint8_t)(src_path != src_sub)) != 0) {
        return -1;
    }
    if (find_dir_entry(src_vol, src_parent, src_name83, &src_entry, 0, 0) != 0) {
        return -1;
    }
    if (src_entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }

    if (vol_resolve_path(dst_path, &dst_vol, &dst_sub) != 0) {
        return -1;
    }
    if (vol_ensure_mounted(dst_vol) != 0) {
        return -3;
    }
    if (resolve_parent_and_leaf(dst_vol, dst_sub, &dst_parent, dst_name83, (uint8_t)(dst_path != dst_sub)) != 0) {
        return -1;
    }
    if (dst_name83[0] == '.') {
        return -1;
    }
    if (!append && resolve_vol_alias(src_vol) == resolve_vol_alias(dst_vol) &&
        src_parent == dst_parent && name_equals(src_name83, dst_name83)) {
        return 0;
    }

    remaining = src_entry.file_size;
    cur_clus  = src_entry.fst_clus_lo;

    if (remaining == 0) {
        if (first_chunk) {
            return (fat16_write_file(dst_path, MK_FP(KERNEL_SEG, copy_buf), 0) < 0) ? -1 : 0;
        }
        return 0;
    }

    while (cur_clus >= 2 && !is_eof_cluster(src_vol, cur_clus) && remaining > 0) {
        uint32_t first_lba = cluster_to_lba(src_vol, cur_clus);
        uint16_t next_clus = fat_get_cluster(src_vol, cur_clus);
        uint8_t  s;
        uint16_t clus_bytes = 0;
        int32_t rc;

        for (s = 0; s < src_vol->sectors_per_cluster && remaining > 0; s++) {
            uint16_t chunk = (remaining > 512) ? 512 : (uint16_t)remaining;

            if (vol_rw_lba(src_vol, 0, first_lba + s, MK_FP(KERNEL_SEG, copy_buf + clus_bytes)) != 0) {
                return -1;
            }
            clus_bytes += chunk;
            remaining  -= chunk;
        }
        if (first_chunk) {
            rc = fat16_write_file(dst_path, MK_FP(KERNEL_SEG, copy_buf), clus_bytes);
            first_chunk = 0;
        } else {
            rc = fat16_append_file(dst_path, MK_FP(KERNEL_SEG, copy_buf), clus_bytes);
        }
        if (rc < 0) {
            return -1;
        }
        cur_clus = next_clus;
    }

    return 0;
}

int fat16_rename_file(const char __far *src_path, const char __far *dst_path) {
    os_volume_t *src_vol, *dst_vol;
    const char __far *src_sub;
    const char __far *dst_sub;
    uint16_t src_parent, dst_parent;
    uint8_t  src_name83[11], dst_name83[11];
    fat16_dirent_t src_entry, dst_entry;
    uint32_t src_sec_lba = 0;
    uint16_t src_idx     = 0;
    uint32_t dst_sec_lba = 0;
    uint16_t dst_idx     = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;
    int i;

    if (vol_resolve_path(src_path, &src_vol, &src_sub) != 0) {
        return -1;
    }
    if (resolve_parent_and_leaf(src_vol, src_sub, &src_parent, src_name83, (uint8_t)(src_path != src_sub)) != 0) {
        return -1;
    }
    if (find_dir_entry(src_vol, src_parent, src_name83, &src_entry, &src_sec_lba, &src_idx) != 0) {
        return -1;
    }
    if (src_entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }

    if (vol_resolve_path(dst_path, &dst_vol, &dst_sub) != 0) {
        return -1;
    }
    if (vol_ensure_mounted(src_vol) != 0 || vol_ensure_mounted(dst_vol) != 0) {
        return -3;
    }
    if (resolve_vol_alias(src_vol) != resolve_vol_alias(dst_vol)) {
        /* Cannot rename across physical volumes */
        return -1;
    }

    if (resolve_parent_and_leaf(dst_vol, dst_sub, &dst_parent, dst_name83, (uint8_t)(dst_path != dst_sub)) != 0) {
        return -1;
    }
    if (dst_name83[0] == '.') {
        return -1;
    }
    if (src_parent == dst_parent && name_equals(src_name83, dst_name83)) {
        return 0;
    }

    if (find_dir_entry(dst_vol, dst_parent, dst_name83, &dst_entry, &dst_sec_lba, &dst_idx) == 0) {
        if (dst_entry.attr & FAT_ATTR_DIRECTORY) {
            return -1;
        }
        if (dst_entry.fst_clus_lo >= 2) {
            if (free_cluster_chain(dst_vol, dst_entry.fst_clus_lo) != 0) {
                return -1;
            }
        }
    } else {
        if (find_free_dir_slot(dst_vol, dst_parent, &dst_sec_lba, &dst_idx) != 0) {
            return -1;
        }
    }

    for (i = 0; i < 11; i++) {
        src_entry.name[i] = dst_name83[i];
    }

    if (dst_sec_lba == src_sec_lba) {
        if (vol_rw_lba(dst_vol, 0, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        entries[src_idx].name[0] = 0xE5;
        entries[dst_idx] = src_entry;
        if (vol_rw_lba(dst_vol, 1, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
    } else {
        if (vol_rw_lba(dst_vol, 0, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        entries[dst_idx] = src_entry;
        if (vol_rw_lba(dst_vol, 1, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }

        if (vol_rw_lba(src_vol, 0, src_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        entries[src_idx].name[0] = 0xE5;
        if (vol_rw_lba(src_vol, 1, src_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int resolve_dir_cluster(os_volume_t *vol, const char __far *subpath, uint16_t *out_cluster, uint8_t is_vol_root) {
    uint16_t cur_clus;
    uint16_t pos = 0;
    char comp[16];

    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }
    if (!subpath || subpath[0] == '\0') {
        *out_cluster = vol->cwd_cluster;
        return 0;
    }

    if (is_sep(subpath[0]) || is_vol_root) {
        cur_clus = 0;
        while (is_sep(subpath[pos])) {
            pos++;
        }
    } else {
        cur_clus = vol->cwd_cluster;
    }

    while (subpath[pos] != '\0') {
        uint16_t k = 0;
        while (subpath[pos] != '\0' && !is_sep(subpath[pos])) {
            if (k >= 14) {
                return -1;
            }
            comp[k++] = subpath[pos++];
        }
        comp[k] = '\0';

        while (is_sep(subpath[pos])) {
            pos++;
        }

        if (step_into_dir(vol, cur_clus, comp, &cur_clus) != 0) {
            return -1;
        }
    }

    *out_cluster = cur_clus;
    return 0;
}

int fat16_dir_entry(const char __far *path, uint16_t index, os_dir_entry_t __far *out_info) {
    os_volume_t *vol;
    const char __far *subpath;
    uint16_t target_clus;
    uint16_t s, i;
    uint32_t sec_lba;
    uint16_t cur_idx = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (!fs_mounted || !out_info) {
        return -1;
    }
    if (vol_resolve_path(path, &vol, &subpath) != 0) {
        return -2;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -2;
    }
    if (vol_ensure_mounted(vol) != 0) {
        return -3;
    }
    if (resolve_dir_cluster(vol, subpath, &target_clus, (uint8_t)(path != subpath)) != 0) {
        return -2;
    }

    for (s = 0; dir_get_sector_lba(vol, target_clus, s, &sec_lba) == 0; s++) {
        if (vol_rw_lba(vol, 0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        for (i = 0; i < 16; i++) {
            uint8_t first = entries[i].name[0];
            if (first == 0x00) {
                return -1;
            }
            if (first == 0xE5 || first == '.' || (entries[i].attr & FAT_ATTR_VOLUME_ID)) {
                continue;
            }
            if (cur_idx == index) {
                char disp[13];
                int k;
                format_display_name(entries[i].name, disp);
                for (k = 0; k < 13; k++) {
                    out_info->name[k] = disp[k];
                }
                out_info->attr = entries[i].attr;
                out_info->size = entries[i].file_size;
                return 0;
            }
            cur_idx++;
        }
    }
    return -1;
}

int fat16_format_volume(const char __far *dev_name, const char __far *label, uint16_t fmt_flags) {
    os_volume_t *vol = 0;
    const char __far *subpath = 0;
    uint16_t cyls = 0, spt = 0;
    uint8_t heads = 0;
    uint8_t p_heads = 0, p_spt = 0, p_drives = 0;
    uint32_t start_lba = 0;
    uint32_t total_sec = 0;
    uint16_t tot_sec_16 = 0;
    uint32_t tot_sec_32 = 0;
    uint16_t tot_sec = 0;
    uint8_t spc = 2;
    uint16_t root_entries = 112;
    uint16_t sec_per_fat = 3;
    uint8_t media = 0xF9;
    uint8_t fat_type = FAT_TYPE_FAT12;
    uint8_t req_type = (uint8_t)(fmt_flags & 0x7FFF);
    uint8_t quick = (fmt_flags & FORMAT_FLAG_QUICK) ? 1 : 0;
    uint32_t fat1_lba, fat2_lba, root_lba;
    uint16_t root_sec, s;
    int i;
    char vol_label[11];

    if (!dev_name) {
        return -1;
    }
    if (vol_resolve_path(dev_name, &vol, &subpath) != 0) {
        return -1;
    }
    vol = resolve_vol_alias(vol);
    if (!vol || !vol->active) {
        return -1;
    }

    /* Format volume label: 11 bytes space-padded */
    for (i = 0; i < 11; i++) {
        vol_label[i] = ' ';
    }
    if (label) {
        for (i = 0; i < 11 && label[i] != '\0'; i++) {
            char c = label[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            vol_label[i] = c;
        }
    } else {
        const char *def_lbl = "HOBBYOS    ";
        for (i = 0; i < 11; i++) {
            vol_label[i] = def_lbl[i];
        }
    }

    if (vol->is_removable) {
        /* Determine Floppy Geometry */
        if (req_type == FORMAT_TYPE_AUTO) {
            int det = 0;
            bios_disk_reset(vol->bios_drive);
            /* Try reading existing sector 0 to detect inserted media type */
            if (bios_disk_rw_lba(vol->bios_drive, 0, 0, vol->sectors_per_track, vol->num_heads, MK_FP(KERNEL_SEG, io_buf)) == 0) {
                if (io_buf[510] == 0x55 && io_buf[511] == 0xAA) {
                    uint16_t b_tot = rd_le16(&io_buf[19]);
                    uint16_t b_spt = rd_le16(&io_buf[24]);
                    if (b_tot == 2880 || b_spt == 18) {
                        req_type = FORMAT_TYPE_1440;
                        det = 1;
                    } else if (b_tot == 2400 || b_spt == 15) {
                        req_type = FORMAT_TYPE_1200;
                        det = 1;
                    } else if (b_tot == 720 || (b_spt == 9 && io_buf[21] == 0xFD)) {
                        req_type = FORMAT_TYPE_360;
                        det = 1;
                    } else if (b_tot == 1440 || b_spt == 9) {
                        req_type = FORMAT_TYPE_720;
                        det = 1;
                    }
                }
            }
            if (!det) {
                /* Query BIOS drive parameters */
                if (bios_get_drive_params(vol->bios_drive, &cyls, &p_heads, &p_spt, &p_drives) == 0 && p_spt > 0 && p_heads > 0) {
                    if (p_spt == 18) {
                        req_type = FORMAT_TYPE_1440;
                    } else if (p_spt == 15) {
                        req_type = FORMAT_TYPE_1200;
                    } else if (p_spt == 9 && cyls <= 40 && cyls > 0) {
                        req_type = FORMAT_TYPE_360;
                    } else {
                        req_type = FORMAT_TYPE_720;
                    }
                } else if (vol->sectors_per_track == 18) {
                    req_type = FORMAT_TYPE_1440;
                } else {
                    req_type = FORMAT_TYPE_720;
                }
            }
        }

        /* Configure target geometry parameters */
        if (req_type == FORMAT_TYPE_1440) {
            /* 1.44 MB 3.5" Floppy */
            tot_sec = 2880;
            cyls = 80;
            heads = 2;
            spt = 18;
            spc = 1;
            root_entries = 224;
            sec_per_fat = 9;
            media = 0xF0;
            fat_type = FAT_TYPE_FAT12;
        } else if (req_type == FORMAT_TYPE_1200) {
            /* 1.2 MB 5.25" Floppy */
            tot_sec = 2400;
            cyls = 80;
            heads = 2;
            spt = 15;
            spc = 1;
            root_entries = 224;
            sec_per_fat = 7;
            media = 0xF9;
            fat_type = FAT_TYPE_FAT12;
        } else if (req_type == FORMAT_TYPE_360) {
            /* 360 KB 5.25" Floppy */
            tot_sec = 720;
            cyls = 40;
            heads = 2;
            spt = 9;
            spc = 2;
            root_entries = 112;
            sec_per_fat = 2;
            media = 0xFD;
            fat_type = FAT_TYPE_FAT12;
        } else {
            /* 720 KB 3.5" Floppy (Default) */
            tot_sec = 1440;
            cyls = 80;
            heads = 2;
            spt = 9;
            spc = 2;
            root_entries = 112;
            sec_per_fat = 3;
            media = 0xF9;
            fat_type = FAT_TYPE_FAT12;
        }

        start_lba = 0;
        vol->sectors_per_track = spt;
        vol->num_heads = heads;
        vol->total_sectors = tot_sec;
        tot_sec_16 = tot_sec;
        tot_sec_32 = 0;

        /* Program data transfer rate and parameter table on AT BIOS */
        bios_disk_reset(vol->bios_drive);
        bios_disk_set_media_type(vol->bios_drive, cyls, spt);

        /* Low-Level Format Tracks if requested */
        if (!quick) {
            uint16_t c;
            uint8_t h;
            for (c = 0; c < cyls; c++) {
                for (h = 0; h < heads; h++) {
                    int retry;
                    /* Build 4-byte address fields for sectors 1..spt */
                    for (s = 1; s <= spt; s++) {
                        uint16_t off = (uint16_t)((s - 1) * 4);
                        io_buf[off + 0] = (uint8_t)c;
                        io_buf[off + 1] = h;
                        io_buf[off + 2] = (uint8_t)s;
                        io_buf[off + 3] = 2; /* 512 bytes */
                    }
                    for (retry = 0; retry < 3; retry++) {
                        if (bios_disk_format_track(vol->bios_drive, c, h, MK_FP(KERNEL_SEG, io_buf)) == 0) {
                            break;
                        }
                        bios_disk_reset(vol->bios_drive);
                        bios_disk_set_media_type(vol->bios_drive, cyls, spt);
                    }
                    if (retry == 3) {
                        if (c == 0 && h == 0) {
                            /* Fall back gracefully if controller doesn't support track format */
                            c = cyls;
                            break;
                        }
                        return -1;
                    }
                }
            }
        }
    } else {
        /* Hard Disk Partition */
        start_lba = vol->start_lba;
        spt = vol->sectors_per_track;
        heads = (uint8_t)vol->num_heads;
        total_sec = vol->total_sectors;

        /* If volume is an MBR partition, check MBR partition table (LBA 0)
         * to obtain the true partition size in case it was previously clamped
         * or formatted to a smaller size, and ensure bootability.
         */
        if (vol->part_idx < 4) {
            if (bios_disk_rw_lba(vol->bios_drive, 0, 0, spt, heads, MK_FP(KERNEL_SEG, io_buf)) == 0) {
                mbr_partition_t *parts = (mbr_partition_t *)&io_buf[0x01BE];
                int mbr_dirty = 0;
                int p, has_active = 0;

                if (parts[vol->part_idx].total_sectors > 0) {
                    total_sec = parts[vol->part_idx].total_sectors;
                    /* If FAT16B (> 65535 sectors) and MBR type was 0x04, update to 0x06 (BigDOS) */
                    if (total_sec > 65535UL && parts[vol->part_idx].part_type == 0x04) {
                        parts[vol->part_idx].part_type = 0x06;
                        mbr_dirty = 1;
                    }
                }

                /* Ensure partition is marked active (0x80) in MBR if no partition is active */
                for (p = 0; p < 4; p++) {
                    if (parts[p].boot_indicator == 0x80) {
                        has_active = 1;
                        break;
                    }
                }
                if (!has_active) {
                    parts[vol->part_idx].boot_indicator = 0x80;
                    mbr_dirty = 1;
                }

                if (mbr_dirty) {
                    bios_disk_rw_lba(vol->bios_drive, 1, 0, spt, heads, MK_FP(KERNEL_SEG, io_buf));
                }
            }
        }

        if (total_sec == 0) {
            return -1;
        }
        media = 0xF8;

        if (total_sec < 8192UL) {
            /* Small partition: FAT12 */
            fat_type = FAT_TYPE_FAT12;
            spc = 2;
            root_entries = 224;
            sec_per_fat = 6;
            tot_sec_16 = (uint16_t)total_sec;
            tot_sec_32 = 0;
        } else {
            /* Standard partition: FAT16 / FAT16B */
            uint16_t shift = 2;
            uint32_t max_clusters;
            uint32_t fat_bytes;

            fat_type = FAT_TYPE_FAT16;
            root_entries = 512;
            spc = 4; /* 2 KB clusters */

            /* Scale SPC so total clusters <= 65520 to fit within 16-bit FAT limit */
            while (spc < 64 && (uint32_t)(total_sec >> shift) > 65520UL) {
                spc = (uint8_t)(spc * 2);
                shift++;
            }

            max_clusters = total_sec >> shift;
            if (max_clusters > 65520UL) {
                max_clusters = 65520UL;
            }
            fat_bytes = ((uint32_t)(max_clusters + 2)) << 1;
            sec_per_fat = (uint16_t)((fat_bytes + 511) >> 9);
            if (sec_per_fat == 0) sec_per_fat = 1;

            if (total_sec <= 65535UL) {
                tot_sec_16 = (uint16_t)total_sec;
                tot_sec_32 = 0;
            } else {
                tot_sec_16 = 0;
                tot_sec_32 = total_sec;
            }
        }
        vol->total_sectors = total_sec;
    }

    /* Invalidate cached FAT */
    cached_fat_vol = 0xFF;
    cached_fat_sec = 0xFFFF;
    fat_dirty = 0;

    /* 1. Build & Write Sector 0 (VBR with HobbyOS bootblock) */
    for (i = 0; i < 512; i++) {
        io_buf[i] = default_boot_sector[i];
    }
    io_buf[0] = 0xEB;
    io_buf[1] = 0x3C;
    io_buf[2] = 0x90;
    io_buf[3] = 'H'; io_buf[4] = 'O'; io_buf[5] = 'B'; io_buf[6] = 'B';
    io_buf[7] = 'Y'; io_buf[8] = 'O'; io_buf[9] = 'S'; io_buf[10] = ' ';

    wr_le16(&io_buf[11], 512);
    io_buf[13] = spc;
    wr_le16(&io_buf[14], 1);          /* Reserved sectors = 1 */
    io_buf[16] = 2;                  /* Num FATs = 2 */
    wr_le16(&io_buf[17], root_entries);
    wr_le16(&io_buf[19], tot_sec_16);
    io_buf[21] = media;
    wr_le16(&io_buf[22], sec_per_fat);
    wr_le16(&io_buf[24], spt);
    wr_le16(&io_buf[26], heads);
    wr_le32(&io_buf[28], start_lba);  /* Hidden sectors */
    wr_le32(&io_buf[32], tot_sec_32); /* Total sectors 32 (FAT16B) */

    io_buf[36] = vol->bios_drive;
    io_buf[37] = 0;
    io_buf[38] = 0x29;                /* Extended boot signature */
    wr_le32(&io_buf[39], 0x19800101); /* Volume ID */

    for (i = 0; i < 11; i++) {
        io_buf[43 + i] = vol_label[i];
    }
    if (fat_type == FAT_TYPE_FAT12) {
        io_buf[54] = 'F'; io_buf[55] = 'A'; io_buf[56] = 'T';
        io_buf[57] = '1'; io_buf[58] = '2'; io_buf[59] = ' ';
        io_buf[60] = ' '; io_buf[61] = ' ';
    } else {
        io_buf[54] = 'F'; io_buf[55] = 'A'; io_buf[56] = 'T';
        io_buf[57] = '1'; io_buf[58] = '6'; io_buf[59] = ' ';
        io_buf[60] = ' '; io_buf[61] = ' ';
    }
    io_buf[510] = 0x55;
    io_buf[511] = 0xAA;

    if (bios_disk_rw_lba(vol->bios_drive, 1, start_lba, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    /* 2. Write FAT1 & FAT2 */
    fat1_lba = start_lba + 1;
    fat2_lba = fat1_lba + sec_per_fat;

    /* First sector of FAT: media byte + 0xFF markers */
    for (i = 0; i < 512; i++) {
        io_buf[i] = 0;
    }
    io_buf[0] = media;
    io_buf[1] = 0xFF;
    io_buf[2] = 0xFF;
    if (fat_type == FAT_TYPE_FAT16) {
        io_buf[3] = 0xFF;
    }
    if (bios_disk_rw_lba(vol->bios_drive, 1, fat1_lba, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) return -1;
    if (bios_disk_rw_lba(vol->bios_drive, 1, fat2_lba, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) return -1;

    /* Subsequent FAT sectors (if any): all zeros */
    for (i = 0; i < 512; i++) {
        io_buf[i] = 0;
    }
    for (s = 1; s < sec_per_fat; s++) {
        if (bios_disk_rw_lba(vol->bios_drive, 1, fat1_lba + s, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) return -1;
        if (bios_disk_rw_lba(vol->bios_drive, 1, fat2_lba + s, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) return -1;
    }

    /* 3. Write Root Directory */
    root_lba = fat2_lba + sec_per_fat;
    root_sec = (root_entries * 32) >> 9;

    /* Sector 0 of Root Directory: optional Volume Label entry */
    for (i = 0; i < 512; i++) {
        io_buf[i] = 0;
    }
    if (label) {
        for (i = 0; i < 11; i++) {
            io_buf[i] = vol_label[i];
        }
        io_buf[11] = FAT_ATTR_VOLUME_ID;
    }
    if (bios_disk_rw_lba(vol->bios_drive, 1, root_lba, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) return -1;

    /* Remaining root sectors: all zeros */
    for (i = 0; i < 512; i++) {
        io_buf[i] = 0;
    }
    for (s = 1; s < root_sec; s++) {
        if (bios_disk_rw_lba(vol->bios_drive, 1, root_lba + s, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) return -1;
    }

    /* 4. Reload VBR into vol and update in-memory state */
    if (bios_disk_rw_lba(vol->bios_drive, 0, start_lba, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    if (parse_bpb(vol, io_buf) != 0) {
        return -1;
    }
    vol->is_mounted  = 1;
    vol->cwd_cluster = 0;
    vol->cwd_path[0] = '/';
    vol->cwd_path[1] = '\0';

    return 0;
}

/* --- Fixed Disk Partition & Geometry Management --- */

int disk_get_geom(uint8_t drive, os_disk_geom_t __far *out_geom) {
    uint16_t cyls = 0;
    uint8_t heads = 0;
    uint8_t spt = 0;
    uint8_t drives = 0;
    uint16_t track_sec;
    uint32_t acc = 0;
    uint16_t c;

    if (!out_geom) return -1;
    if (bios_get_drive_params(drive, &cyls, &heads, &spt, &drives) != 0) {
        return -1;
    }
    if (spt == 0 || heads == 0 || cyls == 0) {
        return -1;
    }

    track_sec = (uint16_t)heads * spt;
    for (c = 0; c < cyls; c++) {
        acc += track_sec;
    }

    out_geom->bios_drive    = drive;
    out_geom->num_drives    = drives;
    out_geom->cyls          = cyls;
    out_geom->heads         = heads;
    out_geom->spt           = spt;
    out_geom->total_sectors = acc;
    return 0;
}

int disk_part_read(uint8_t drive, os_part_table_t __far *out_table) {
    uint16_t cyls = 0;
    uint8_t heads = 0, spt = 0, drives = 0;
    mbr_partition_t *parts;
    int i, p;

    if (!out_table) return -1;
    if (bios_get_drive_params(drive, &cyls, &heads, &spt, &drives) != 0) {
        return -1;
    }
    if (bios_disk_rw_lba(drive, 0, 0, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    out_table->is_valid_mbr = (io_buf[510] == 0x55 && io_buf[511] == 0xAA) ? 1 : 0;
    out_table->has_boot_code = 0;
    for (i = 0; i < 446; i++) {
        if (io_buf[i] != 0x00) {
            out_table->has_boot_code = 1;
            break;
        }
    }

    parts = (mbr_partition_t *)&io_buf[0x01BE];
    for (p = 0; p < 4; p++) {
        out_table->parts[p].boot_indicator = parts[p].boot_indicator;
        out_table->parts[p].start_head     = parts[p].start_head;
        out_table->parts[p].start_sector   = parts[p].start_sector;
        out_table->parts[p].start_cylinder = parts[p].start_cylinder;
        out_table->parts[p].part_type      = parts[p].part_type;
        out_table->parts[p].end_head       = parts[p].end_head;
        out_table->parts[p].end_sector     = parts[p].end_sector;
        out_table->parts[p].end_cylinder   = parts[p].end_cylinder;
        out_table->parts[p].start_lba      = parts[p].start_lba;
        out_table->parts[p].total_sectors  = parts[p].total_sectors;
    }
    return 0;
}

int disk_part_write(uint8_t drive, const os_part_table_t __far *in_table, uint16_t write_mbr_code) {
    uint16_t cyls = 0;
    uint8_t heads = 0, spt = 0, drives = 0;
    mbr_partition_t *parts;
    int i, p;

    if (!in_table) return -1;
    if (bios_get_drive_params(drive, &cyls, &heads, &spt, &drives) != 0) {
        return -1;
    }

    /* Read existing Sector 0 to preserve existing boot code if write_mbr_code is 0 */
    if (bios_disk_rw_lba(drive, 0, 0, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        for (i = 0; i < 512; i++) {
            io_buf[i] = 0;
        }
        write_mbr_code = 1;
    }

    if (write_mbr_code) {
        for (i = 0; i < 446; i++) {
            io_buf[i] = default_mbr_code[i];
        }
    } else {
        int has_code = 0;
        for (i = 0; i < 446; i++) {
            if (io_buf[i] != 0) {
                has_code = 1;
                break;
            }
        }
        if (!has_code) {
            for (i = 0; i < 446; i++) {
                io_buf[i] = default_mbr_code[i];
            }
        }
    }

    parts = (mbr_partition_t *)&io_buf[0x01BE];
    for (p = 0; p < 4; p++) {
        parts[p].boot_indicator = in_table->parts[p].boot_indicator;
        parts[p].start_head     = in_table->parts[p].start_head;
        parts[p].start_sector   = in_table->parts[p].start_sector;
        parts[p].start_cylinder = in_table->parts[p].start_cylinder;
        parts[p].part_type      = in_table->parts[p].part_type;
        parts[p].end_head       = in_table->parts[p].end_head;
        parts[p].end_sector     = in_table->parts[p].end_sector;
        parts[p].end_cylinder   = in_table->parts[p].end_cylinder;
        parts[p].start_lba      = in_table->parts[p].start_lba;
        parts[p].total_sectors  = in_table->parts[p].total_sectors;
    }

    io_buf[510] = 0x55;
    io_buf[511] = 0xAA;

    if (bios_disk_rw_lba(drive, 1, 0, spt, heads, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    fat16_rescan_drives();
    return 0;
}

int ramdisk_mount(uint8_t type, uint16_t size_kb, uint16_t __far *out_size_kb) {
    uint16_t total_sectors;
    uint8_t spc;
    uint16_t root_entries;
    uint16_t sectors_per_fat;
    uint16_t fat_bytes;
    uint16_t root_sectors;
    uint16_t root_lba;
    uint16_t data_lba;
    uint16_t total_clusters;
    os_volume_t *vol;
    uint16_t s;
    int i;
    int vol_idx;

    if (ramdisk_info.active) {
        return -4; /* Already mounted */
    }

    if (type != RAMDISK_TYPE_EMS && type != RAMDISK_TYPE_XMS) {
        return -1;
    }

    if (type == RAMDISK_TYPE_EMS) {
        uint16_t avail_pages;
        uint16_t needed_pages;

        if (ems_total_pages() == 0) {
            return -2; /* EMS not present */
        }
        avail_pages = ems_avail_pages();
        if (size_kb == 0) {
            needed_pages = avail_pages;
            if (needed_pages == 0) {
                return -3;
            }
            size_kb = needed_pages * 16;
        } else {
            needed_pages = (size_kb + 15) / 16;
            if (needed_pages == 0 || needed_pages > avail_pages || needed_pages > 128) {
                return -3;
            }
            size_kb = needed_pages * 16;
        }

        for (i = 0; i < (int)needed_pages; i++) {
            int p = ems_alloc_page(KERNEL_SEG);
            if (p < 0) {
                int j;
                for (j = 0; j < i; j++) {
                    ems_free_page(ramdisk_info.ems_pages[j]);
                }
                return -3;
            }
            ramdisk_info.ems_pages[i] = (uint8_t)p;
        }
        ramdisk_info.ems_num_pages = needed_pages;
        ramdisk_info.xms_handle = 0;
    } else {
        uint16_t max_free;
        uint16_t h;

        if (!xms_is_present()) {
            return -2; /* XMS not present */
        }
        max_free = xms_largest_free_kb();
        if (size_kb == 0) {
            size_kb = max_free;
            if (size_kb < 16) {
                return -3;
            }
        } else {
            if (size_kb < 16 || size_kb > max_free) {
                return -3;
            }
        }
        h = xms_alloc(size_kb, KERNEL_SEG);
        if (h == 0) {
            return -3;
        }
        ramdisk_info.xms_handle = h;
        ramdisk_info.ems_num_pages = 0;
    }

    ramdisk_info.active = 1;
    ramdisk_info.type = type;
    ramdisk_info.size_kb = size_kb;
    ramdisk_info.total_sectors = size_kb * 2;

    fat_flush();
    cached_fat_vol = 0xFF;
    cached_fat_sec = 0xFFFF;
    fat_dirty = 0;

    total_sectors = ramdisk_info.total_sectors;
    spc = (size_kb <= 2048) ? 1 : ((size_kb <= 4096) ? 2 : 4);
    root_entries = (size_kb <= 256) ? 64 : 128;
    root_sectors = (root_entries * 32) / 512;
    fat_bytes = (total_sectors + 2) + ((total_sectors + 2) >> 1);
    sectors_per_fat = (fat_bytes + 511) >> 9;
    root_lba = 1 + sectors_per_fat;
    data_lba = root_lba + root_sectors;
    total_clusters = (total_sectors - data_lba) / spc;

    /* Write VBR (Sector 0) */
    for (i = 0; i < 512; i++) io_buf[i] = 0;
    io_buf[0] = 0xEB; io_buf[1] = 0x3C; io_buf[2] = 0x90;
    io_buf[3] = 'H'; io_buf[4] = 'O'; io_buf[5] = 'B'; io_buf[6] = 'B';
    io_buf[7] = 'Y'; io_buf[8] = 'O'; io_buf[9] = 'S'; io_buf[10] = ' ';
    wr_le16(&io_buf[11], 512);
    io_buf[13] = spc;
    wr_le16(&io_buf[14], 1);
    io_buf[16] = 1;
    wr_le16(&io_buf[17], root_entries);
    wr_le16(&io_buf[19], total_sectors);
    io_buf[21] = 0xF8;
    wr_le16(&io_buf[22], sectors_per_fat);
    wr_le16(&io_buf[24], 32);
    wr_le16(&io_buf[26], 1);
    wr_le32(&io_buf[28], 0);
    wr_le32(&io_buf[32], 0);
    io_buf[36] = 0x80;
    io_buf[37] = 0;
    io_buf[38] = 0x29;
    wr_le32(&io_buf[39], 0x52414D44);
    for (i = 0; i < 11; i++) io_buf[43 + i] = "RAM DISK   "[i];
    for (i = 0; i < 8; i++)  io_buf[54 + i] = "FAT12   "[i];
    io_buf[510] = 0x55;
    io_buf[511] = 0xAA;
    ramdisk_rw(1, 0, io_buf);

    /* Write FAT Sector 0 (Sector 1) */
    for (i = 0; i < 512; i++) io_buf[i] = 0;
    io_buf[0] = 0xF8;
    io_buf[1] = 0xFF;
    io_buf[2] = 0xFF;
    ramdisk_rw(1, 1, io_buf);

    /* Clear remaining FAT sectors */
    for (i = 0; i < 512; i++) io_buf[i] = 0;
    for (s = 2; s <= sectors_per_fat; s++) {
        ramdisk_rw(1, s, io_buf);
    }

    /* Write Root Directory Sector 0 with Volume Label */
    for (i = 0; i < 11; i++) io_buf[i] = "RAM DISK   "[i];
    io_buf[11] = 0x08; /* Volume Label */
    ramdisk_rw(1, root_lba, io_buf);

    /* Clear remaining Root Directory sectors */
    for (i = 0; i < 512; i++) io_buf[i] = 0;
    for (s = 1; s < root_sectors; s++) {
        ramdisk_rw(1, root_lba + s, io_buf);
    }

    /* Find volume slot */
    vol_idx = -1;
    for (i = 0; i < MAX_VOLUMES; i++) {
        if (!volumes[i].active) {
            vol_idx = i;
            break;
        }
    }
    if (vol_idx < 0) {
        ramdisk_unmount();
        return -5;
    }

    vol = &volumes[vol_idx];
    vol->name[0] = 'R'; vol->name[1] = 'A'; vol->name[2] = 'M'; vol->name[3] = '\0';
    vol->active = 1;
    vol->is_alias = 0;
    vol->target_vol_idx = 0;
    vol->bios_drive = 0xFF;
    vol->part_idx = 0xFF;
    vol->is_removable = 0;
    vol->is_mounted = 1;
    vol->fat_type = FAT_TYPE_FAT12;
    vol->sectors_per_track = 32;
    vol->num_heads = 1;
    vol->start_lba = 0;
    vol->total_sectors = total_sectors;
    vol->bytes_per_sector = 512;
    vol->sectors_per_cluster = spc;
    vol->reserved_sectors = 1;
    vol->num_fats = 1;
    vol->root_entries = root_entries;
    vol->sectors_per_fat = sectors_per_fat;
    vol->fat_lba = 1;
    vol->root_lba = root_lba;
    vol->root_sectors = root_sectors;
    vol->data_lba = data_lba;
    vol->total_clusters = total_clusters;
    vol->cwd_cluster = 0;
    vol->cwd_path[0] = '/'; vol->cwd_path[1] = '\0';

    if (vol_idx >= num_volumes) {
        num_volumes = (uint8_t)(vol_idx + 1);
    }

    if (out_size_kb) {
        *out_size_kb = size_kb;
    }
    return 0;
}

int ramdisk_unmount(void) {
    int i;
    if (!ramdisk_info.active) {
        return -1;
    }

    fat_flush();
    cached_fat_vol = 0xFF;
    cached_fat_sec = 0xFFFF;
    fat_dirty = 0;

    if (ramdisk_info.type == RAMDISK_TYPE_EMS) {
        for (i = 0; i < (int)ramdisk_info.ems_num_pages; i++) {
            ems_free_page(ramdisk_info.ems_pages[i]);
        }
        ramdisk_info.ems_num_pages = 0;
    } else if (ramdisk_info.type == RAMDISK_TYPE_XMS) {
        if (ramdisk_info.xms_handle != 0) {
            xms_free(ramdisk_info.xms_handle);
            ramdisk_info.xms_handle = 0;
        }
    }
    ramdisk_info.active = 0;

    for (i = 0; i < MAX_VOLUMES; i++) {
        if (volumes[i].active && volumes[i].bios_drive == 0xFF) {
            volumes[i].active = 0;
            volumes[i].is_mounted = 0;
            volumes[i].name[0] = '\0';
            if (current_vol_idx == (uint8_t)i) {
                int s;
                for (s = 0; s < MAX_VOLUMES; s++) {
                    if (volumes[s].active && (vol_name_match(volumes[s].name, "SYS") ||
                                              vol_name_match(volumes[s].name, "HD0"))) {
                        current_vol_idx = (uint8_t)s;
                        break;
                    }
                }
            }
            break;
        }
    }

    return 0;
}

int ramdisk_status(os_ramdisk_status_t __far *status) {
    if (!ramdisk_info.active) {
        if (status) {
            status->active = 0;
            status->type = 0;
            status->size_kb = 0;
            status->total_clusters = 0;
            status->free_clusters = 0;
            status->bytes_per_cluster = 0;
        }
        return 0;
    }
    if (status) {
        int i;
        os_volume_t *vol = 0;
        status->active = 1;
        status->type = ramdisk_info.type;
        status->size_kb = ramdisk_info.size_kb;
        for (i = 0; i < MAX_VOLUMES; i++) {
            if (volumes[i].active && volumes[i].bios_drive == 0xFF) {
                vol = &volumes[i];
                break;
            }
        }
        if (vol) {
            status->total_clusters = vol->total_clusters;
            status->free_clusters = vol_get_free_clusters(vol);
            status->bytes_per_cluster = (uint16_t)vol->sectors_per_cluster * 512;
        } else {
            status->total_clusters = 0;
            status->free_clusters = 0;
            status->bytes_per_cluster = 512;
        }
    }
    return 1;
}
