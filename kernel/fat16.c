/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FAT16 hierarchical file system driver (subdirectories, CWD, path resolution,
 * read, write, create, delete, mkdir, rmdir, chdir, directory listing, and
 * executable loading).
 */

#include "kernel.h"

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
#pragma pack(pop)

#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20

static uint16_t bpb_bytes_per_sector    = 512;
static uint8_t  bpb_sectors_per_cluster = 4;
static uint16_t bpb_reserved_sectors    = 1;
static uint8_t  bpb_num_fats            = 2;
static uint16_t bpb_root_entries        = 512;
static uint16_t bpb_total_sectors       = 20808;
static uint16_t bpb_sectors_per_fat     = 21;
static uint16_t bpb_sectors_per_track   = 17;
static uint16_t bpb_num_heads           = 4;
static uint16_t bpb_hidden_sectors      = 0;

static uint16_t fat_lba        = 1;
static uint16_t root_lba       = 43;
static uint16_t root_sectors   = 32;
static uint16_t data_lba       = 75;
static uint16_t total_clusters = 5183;
static uint8_t  fs_mounted     = 0;

static uint16_t cwd_cluster    = 0; /* 0 = Root Directory (/) */
static char     cwd_path[64]   = "/";

static uint8_t  io_buf[512]    = {0};
static uint8_t  fat_buf[512]   = {0};
static uint16_t cached_fat_sec = 0xFFFF;
static uint8_t  fat_dirty      = 0;

static int disk_rw_lba(uint8_t is_write, uint16_t lba, void __far *buf) {
    uint16_t track = lba / bpb_sectors_per_track;
    uint16_t sec   = (lba % bpb_sectors_per_track) + 1;
    uint16_t cyl   = track / bpb_num_heads;
    uint16_t head  = track % bpb_num_heads;
    return bios_disk_rw(is_write, cyl, head, sec, buf);
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int fat_flush(void) {
    uint8_t f;
    if (!fat_dirty || cached_fat_sec == 0xFFFF) {
        return 0;
    }
    for (f = 0; f < bpb_num_fats; f++) {
        uint16_t lba = fat_lba + (uint16_t)f * bpb_sectors_per_fat + cached_fat_sec;
        if (disk_rw_lba(1, lba, MK_FP(KERNEL_SEG, fat_buf)) != 0) {
            return -1;
        }
    }
    fat_dirty = 0;
    return 0;
}

static int fat_load_sector(uint16_t fat_sec_idx) {
    if (cached_fat_sec == fat_sec_idx) {
        return 0;
    }
    if (fat_flush() != 0) {
        return -1;
    }
    if (disk_rw_lba(0, fat_lba + fat_sec_idx, MK_FP(KERNEL_SEG, fat_buf)) != 0) {
        return -1;
    }
    cached_fat_sec = fat_sec_idx;
    return 0;
}

static uint16_t fat_get_cluster(uint16_t cluster) {
    uint16_t sec_idx = cluster >> 8;
    uint16_t off     = (cluster & 0xFF) << 1;
    if (fat_load_sector(sec_idx) != 0) {
        return 0xFFFF;
    }
    return (uint16_t)fat_buf[off] | ((uint16_t)fat_buf[off + 1] << 8);
}

static int fat_set_cluster(uint16_t cluster, uint16_t val) {
    uint16_t sec_idx = cluster >> 8;
    uint16_t off     = (cluster & 0xFF) << 1;
    if (fat_load_sector(sec_idx) != 0) {
        return -1;
    }
    fat_buf[off]     = (uint8_t)(val & 0xFF);
    fat_buf[off + 1] = (uint8_t)(val >> 8);
    fat_dirty = 1;
    return 0;
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

/* Convert a single path component ("foo.txt", ".", "..") into 11-byte 8.3 name */
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

/* Map the sec_idx-th sector of directory dir_cluster (0 = Root, >=2 = Subdir) to LBA */
static int dir_get_sector_lba(uint16_t dir_cluster, uint16_t sec_idx, uint16_t *out_lba) {
    if (dir_cluster == 0) {
        if (sec_idx >= root_sectors) {
            return -1;
        }
        *out_lba = root_lba + sec_idx;
        return 0;
    } else {
        uint16_t clus_steps = sec_idx / bpb_sectors_per_cluster;
        uint16_t sec_offset = sec_idx % bpb_sectors_per_cluster;
        uint16_t cur        = dir_cluster;
        while (clus_steps > 0) {
            if (cur < 2 || cur >= 0xFFF8) {
                return -1;
            }
            cur = fat_get_cluster(cur);
            clus_steps--;
        }
        if (cur < 2 || cur >= 0xFFF8) {
            return -1;
        }
        *out_lba = data_lba + (cur - 2) * bpb_sectors_per_cluster + sec_offset;
        return 0;
    }
}

int fat16_init(void) {
    if (disk_rw_lba(0, 0, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    if (io_buf[510] != 0x55 || io_buf[511] != 0xAA) {
        return -1;
    }

    bpb_bytes_per_sector    = rd_le16(&io_buf[11]);
    bpb_sectors_per_cluster = io_buf[13];
    bpb_reserved_sectors    = rd_le16(&io_buf[14]);
    bpb_num_fats            = io_buf[16];
    bpb_root_entries        = rd_le16(&io_buf[17]);
    bpb_total_sectors       = rd_le16(&io_buf[19]);
    bpb_sectors_per_fat     = rd_le16(&io_buf[22]);
    bpb_sectors_per_track   = rd_le16(&io_buf[24]);
    bpb_num_heads           = rd_le16(&io_buf[26]);
    bpb_hidden_sectors      = rd_le16(&io_buf[28]);

    if (bpb_bytes_per_sector != 512 || bpb_sectors_per_cluster == 0) {
        return -1;
    }

    fat_lba        = bpb_reserved_sectors + bpb_hidden_sectors;
    root_lba       = fat_lba + (uint16_t)bpb_num_fats * bpb_sectors_per_fat;
    root_sectors   = (bpb_root_entries * 32) / 512;
    data_lba       = root_lba + root_sectors;
    total_clusters = (bpb_total_sectors - data_lba) / bpb_sectors_per_cluster;
    cached_fat_sec = 0xFFFF;
    fat_dirty      = 0;
    cwd_cluster    = 0;
    cwd_path[0]    = '/';
    cwd_path[1]    = '\0';
    fs_mounted     = 1;
    return 0;
}

static int find_dir_entry(uint16_t dir_cluster, const uint8_t name83[11],
                          fat16_dirent_t *out_entry, uint16_t *out_sec_lba, uint16_t *out_idx) {
    uint16_t s, i, sec_lba;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (!fs_mounted) {
        return -1;
    }

    for (s = 0; dir_get_sector_lba(dir_cluster, s, &sec_lba) == 0; s++) {
        if (disk_rw_lba(0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
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

static uint16_t alloc_free_cluster(uint16_t start_search) {
    uint16_t c;
    if (start_search < 2) {
        start_search = 2;
    }
    for (c = start_search; c < total_clusters + 2; c++) {
        if (fat_get_cluster(c) == 0x0000) {
            if (fat_set_cluster(c, 0xFFFF) != 0) {
                return 0;
            }
            return c;
        }
    }
    return 0;
}

static int zero_cluster_sectors(uint16_t clus) {
    uint16_t first_lba = data_lba + (clus - 2) * bpb_sectors_per_cluster;
    uint16_t i;
    uint8_t s;
    for (i = 0; i < 512; i++) {
        io_buf[i] = 0;
    }
    for (s = 0; s < bpb_sectors_per_cluster; s++) {
        if (disk_rw_lba(1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
    }
    return 0;
}

static int find_free_dir_slot(uint16_t dir_cluster, uint16_t *out_sec_lba, uint16_t *out_idx) {
    uint16_t s, i, sec_lba;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    for (s = 0; dir_get_sector_lba(dir_cluster, s, &sec_lba) == 0; s++) {
        if (disk_rw_lba(0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
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

    /* If a subdirectory is full, extend its cluster chain by 1 cluster */
    if (dir_cluster >= 2) {
        uint16_t cur = dir_cluster;
        uint16_t new_clus;
        while ( cur >= 2 ) {
            uint16_t next = fat_get_cluster(cur);
            if (next >= 0xFFF8 || next < 2) {
                break;
            }
            cur = next;
        }
        new_clus = alloc_free_cluster(2);
        if (new_clus == 0) {
            return -1;
        }
        if (fat_set_cluster(cur, new_clus) != 0 || fat_flush() != 0) {
            return -1;
        }
        if (zero_cluster_sectors(new_clus) != 0) {
            return -1;
        }
        *out_sec_lba = data_lba + (new_clus - 2) * bpb_sectors_per_cluster;
        *out_idx     = 0;
        return 0;
    }

    return -1;
}

static int free_cluster_chain(uint16_t start_cluster) {
    uint16_t cur = start_cluster;
    while (cur >= 2 && cur < 0xFFF8) {
        uint16_t next = fat_get_cluster(cur);
        if (fat_set_cluster(cur, 0x0000) != 0) {
            return -1;
        }
        cur = next;
    }
    return fat_flush();
}

/* Step from dir_cluster into child directory component `comp` */
static int step_into_dir(uint16_t cur_cluster, const char *comp, uint16_t *out_next_cluster) {
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
        if (find_dir_entry(cur_cluster, name83, &ent, 0, 0) != 0) {
            *out_next_cluster = 0;
            return 0;
        }
        *out_next_cluster = ent.fst_clus_lo;
        return 0;
    }

    if (make_83_component(comp, name83) != 0) {
        return -1;
    }
    if (find_dir_entry(cur_cluster, name83, &ent, 0, 0) != 0) {
        return -1;
    }
    if ((ent.attr & FAT_ATTR_DIRECTORY) == 0) {
        return -1;
    }
    *out_next_cluster = ent.fst_clus_lo;
    return 0;
}

/*
 * Resolve a path into its parent directory cluster and 11-byte 8.3 leaf name.
 * Supports '/', '\', absolute paths, relative paths, '.', and '..'.
 */
static int resolve_parent_and_leaf(const char __far *path, uint16_t *out_parent_clus, uint8_t out_leaf83[11]) {
    uint16_t cur_clus;
    uint16_t pos = 0;
    char comp[16];

    if (!fs_mounted || !path || path[0] == '\0') {
        return -1;
    }

    if (is_sep(path[0])) {
        cur_clus = 0;
        while (is_sep(path[pos])) {
            pos++;
        }
    } else {
        cur_clus = cwd_cluster;
    }

    if (path[pos] == '\0') {
        return -1;
    }

    for (;;) {
        uint16_t k = 0;
        while (path[pos] != '\0' && !is_sep(path[pos])) {
            if (k >= 14) {
                return -1;
            }
            comp[k++] = path[pos++];
        }
        comp[k] = '\0';

        while (is_sep(path[pos])) {
            pos++;
        }

        if (path[pos] == '\0') {
            /* Final component */
            if (make_83_component(comp, out_leaf83) != 0) {
                return -1;
            }
            *out_parent_clus = cur_clus;
            return 0;
        }

        /* Intermediate directory component */
        if (step_into_dir(cur_clus, comp, &cur_clus) != 0) {
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

int fat16_chdir(const char __far *path) {
    uint16_t cur_clus;
    uint16_t pos = 0;
    char new_path[64];
    char comp[16];
    int i;

    if (!fs_mounted || !path || path[0] == '\0') {
        return -1;
    }

    if (is_sep(path[0])) {
        cur_clus    = 0;
        new_path[0] = '/';
        new_path[1] = '\0';
        while (is_sep(path[pos])) {
            pos++;
        }
    } else {
        cur_clus = cwd_cluster;
        for (i = 0; i < 64; i++) {
            new_path[i] = cwd_path[i];
            if (cwd_path[i] == '\0') break;
        }
    }

    while (path[pos] != '\0') {
        uint16_t k = 0;
        while (path[pos] != '\0' && !is_sep(path[pos])) {
            if (k >= 14) {
                return -1;
            }
            comp[k++] = path[pos++];
        }
        comp[k] = '\0';

        while (is_sep(path[pos])) {
            pos++;
        }

        if (comp[0] == '.' && comp[1] == '\0') {
            continue;
        }
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (step_into_dir(cur_clus, "..", &cur_clus) != 0) {
                return -1;
            }
            canon_pop(new_path);
            continue;
        }

        if (step_into_dir(cur_clus, comp, &cur_clus) != 0) {
            return -1;
        }
        canon_push(new_path, comp);
    }

    cwd_cluster = cur_clus;
    for (i = 0; i < 64; i++) {
        cwd_path[i] = new_path[i];
        if (new_path[i] == '\0') break;
    }
    return 0;
}

int fat16_getcwd(char __far *buf, uint16_t max_len) {
    uint16_t i = 0;
    if (!buf || max_len == 0) {
        return -1;
    }
    while (cwd_path[i] != '\0' && i + 1 < max_len) {
        buf[i] = cwd_path[i];
        i++;
    }
    buf[i] = '\0';
    return 0;
}

int fat16_mkdir(const char __far *path) {
    uint16_t parent_clus;
    uint8_t name83[11];
    uint8_t dot83[11];
    uint16_t dir_sec_lba, dir_idx;
    uint16_t new_clus;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (resolve_parent_and_leaf(path, &parent_clus, name83) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }
    if (find_dir_entry(parent_clus, name83, 0, 0, 0) == 0) {
        return -1; /* Already exists */
    }
    if (find_free_dir_slot(parent_clus, &dir_sec_lba, &dir_idx) != 0) {
        return -1;
    }

    new_clus = alloc_free_cluster(2);
    if (new_clus == 0) {
        return -1;
    }
    if (fat_flush() != 0) {
        return -1;
    }
    if (zero_cluster_sectors(new_clus) != 0) {
        return -1;
    }

    /* Populate '.' and '..' entries in sector 0 of new_clus */
    make_83_component(".", dot83);
    fill_dirent_meta(&entries[0], dot83, FAT_ATTR_DIRECTORY, new_clus, 0);
    make_83_component("..", dot83);
    fill_dirent_meta(&entries[1], dot83, FAT_ATTR_DIRECTORY, parent_clus, 0);
    if (disk_rw_lba(1, data_lba + (new_clus - 2) * bpb_sectors_per_cluster,
                    MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    /* Write new directory entry in parent directory */
    if (disk_rw_lba(0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    fill_dirent_meta(&entries[dir_idx], name83, FAT_ATTR_DIRECTORY, new_clus, 0);
    if (disk_rw_lba(1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    return 0;
}

static int is_dir_empty(uint16_t dir_cluster) {
    uint16_t s, i, sec_lba;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    for (s = 0; dir_get_sector_lba(dir_cluster, s, &sec_lba) == 0; s++) {
        if (disk_rw_lba(0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
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
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t dir_sec_lba = 0;
    uint16_t dir_idx     = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (resolve_parent_and_leaf(path, &parent_clus, name83) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }
    if (find_dir_entry(parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) != 0) {
        return -1;
    }
    if ((entry.attr & FAT_ATTR_DIRECTORY) == 0) {
        return -1;
    }
    if (entry.fst_clus_lo == 0 || entry.fst_clus_lo == cwd_cluster) {
        return -1;
    }
    if (!is_dir_empty(entry.fst_clus_lo)) {
        return -1;
    }
    if (free_cluster_chain(entry.fst_clus_lo) != 0) {
        return -1;
    }
    if (disk_rw_lba(0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    entries[dir_idx].name[0] = 0xE5;
    if (disk_rw_lba(1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    return 0;
}

int32_t fat16_file_size(const char __far *filename) {
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;

    if (resolve_parent_and_leaf(filename, &parent_clus, name83) != 0) {
        return -1;
    }
    if (find_dir_entry(parent_clus, name83, &entry, 0, 0) != 0) {
        return -1;
    }
    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }
    return (int32_t)entry.file_size;
}

int fat16_read_file(const char __far *filename, void __far *dest, uint16_t max_bytes) {
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t cluster;
    uint16_t target_bytes;
    uint16_t bytes_read = 0;
    uint8_t __far *out_ptr = (uint8_t __far *)dest;

    if (resolve_parent_and_leaf(filename, &parent_clus, name83) != 0) {
        return -1;
    }
    if (find_dir_entry(parent_clus, name83, &entry, 0, 0) != 0) {
        return -1;
    }
    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }

    target_bytes = (entry.file_size < (uint32_t)max_bytes)
                       ? (uint16_t)entry.file_size
                       : max_bytes;
    cluster = entry.fst_clus_lo;

    while (cluster >= 2 && cluster < 0xFFF8 && bytes_read < target_bytes) {
        uint16_t first_lba = data_lba + (cluster - 2) * bpb_sectors_per_cluster;
        uint8_t s;
        for (s = 0; s < bpb_sectors_per_cluster && bytes_read < target_bytes; s++) {
            uint16_t chunk = target_bytes - bytes_read;
            uint16_t j;
            if (chunk > 512) {
                chunk = 512;
            }
            if (disk_rw_lba(0, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
            for (j = 0; j < chunk; j++) {
                out_ptr[bytes_read + j] = io_buf[j];
            }
            bytes_read += chunk;
        }
        cluster = fat_get_cluster(cluster);
    }

    return (int)bytes_read;
}

int fat16_load_executable(const char __far *filename, uint16_t dest_seg, uint32_t *out_size) {
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t cluster;
    uint16_t cur_seg = dest_seg;
    uint16_t cur_off = 0x0000;
    uint32_t remaining;

    if (resolve_parent_and_leaf(filename, &parent_clus, name83) != 0) {
        return -1;
    }
    if (find_dir_entry(parent_clus, name83, &entry, 0, 0) != 0) {
        return -1;
    }
    if ((entry.attr & FAT_ATTR_DIRECTORY) || entry.file_size == 0) {
        return -1;
    }

    remaining = entry.file_size;
    cluster   = entry.fst_clus_lo;

    while (cluster >= 2 && cluster < 0xFFF8 && remaining > 0) {
        uint16_t first_lba = data_lba + (cluster - 2) * bpb_sectors_per_cluster;
        uint8_t s;
        for (s = 0; s < bpb_sectors_per_cluster && remaining > 0; s++) {
            if (disk_rw_lba(0, first_lba + s, MK_FP(cur_seg, cur_off)) != 0) {
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
        cluster = fat_get_cluster(cluster);
    }

    if (out_size) {
        *out_size = entry.file_size;
    }
    return 0;
}

int fat16_write_file(const char __far *filename, const void __far *src, uint16_t size) {
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t dir_sec_lba   = 0;
    uint16_t dir_idx       = 0;
    uint16_t first_cluster = 0;
    uint16_t prev_cluster  = 0;
    uint16_t search_hint   = 2;
    uint16_t written       = 0;
    const uint8_t __far *in_ptr = (const uint8_t __far *)src;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (resolve_parent_and_leaf(filename, &parent_clus, name83) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }

    if (find_dir_entry(parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) == 0) {
        if (entry.attr & FAT_ATTR_DIRECTORY) {
            return -1;
        }
        if (entry.fst_clus_lo >= 2) {
            if (free_cluster_chain(entry.fst_clus_lo) != 0) {
                return -1;
            }
        }
    } else {
        if (find_free_dir_slot(parent_clus, &dir_sec_lba, &dir_idx) != 0) {
            return -1;
        }
    }

    while (written < size) {
        uint16_t clus = alloc_free_cluster(search_hint);
        uint16_t first_lba;
        uint8_t s;

        if (clus == 0) {
            return -1;
        }
        search_hint = clus + 1;

        if (first_cluster == 0) {
            first_cluster = clus;
        } else {
            if (fat_set_cluster(prev_cluster, clus) != 0) {
                return -1;
            }
        }
        prev_cluster = clus;

        first_lba = data_lba + (clus - 2) * bpb_sectors_per_cluster;
        for (s = 0; s < bpb_sectors_per_cluster; s++) {
            uint16_t j;
            for (j = 0; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (disk_rw_lba(1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
        }
    }

    if (fat_flush() != 0) {
        return -1;
    }

    if (disk_rw_lba(0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    fill_dirent_meta(&entries[dir_idx], name83, FAT_ATTR_ARCHIVE, first_cluster, (uint32_t)size);
    if (disk_rw_lba(1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    return (int)size;
}

int fat16_append_file(const char __far *filename, const void __far *src, uint16_t size) {
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t dir_sec_lba = 0;
    uint16_t dir_idx     = 0;
    uint16_t last_clus;
    uint16_t cluster_bytes;
    uint16_t off_in_clus;
    uint16_t written = 0;
    uint16_t search_hint = 2;
    const uint8_t __far *in_ptr = (const uint8_t __far *)src;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (resolve_parent_and_leaf(filename, &parent_clus, name83) != 0) {
        return -1;
    }
    if (name83[0] == '.') {
        return -1;
    }

    /* If file does not exist or is empty, create/write it from scratch */
    if (find_dir_entry(parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) != 0 ||
        entry.file_size == 0 || entry.fst_clus_lo < 2) {
        return fat16_write_file(filename, src, size);
    }

    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }
    if (size == 0) {
        return 0;
    }

    /* Walk to the last cluster in the existing file's chain */
    last_clus = entry.fst_clus_lo;
    while (last_clus >= 2 && last_clus < 0xFFF8) {
        uint16_t next = fat_get_cluster(last_clus);
        if (next >= 0xFFF8 || next < 2) {
            break;
        }
        last_clus = next;
    }

    cluster_bytes = (uint16_t)bpb_sectors_per_cluster * 512;
    off_in_clus   = (uint16_t)(entry.file_size & (cluster_bytes - 1));

    /* Fill remaining space in the last allocated cluster if not on a cluster boundary */
    if (off_in_clus != 0) {
        uint16_t first_lba   = data_lba + (last_clus - 2) * bpb_sectors_per_cluster;
        uint8_t  sec_in_clus = (uint8_t)(off_in_clus >> 9);
        uint16_t off_in_sec  = off_in_clus & 511;
        uint8_t  s;

        if (off_in_sec != 0) {
            uint16_t j;
            if (disk_rw_lba(0, first_lba + sec_in_clus, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
            for (j = off_in_sec; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (disk_rw_lba(1, first_lba + sec_in_clus, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
            sec_in_clus++;
        }

        for (s = sec_in_clus; s < bpb_sectors_per_cluster && written < size; s++) {
            uint16_t j;
            for (j = 0; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (disk_rw_lba(1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
        }
    }

    /* Allocate additional clusters if more data remains */
    while (written < size) {
        uint16_t clus = alloc_free_cluster(search_hint);
        uint16_t first_lba;
        uint8_t  s;

        if (clus == 0) {
            return -1;
        }
        search_hint = clus + 1;
        if (fat_set_cluster(last_clus, clus) != 0) {
            return -1;
        }
        last_clus = clus;

        first_lba = data_lba + (clus - 2) * bpb_sectors_per_cluster;
        for (s = 0; s < bpb_sectors_per_cluster; s++) {
            uint16_t j;
            for (j = 0; j < 512; j++) {
                if (written < size) {
                    io_buf[j] = in_ptr[written++];
                } else {
                    io_buf[j] = 0;
                }
            }
            if (disk_rw_lba(1, first_lba + s, MK_FP(KERNEL_SEG, io_buf)) != 0) {
                return -1;
            }
        }
    }

    if (fat_flush() != 0) {
        return -1;
    }

    /* Update directory entry with the new file size */
    if (disk_rw_lba(0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    entries[dir_idx].file_size = entry.file_size + (uint32_t)size;
    if (disk_rw_lba(1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }

    return (int)size;
}

int fat16_delete_file(const char __far *filename) {
    uint16_t parent_clus;
    uint8_t name83[11];
    fat16_dirent_t entry;
    uint16_t dir_sec_lba = 0;
    uint16_t dir_idx     = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (resolve_parent_and_leaf(filename, &parent_clus, name83) != 0) {
        return -1;
    }
    if (find_dir_entry(parent_clus, name83, &entry, &dir_sec_lba, &dir_idx) != 0) {
        return -1;
    }
    if (entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }
    if (entry.fst_clus_lo >= 2) {
        if (free_cluster_chain(entry.fst_clus_lo) != 0) {
            return -1;
        }
    }
    if (disk_rw_lba(0, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    entries[dir_idx].name[0] = 0xE5;
    if (disk_rw_lba(1, dir_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
        return -1;
    }
    return 0;
}

int fat16_copy_file(const char __far *src_path, const char __far *dst_path, uint8_t append) {
    static uint8_t copy_buf[512];
    uint16_t src_parent, dst_parent;
    uint8_t  src_name83[11], dst_name83[11];
    fat16_dirent_t src_entry;
    uint32_t remaining;
    uint16_t cur_clus;
    uint8_t  first_chunk = (uint8_t)(!append);

    if (resolve_parent_and_leaf(src_path, &src_parent, src_name83) != 0) {
        return -1;
    }
    if (find_dir_entry(src_parent, src_name83, &src_entry, 0, 0) != 0) {
        return -1;
    }
    if (src_entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }

    if (resolve_parent_and_leaf(dst_path, &dst_parent, dst_name83) != 0) {
        return -1;
    }
    if (dst_name83[0] == '.') {
        return -1;
    }
    if (!append && src_parent == dst_parent && name_equals(src_name83, dst_name83)) {
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

    while (cur_clus >= 2 && cur_clus < 0xFFF8 && remaining > 0) {
        uint16_t first_lba = data_lba + (cur_clus - 2) * bpb_sectors_per_cluster;
        uint16_t next_clus = fat_get_cluster(cur_clus);
        uint8_t  s;

        for (s = 0; s < bpb_sectors_per_cluster && remaining > 0; s++) {
            uint16_t chunk = (remaining > 512) ? 512 : (uint16_t)remaining;
            int rc;

            if (disk_rw_lba(0, first_lba + s, MK_FP(KERNEL_SEG, copy_buf)) != 0) {
                return -1;
            }
            if (first_chunk) {
                rc = fat16_write_file(dst_path, MK_FP(KERNEL_SEG, copy_buf), chunk);
                first_chunk = 0;
            } else {
                rc = fat16_append_file(dst_path, MK_FP(KERNEL_SEG, copy_buf), chunk);
            }
            if (rc < 0) {
                return -1;
            }
            remaining -= chunk;
        }
        cur_clus = next_clus;
    }

    return 0;
}

int fat16_rename_file(const char __far *src_path, const char __far *dst_path) {
    uint16_t src_parent, dst_parent;
    uint8_t  src_name83[11], dst_name83[11];
    fat16_dirent_t src_entry, dst_entry;
    uint16_t src_sec_lba = 0, src_idx = 0;
    uint16_t dst_sec_lba = 0, dst_idx = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;
    int i;

    if (resolve_parent_and_leaf(src_path, &src_parent, src_name83) != 0) {
        return -1;
    }
    if (find_dir_entry(src_parent, src_name83, &src_entry, &src_sec_lba, &src_idx) != 0) {
        return -1;
    }
    if (src_entry.attr & FAT_ATTR_DIRECTORY) {
        return -1;
    }

    if (resolve_parent_and_leaf(dst_path, &dst_parent, dst_name83) != 0) {
        return -1;
    }
    if (dst_name83[0] == '.') {
        return -1;
    }
    if (src_parent == dst_parent && name_equals(src_name83, dst_name83)) {
        return 0;
    }

    if (find_dir_entry(dst_parent, dst_name83, &dst_entry, &dst_sec_lba, &dst_idx) == 0) {
        if (dst_entry.attr & FAT_ATTR_DIRECTORY) {
            return -1;
        }
        if (dst_entry.fst_clus_lo >= 2) {
            if (free_cluster_chain(dst_entry.fst_clus_lo) != 0) {
                return -1;
            }
        }
    } else {
        if (find_free_dir_slot(dst_parent, &dst_sec_lba, &dst_idx) != 0) {
            return -1;
        }
    }

    for (i = 0; i < 11; i++) {
        src_entry.name[i] = dst_name83[i];
    }

    if (dst_sec_lba == src_sec_lba) {
        if (disk_rw_lba(0, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        entries[src_idx].name[0] = 0xE5;
        entries[dst_idx] = src_entry;
        if (disk_rw_lba(1, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
    } else {
        if (disk_rw_lba(0, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        entries[dst_idx] = src_entry;
        if (disk_rw_lba(1, dst_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }

        if (disk_rw_lba(0, src_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
        entries[src_idx].name[0] = 0xE5;
        if (disk_rw_lba(1, src_sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
            return -1;
        }
    }

    return 0;
}

static int resolve_dir_cluster(const char __far *path, uint16_t *out_cluster) {
    uint16_t cur_clus;
    uint16_t pos = 0;
    char comp[16];

    if (!fs_mounted) {
        return -1;
    }
    if (!path || path[0] == '\0') {
        *out_cluster = cwd_cluster;
        return 0;
    }

    if (is_sep(path[0])) {
        cur_clus = 0;
        while (is_sep(path[pos])) {
            pos++;
        }
    } else {
        cur_clus = cwd_cluster;
    }

    while (path[pos] != '\0') {
        uint16_t k = 0;
        while (path[pos] != '\0' && !is_sep(path[pos])) {
            if (k >= 14) {
                return -1;
            }
            comp[k++] = path[pos++];
        }
        comp[k] = '\0';

        while (is_sep(path[pos])) {
            pos++;
        }

        if (step_into_dir(cur_clus, comp, &cur_clus) != 0) {
            return -1;
        }
    }

    *out_cluster = cur_clus;
    return 0;
}

int fat16_dir_entry(const char __far *path, uint16_t index, os_dir_entry_t __far *out_info) {
    uint16_t target_clus;
    uint16_t s, i, sec_lba;
    uint16_t cur_idx = 0;
    fat16_dirent_t *entries = (fat16_dirent_t *)io_buf;

    if (!fs_mounted || !out_info) {
        return -1;
    }
    if (resolve_dir_cluster(path, &target_clus) != 0) {
        return -2;
    }

    for (s = 0; dir_get_sector_lba(target_clus, s, &sec_lba) == 0; s++) {
        if (disk_rw_lba(0, sec_lba, MK_FP(KERNEL_SEG, io_buf)) != 0) {
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
