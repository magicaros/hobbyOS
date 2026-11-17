/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * FDISK.BIN - Fixed Disk Partitioning Utility for hobbyOS.
 * Supports interactive menu-driven operation and command-line flags.
 */

#include "../include/hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

uint16_t div_dx_ax(uint16_t hi, uint16_t lo, uint16_t divisor);
#pragma aux div_dx_ax = "div bx" parm [dx] [ax] [bx] value [ax];

uint16_t rem_dx_ax(uint16_t hi, uint16_t lo, uint16_t divisor);
#pragma aux rem_dx_ax = "div bx" parm [dx] [ax] [bx] value [dx];

static uint32_t u32_div_u16(uint32_t val, uint16_t divisor, uint16_t *rem_out) {
    uint16_t hi = (uint16_t)(val >> 16);
    uint16_t lo = (uint16_t)(val & 0xFFFF);
    uint16_t q_hi = 0;
    uint16_t r_hi = hi;
    uint16_t q_lo;
    uint16_t r_lo;

    if (divisor == 0) return 0;
    if (hi >= divisor) {
        q_hi = hi / divisor;
        r_hi = hi % divisor;
    }
    q_lo = div_dx_ax(r_hi, lo, divisor);
    r_lo = rem_dx_ax(r_hi, lo, divisor);
    if (rem_out) *rem_out = r_lo;
    return ((uint32_t)q_hi << 16) | q_lo;
}

static uint32_t u32_mul10(uint32_t val) {
    return (val << 3) + (val << 1);
}

static uint16_t u32_div10(uint32_t *val) {
    uint16_t hi = (uint16_t)(*val >> 16);
    uint16_t lo = (uint16_t)(*val & 0xFFFF);
    uint16_t q_hi = hi / 10;
    uint16_t r_hi = hi % 10;
    uint16_t d_hi = (r_hi << 4) + (lo >> 12);
    uint16_t q_mid = d_hi / 10;
    uint16_t r_mid = d_hi % 10;
    uint16_t d_lo = (r_mid << 12) + (lo & 0x0FFF);
    uint16_t q_lo = d_lo / 10;
    uint16_t rem  = d_lo % 10;
    *val = ((uint32_t)q_hi << 16) | ((uint32_t)(q_mid << 12) + q_lo);
    return rem;
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

static void print_hex_byte(uint8_t val) {
    static const char hex[] = "0123456789ABCDEF";
    os_putc(hex[(val >> 4) & 0x0F]);
    os_putc(hex[val & 0x0F]);
}

static uint16_t str_len(const char *s) {
    uint16_t len = 0;
    while (s && s[len] != '\0') {
        len++;
    }
    return len;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static int str_eq_nocase(const char *a, const char *b) {
    uint16_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (to_upper(a[i]) != to_upper(b[i])) return 0;
        i++;
    }
    return (a[i] == '\0' && b[i] == '\0') ? 1 : 0;
}

static uint16_t read_line(char *buf, uint16_t max_len) {
    uint16_t pos = 0;
    while (pos + 1 < max_len) {
        uint16_t key = os_getc();
        char c = (char)(key & 0xFF);
        if (c == '\r' || c == '\n') {
            os_putc('\n');
            break;
        } else if (c == 0x08) { /* Backspace */
            if (pos > 0) {
                pos--;
                os_putc(0x08);
                os_putc(' ');
                os_putc(0x08);
            }
        } else if (c == 0x1B) { /* Escape */
            pos = 0;
            os_puts("\n");
            break;
        } else if (c >= 0x20 && c <= 0x7E) {
            buf[pos++] = c;
            os_putc(c);
        }
    }
    buf[pos] = '\0';
    return pos;
}

static const char *part_type_name(uint8_t type) {
    switch (type) {
    case 0x00: return "Empty";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16A";
    case 0x06: return "FAT16B";
    case 0x0E: return "FAT16B LBA";
    case 0x0B: return "FAT32";
    case 0x0C: return "FAT32 LBA";
    case 0x83: return "Linux Native";
    case 0x82: return "Linux Swap";
    default:   return "Unknown";
    }
}

static void display_part_table(uint8_t drive, const os_disk_geom_t *geom, const os_part_table_t *table) {
    uint32_t total_mb;
    int p;

    total_mb = (geom->total_sectors + 1024) >> 11;

    os_puts("\nFixed Disk: Drive 0x");
    print_hex_byte(drive);
    os_puts(" (");
    print_uint32(total_mb);
    os_puts(" MB, ");
    print_uint16(geom->cyls);
    os_puts(" Cyls, ");
    print_uint16((uint16_t)geom->heads);
    os_puts(" Heads, ");
    print_uint16((uint16_t)geom->spt);
    os_puts(" Sectors/Track)\n");

    if (!table->is_valid_mbr) {
        os_puts("Notice: No valid MBR signature (0x55AA) found on Sector 0.\n");
    }

    os_puts("Part  Status   Type             Start (MB)  Size (MB)   Sectors\n");
    os_puts("----  -------  ---------------  ----------  ----------  ----------\n");

    for (p = 0; p < 4; p++) {
        const os_partition_t *part = &table->parts[p];
        uint32_t start_mb = (part->start_lba + 1024) >> 11;
        uint32_t size_mb  = (part->total_sectors + 1024) >> 11;

        os_puts(" ");
        print_uint16((uint16_t)(p + 1));
        os_puts("    ");

        if (part->boot_indicator == 0x80) {
            os_puts("Active   ");
        } else {
            os_puts("         ");
        }

        os_puts(part_type_name(part->part_type));
        if (part->part_type != 0x00) {
            os_puts(" (0x");
            print_hex_byte(part->part_type);
            os_puts(")");
            if (part->part_type == 0x04 || part->part_type == 0x06 || part->part_type == 0x01) {
                os_puts("  ");
            } else {
                os_puts(" ");
            }
        } else {
            os_puts("         ");
        }

        if (part->part_type != 0x00 && part->total_sectors > 0) {
            print_uint32(start_mb);
            os_puts(" MB\t");
            print_uint32(size_mb);
            os_puts(" MB\t");
            print_uint32(part->total_sectors);
        } else {
            os_puts("--\t\t--\t\t--");
        }
        os_puts("\n");
    }
    os_puts("\n");
}

static void cmd_display_status(void) {
    os_disk_geom_t geom;
    os_part_table_t table;
    uint8_t d;

    if (os_disk_get_geom(0x80, (os_disk_geom_t __far *)FP_HERE(&geom)) != 0) {
        os_puts("Error: No fixed disk drives detected by BIOS.\n");
        return;
    }

    for (d = 0; d < geom.num_drives && d < 2; d++) {
        uint8_t drive = (uint8_t)(0x80 + d);
        if (os_disk_get_geom(drive, (os_disk_geom_t __far *)FP_HERE(&geom)) == 0) {
            if (os_part_read(drive, (os_part_table_t __far *)FP_HERE(&table)) == 0) {
                display_part_table(drive, &geom, &table);
            }
        }
    }
}

static int do_activate_partition(uint8_t drive, int part_idx) {
    os_part_table_t table;
    int p;

    if (part_idx < 0 || part_idx > 3) {
        os_puts("Error: Invalid partition index (1-4 expected).\n");
        return -1;
    }
    if (os_part_read(drive, (os_part_table_t __far *)FP_HERE(&table)) != 0) {
        os_puts("Error reading partition table.\n");
        return -1;
    }
    if (table.parts[part_idx].part_type == 0x00 || table.parts[part_idx].total_sectors == 0) {
        os_puts("Error: Cannot activate empty partition.\n");
        return -1;
    }

    for (p = 0; p < 4; p++) {
        table.parts[p].boot_indicator = (uint8_t)((p == part_idx) ? 0x80 : 0x00);
    }

    if (os_part_write(drive, (const os_part_table_t __far *)FP_HERE(&table), 0) != 0) {
        os_puts("Error writing partition table.\n");
        return -1;
    }

    os_puts("Partition ");
    print_uint16((uint16_t)(part_idx + 1));
    os_puts(" marked Active (Bootable).\n");
    return 0;
}

static int do_delete_partition(uint8_t drive, int part_idx) {
    os_part_table_t table;
    int i;
    uint8_t *p_bytes;

    if (part_idx < 0 || part_idx > 3) {
        os_puts("Error: Invalid partition index (1-4 expected).\n");
        return -1;
    }
    if (os_part_read(drive, (os_part_table_t __far *)FP_HERE(&table)) != 0) {
        os_puts("Error reading partition table.\n");
        return -1;
    }
    if (table.parts[part_idx].part_type == 0x00 && table.parts[part_idx].total_sectors == 0) {
        os_puts("Notice: Partition is already empty.\n");
        return 0;
    }

    p_bytes = (uint8_t *)&table.parts[part_idx];
    for (i = 0; i < sizeof(os_partition_t); i++) {
        p_bytes[i] = 0;
    }

    if (os_part_write(drive, (const os_part_table_t __far *)FP_HERE(&table), 0) != 0) {
        os_puts("Error updating partition table.\n");
        return -1;
    }

    os_puts("Partition ");
    print_uint16((uint16_t)(part_idx + 1));
    os_puts(" deleted.\n");
    return 0;
}

static int do_create_partition(uint8_t drive, uint32_t size_mb) {
    os_disk_geom_t geom;
    os_part_table_t table;
    int slot = -1;
    int p;
    uint32_t start_lba = 0;
    uint32_t avail_sectors = 0;
    uint32_t part_sectors = 0;
    uint16_t track_sec;
    uint16_t start_cyl, end_cyl;
    uint16_t rem;
    uint8_t p_type;
    uint8_t boot_flag = 0x00;

    if (os_disk_get_geom(drive, (os_disk_geom_t __far *)FP_HERE(&geom)) != 0) {
        os_puts("Error: Could not query drive geometry.\n");
        return -1;
    }
    if (os_part_read(drive, (os_part_table_t __far *)FP_HERE(&table)) != 0) {
        os_puts("Error reading partition table.\n");
        return -1;
    }

    /* Find first empty partition slot */
    for (p = 0; p < 4; p++) {
        if (table.parts[p].part_type == 0x00 && table.parts[p].total_sectors == 0) {
            slot = p;
            break;
        }
    }
    if (slot == -1) {
        os_puts("Error: Partition table full (maximum 4 primary partitions).\n");
        return -1;
    }

    /* Find highest used LBA */
    track_sec = (uint16_t)geom.heads * geom.spt;
    start_lba = geom.spt; /* Reserve track 0 for MBR */
    for (p = 0; p < 4; p++) {
        if (table.parts[p].part_type != 0x00 && table.parts[p].total_sectors > 0) {
            uint32_t p_end = table.parts[p].start_lba + table.parts[p].total_sectors;
            if (p_end > start_lba) {
                start_lba = p_end;
            }
        }
    }

    /* Align start_lba to next cylinder boundary */
    u32_div_u16(start_lba, track_sec, &rem);
    if (rem != 0 && track_sec > rem) {
        start_lba += (track_sec - rem);
    }

    if (start_lba >= geom.total_sectors) {
        os_puts("Error: No free disk space remaining on drive.\n");
        return -1;
    }

    avail_sectors = geom.total_sectors - start_lba;

    if (size_mb == 0 || size_mb == 0xFFFFFFFF) {
        /* Allocate all remaining space */
        part_sectors = avail_sectors;
    } else {
        uint32_t req_sec = size_mb << 11; /* size_mb * 2048 */
        if (req_sec > avail_sectors) {
            part_sectors = avail_sectors;
        } else {
            part_sectors = req_sec;
        }
    }

    /* Align partition size to cylinder boundary */
    u32_div_u16(part_sectors, track_sec, &rem);
    if (rem != 0 && part_sectors > rem) {
        part_sectors -= rem;
    }

    if (part_sectors == 0) {
        os_puts("Error: Insufficient space for a cylinder-aligned partition.\n");
        return -1;
    }

    /* Select partition type */
    if (part_sectors > 65535UL) {
        p_type = 0x06; /* FAT16B / BigDOS */
    } else if (part_sectors >= 8192UL) {
        p_type = 0x04; /* FAT16A */
    } else {
        p_type = 0x01; /* FAT12 */
    }

    /* Compute cylinder numbers */
    start_cyl = (uint16_t)u32_div_u16(start_lba, track_sec, 0);
    end_cyl   = (uint16_t)u32_div_u16(start_lba + part_sectors - 1, track_sec, 0);

    /* If no partition is currently active, mark this new partition active */
    boot_flag = 0x80;
    for (p = 0; p < 4; p++) {
        if (table.parts[p].boot_indicator == 0x80) {
            boot_flag = 0x00;
            break;
        }
    }

    /* Fill partition table entry */
    table.parts[slot].boot_indicator = boot_flag;
    table.parts[slot].part_type      = p_type;
    table.parts[slot].start_lba      = start_lba;
    table.parts[slot].total_sectors  = part_sectors;

    /* CHS Encoding */
    if (start_cyl == 0) {
        table.parts[slot].start_head     = 1;
        table.parts[slot].start_sector   = 1;
        table.parts[slot].start_cylinder = 0;
    } else {
        uint16_t sc = (start_cyl <= 1023) ? start_cyl : 1023;
        table.parts[slot].start_head     = 0;
        table.parts[slot].start_sector   = (uint8_t)(1 | ((sc >> 2) & 0xC0));
        table.parts[slot].start_cylinder = (uint8_t)(sc & 0xFF);
    }

    {
        uint16_t ec = (end_cyl <= 1023) ? end_cyl : 1023;
        table.parts[slot].end_head     = (uint8_t)(geom.heads - 1);
        table.parts[slot].end_sector   = (uint8_t)((geom.spt & 0x3F) | ((ec >> 2) & 0xC0));
        table.parts[slot].end_cylinder = (uint8_t)(ec & 0xFF);
    }

    /* Write updated partition table (and install MBR boot code if missing) */
    if (os_part_write(drive, (const os_part_table_t __far *)FP_HERE(&table), 1) != 0) {
        os_puts("Error writing partition table to disk.\n");
        return -1;
    }

    os_puts("Partition ");
    print_uint16((uint16_t)(slot + 1));
    os_puts(" created successfully (Type: ");
    os_puts(part_type_name(p_type));
    os_puts(", Size: ");
    print_uint32((part_sectors + 1024) >> 11);
    os_puts(" MB).\n");
    return 0;
}

static void run_interactive_menu(uint8_t current_drive) {
    os_disk_geom_t geom;
    os_part_table_t table;
    char line[16];

    for (;;) {
        if (os_disk_get_geom(current_drive, (os_disk_geom_t __far *)FP_HERE(&geom)) != 0) {
            os_puts("Error: Unable to query fixed disk drive geometry.\n");
            return;
        }
        if (os_part_read(current_drive, (os_part_table_t __far *)FP_HERE(&table)) != 0) {
            os_puts("Error reading partition table.\n");
            return;
        }

        os_puts("\n=======================================================\n");
        os_puts("           hobbyOS Fixed Disk Setup Utility\n");
        os_puts("             Copyright (C) 2026 Benoit Colson\n");
        os_puts("=======================================================\n\n");
        os_puts("Current Fixed Disk Drive: ");
        print_uint16((uint16_t)(current_drive - 0x80 + 1));
        os_puts(" (0x");
        print_hex_byte(current_drive);
        os_puts(", ");
        print_uint32((geom.total_sectors + 1024) >> 11);
        os_puts(" MB)\n\n");
        os_puts("Choose one of the following:\n\n");
        os_puts("  1. Create hobbyOS / DOS Partition\n");
        os_puts("  2. Set Active / Bootable Partition\n");
        os_puts("  3. Delete Partition\n");
        os_puts("  4. Display Partition Information\n");
        if (geom.num_drives > 1) {
            os_puts("  5. Change Current Fixed Disk Drive\n");
        }
        os_puts("  Q. Exit FDISK\n\n");
        os_puts("Enter choice: ");

        read_line(line, sizeof(line));
        if (line[0] == 'Q' || line[0] == 'q') {
            break;
        } else if (line[0] == '1') {
            char sz_buf[16];
            uint32_t sz_mb = 0;
            uint16_t i;
            os_puts("\nEnter partition size in MB (or press Enter for maximum): ");
            read_line(sz_buf, sizeof(sz_buf));
            for (i = 0; sz_buf[i] >= '0' && sz_buf[i] <= '9'; i++) {
                sz_mb = u32_mul10(sz_mb) + (uint32_t)(sz_buf[i] - '0');
            }
            do_create_partition(current_drive, sz_mb);
        } else if (line[0] == '2') {
            char p_buf[8];
            os_puts("\nEnter partition number to mark Active (1-4): ");
            read_line(p_buf, sizeof(p_buf));
            if (p_buf[0] >= '1' && p_buf[0] <= '4') {
                do_activate_partition(current_drive, (p_buf[0] - '1'));
            } else {
                os_puts("Invalid partition number.\n");
            }
        } else if (line[0] == '3') {
            char p_buf[8];
            char c_buf[8];
            os_puts("\nEnter partition number to delete (1-4): ");
            read_line(p_buf, sizeof(p_buf));
            if (p_buf[0] >= '1' && p_buf[0] <= '4') {
                os_puts("WARNING: All data in partition ");
                os_putc(p_buf[0]);
                os_puts(" will be permanently lost!\nAre you sure (Y/N)? ");
                read_line(c_buf, sizeof(c_buf));
                if (c_buf[0] == 'Y' || c_buf[0] == 'y') {
                    do_delete_partition(current_drive, (p_buf[0] - '1'));
                } else {
                    os_puts("Deletion canceled.\n");
                }
            } else {
                os_puts("Invalid partition number.\n");
            }
        } else if (line[0] == '4') {
            display_part_table(current_drive, &geom, &table);
        } else if (line[0] == '5' && geom.num_drives > 1) {
            current_drive = (current_drive == 0x80) ? 0x81 : 0x80;
            os_puts("Switched to Fixed Disk Drive ");
            print_uint16((uint16_t)(current_drive - 0x80 + 1));
            os_puts(".\n");
        }
    }
}

int app_main(void) {
    const char *args = os_get_args();
    uint8_t drive = 0x80;
    int i = 0;
    int had_flag = 0;

    /* Parse target drive if specified as HD0: or HD1: or 1/2 */
    while (args && args[i] == ' ') i++;
    if (args && args[i] != '\0' && args[i] != '/' && args[i] != '-') {
        char d_str[8];
        int p = 0;
        while (args[i] != '\0' && args[i] != ' ' && args[i] != '/' && p < sizeof(d_str) - 1) {
            d_str[p++] = to_upper(args[i++]);
        }
        d_str[p] = '\0';
        if (str_eq_nocase(d_str, "HD1:") || str_eq_nocase(d_str, "HD1") || str_eq_nocase(d_str, "2")) {
            drive = 0x81;
        } else {
            drive = 0x80;
        }
    }

    /* Parse flags */
    while (args && args[i] != '\0') {
        while (args[i] == ' ') i++;
        if (args[i] == '/' || args[i] == '-') {
            had_flag = 1;
            i++;
            if (to_upper(args[i]) == 'S' || str_eq_nocase(&args[i], "STATUS")) {
                cmd_display_status();
                return 0;
            } else if (to_upper(args[i]) == 'M' || str_eq_nocase(&args[i], "MBR")) {
                os_part_table_t table;
                if (os_part_read(drive, (os_part_table_t __far *)FP_HERE(&table)) == 0) {
                    if (os_part_write(drive, (const os_part_table_t __far *)FP_HERE(&table), 1) == 0) {
                        os_puts("hobbyOS Master Boot Record (MBR) installed successfully on drive 0x");
                        print_hex_byte(drive);
                        os_puts(".\n");
                        return 0;
                    }
                }
                os_puts("Error writing MBR.\n");
                return 1;
            } else if (to_upper(args[i]) == 'A') {
                /* /ACTIVATE:n */
                while (args[i] != '\0' && args[i] != ':' && args[i] != '=') i++;
                if (args[i] == ':' || args[i] == '=') i++;
                if (args[i] >= '1' && args[i] <= '4') {
                    return do_activate_partition(drive, (args[i] - '1'));
                } else {
                    os_puts("Error: Invalid partition index for /ACTIVATE (1-4 expected).\n");
                    return 1;
                }
            } else if (to_upper(args[i]) == 'D') {
                /* /DELETE:n */
                while (args[i] != '\0' && args[i] != ':' && args[i] != '=') i++;
                if (args[i] == ':' || args[i] == '=') i++;
                if (args[i] >= '1' && args[i] <= '4') {
                    return do_delete_partition(drive, (args[i] - '1'));
                } else {
                    os_puts("Error: Invalid partition index for /DELETE (1-4 expected).\n");
                    return 1;
                }
            } else if (to_upper(args[i]) == 'C') {
                /* /CREATE:size */
                uint32_t sz_mb = 0;
                while (args[i] != '\0' && args[i] != ':' && args[i] != '=') i++;
                if (args[i] == ':' || args[i] == '=') i++;
                if (to_upper(args[i]) == 'M' && to_upper(args[i + 1]) == 'A' && to_upper(args[i + 2]) == 'X') {
                    sz_mb = 0;
                } else {
                    while (args[i] >= '0' && args[i] <= '9') {
                        sz_mb = u32_mul10(sz_mb) + (uint32_t)(args[i++] - '0');
                    }
                }
                return do_create_partition(drive, sz_mb);
            } else {
                os_puts("hobbyOS Fixed Disk Setup Utility (FDISK.BIN)\n");
                os_puts("Usage: FDISK [HD0:|HD1:] [/STATUS] [/MBR] [/CREATE:size|MAX] [/ACTIVATE:n] [/DELETE:n]\n");
                return 0;
            }
        } else {
            i++;
        }
    }

    if (!had_flag) {
        run_interactive_menu(drive);
    }
    return 0;
}
