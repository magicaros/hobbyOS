/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LZ4 compression/decompression algorithms:
 * Copyright (C) 2011-2020, Yann Collet.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * HAR (HobbyOS Archiver) - In-memory LZ4 block compression and archive manager.
 */

#include <hobbyos.h>

#define HAR_MAGIC       "HAR2"
#define HAR_ENTRY_MAGIC "HF"
#define HAR_END_MAGIC   "HE"

#define HAR_FLAG_COMPRESSED 0x0001

#define BUF_PARAS_32K   2048  /* 32 KB = 32,768 bytes */
#define BUF_BYTES_32K   32768U
#define HASH_PARAS_8K   512   /* 8 KB hash table */

#define HASH_LOG        12
#define HASH_SIZE       (1 << HASH_LOG)  /* 4096 entries */
#define HASH_MASK       (HASH_SIZE - 1)

#pragma pack(push, 1)

typedef struct {
    char     magic[4];      /* "HAR1" or "HAR2" */
    uint16_t version;       /* 1 or 2 */
    uint16_t flags;         /* Reserved */
} har_header_t;

typedef struct {
    char     magic[2];      /* "HF" */
    uint8_t  attr;          /* FAT attribute (OS_ATTR_ARCHIVE, etc.) */
    uint8_t  name_len;      /* Length of relative filename */
    uint32_t uncomp_size;   /* Uncompressed file size in bytes */
    uint32_t comp_size;     /* Compressed payload size in bytes */
    uint16_t flags;         /* Bit 0: HAR_FLAG_COMPRESSED */
    uint16_t crc;           /* CRC16 of uncompressed data */
} har_entry_t;

typedef struct {
    uint16_t comp_len;      /* Compressed payload length in bytes */
    uint16_t raw_len;       /* Uncompressed chunk length in bytes */
} har_chunk_hdr_t;

typedef struct {
    char     magic[2];      /* "HE" */
    uint16_t entry_count;   /* Total files in archive */
} har_end_t;

#pragma pack(pop)

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

static uint16_t calc_ratio(uint32_t part, uint32_t whole) {
    uint32_t num;
    if (whole == 0) return 0;
    num = (part << 6) + (part << 5) + (part << 2);
    while (whole > 65535UL) {
        num >>= 1;
        whole >>= 1;
    }
    if ((uint16_t)whole == 0) return 0;
    return (uint16_t)u32_div_u16(num, (uint16_t)whole, 0);
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

static void print_hex16(uint16_t val) {
    static const char hex[] = "0123456789ABCDEF";
    os_putc(hex[(val >> 12) & 0x0F]);
    os_putc(hex[(val >> 8) & 0x0F]);
    os_putc(hex[(val >> 4) & 0x0F]);
    os_putc(hex[val & 0x0F]);
}

static uint16_t har_crc16_update(uint16_t crc, const uint8_t __far *buf, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint8_t b = buf[i];
        uint8_t j;
        crc ^= (uint16_t)b;
        for (j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint16_t har_crc16(const uint8_t __far *buf, uint16_t len) {
    return har_crc16_update(0xFFFF, buf, len);
}

static uint16_t lz4_hash(uint16_t w0, uint16_t w1) {
    uint16_t h = (w0 * 40503U) ^ w1;
    return (h >> (16 - HASH_LOG)) & HASH_MASK;
}

static uint16_t lz4_compress_block(const uint8_t __far *src, uint16_t src_len,
                                  uint8_t __far *dst, uint16_t dst_cap,
                                  uint16_t __far *hash_table)
{
    uint16_t ip = 0;
    uint16_t anchor = 0;
    uint16_t op = 0;
    uint16_t mflimit;
    uint16_t i;

    if (src_len < 13) {
        uint16_t lit_len = src_len;
        if (op + lit_len + 5 > dst_cap) return 0;
        if (lit_len >= 15) {
            uint16_t rem = lit_len - 15;
            dst[op++] = 0xF0;
            while (rem >= 255) {
                dst[op++] = 255;
                rem -= 255;
            }
            dst[op++] = (uint8_t)rem;
        } else {
            dst[op++] = (uint8_t)(lit_len << 4);
        }
        for (i = 0; i < lit_len; i++) dst[op++] = src[i];
        return op;
    }

    mflimit = src_len - 12;

    for (i = 0; i < HASH_SIZE; i++) {
        hash_table[i] = 0xFFFF;
    }

    while (ip < mflimit) {
        uint16_t w0 = (uint16_t)src[ip] | ((uint16_t)src[ip + 1] << 8);
        uint16_t w1 = (uint16_t)src[ip + 2] | ((uint16_t)src[ip + 3] << 8);
        uint16_t h = lz4_hash(w0, w1);
        uint16_t ref = hash_table[h];
        hash_table[h] = ip;

        if (ref != 0xFFFF && (ip - ref) < 65535U && (ip - ref) > 0) {
            if (src[ref] == src[ip] &&
                src[ref + 1] == src[ip + 1] &&
                src[ref + 2] == src[ip + 2] &&
                src[ref + 3] == src[ip + 3])
            {
                uint16_t match_len = 4;
                uint16_t lit_len = ip - anchor;
                uint16_t offset = ip - ref;
                uint8_t token;
                uint16_t token_pos;

                while ((ip + match_len < src_len - 5) &&
                       (src[ref + match_len] == src[ip + match_len])) {
                    match_len++;
                }

                token_pos = op++;
                if (token_pos >= dst_cap) return 0;

                if (lit_len >= 15) {
                    uint16_t rem = lit_len - 15;
                    token = 0xF0;
                    while (rem >= 255) {
                        if (op >= dst_cap) return 0;
                        dst[op++] = 255;
                        rem -= 255;
                    }
                    if (op >= dst_cap) return 0;
                    dst[op++] = (uint8_t)rem;
                } else {
                    token = (uint8_t)(lit_len << 4);
                }

                if (op + lit_len + 2 > dst_cap) return 0;
                for (i = 0; i < lit_len; i++) {
                    dst[op++] = src[anchor + i];
                }

                dst[op++] = (uint8_t)(offset & 0xFF);
                dst[op++] = (uint8_t)(offset >> 8);

                if (match_len >= 19) {
                    uint16_t rem = match_len - 19;
                    token |= 0x0F;
                    while (rem >= 255) {
                        if (op >= dst_cap) return 0;
                        dst[op++] = 255;
                        rem -= 255;
                    }
                    if (op >= dst_cap) return 0;
                    dst[op++] = (uint8_t)rem;
                } else {
                    token |= (uint8_t)(match_len - 4);
                }

                dst[token_pos] = token;

                ip += match_len;
                anchor = ip;
                continue;
            }
        }
        ip++;
    }

    /* Trailing literals */
    {
        uint16_t lit_len = src_len - anchor;
        if (op + lit_len + 5 > dst_cap) return 0;
        if (lit_len >= 15) {
            uint16_t rem = lit_len - 15;
            dst[op++] = 0xF0;
            while (rem >= 255) {
                dst[op++] = 255;
                rem -= 255;
            }
            dst[op++] = (uint8_t)rem;
        } else {
            dst[op++] = (uint8_t)(lit_len << 4);
        }
        for (i = 0; i < lit_len; i++) {
            dst[op++] = src[anchor + i];
        }
    }

    return op;
}

static uint16_t lz4_decompress_block(const uint8_t __far *src, uint16_t src_len,
                                    uint8_t __far *dst, uint16_t dst_cap)
{
    uint16_t ip = 0;
    uint16_t op = 0;

    while (ip < src_len) {
        uint8_t token = src[ip++];
        uint16_t lit_len = token >> 4;
        uint16_t match_len;
        uint16_t offset;
        uint16_t i;

        if (lit_len == 15) {
            uint8_t b;
            do {
                if (ip >= src_len) return 0;
                b = src[ip++];
                lit_len += b;
            } while (b == 255);
        }

        if (op + lit_len > dst_cap || ip + lit_len > src_len) return 0;
        for (i = 0; i < lit_len; i++) {
            dst[op++] = src[ip++];
        }

        if (ip >= src_len) {
            break;
        }

        if (ip + 2 > src_len) return 0;
        offset = (uint16_t)src[ip] | ((uint16_t)src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) return 0;

        match_len = (token & 0x0F) + 4;
        if ((token & 0x0F) == 15) {
            uint8_t b;
            do {
                if (ip >= src_len) return 0;
                b = src[ip++];
                match_len += b;
            } while (b == 255);
        }

        if (op + match_len > dst_cap) return 0;
        for (i = 0; i < match_len; i++) {
            dst[op] = dst[op - offset];
            op++;
        }
    }
    return op;
}

static char char_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static int wildcard_match(const char *pattern, const char *text) {
    const char *p = pattern;
    const char *t = text;
    const char *p_star = 0;
    const char *t_star = 0;

    while (*t) {
        if (*p == '?' || char_upper(*p) == char_upper(*t)) {
            p++;
            t++;
        } else if (*p == '*') {
            p_star = p++;
            t_star = t;
        } else if (p_star) {
            p = p_star + 1;
            t = ++t_star;
        } else {
            return 0;
        }
    }
    while (*p == '*') {
        p++;
    }
    return (*p == '\0');
}

static int has_wildcards(const char *str) {
    if (!str) return 0;
    while (*str) {
        if (*str == '*' || *str == '?') return 1;
        str++;
    }
    return 0;
}

static const char *path_basename(const char *path) {
    const char *base = path;
    while (*path) {
        if (*path == '/' || *path == '\\' || *path == ':') {
            base = path + 1;
        }
        path++;
    }
    return base;
}

static void split_path_pattern(const char *input, char *dir_out, char *pat_out) {
    const char *last_sep = 0;
    const char *p = input;

    while (*p) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            last_sep = p;
        }
        p++;
    }

    if (last_sep) {
        int dir_len = (int)(last_sep - input + 1);
        int i;
        for (i = 0; i < dir_len; i++) {
            dir_out[i] = input[i];
        }
        dir_out[dir_len] = '\0';
        p = last_sep + 1;
        i = 0;
        while (*p) {
            pat_out[i++] = *p++;
        }
        pat_out[i] = '\0';
    } else {
        int i = 0;
        dir_out[0] = '\0';
        p = input;
        while (*p) {
            pat_out[i++] = *p++;
        }
        pat_out[i] = '\0';
    }
}

static void combine_dir_file(const char *dir, const char *file, char *out) {
    int i = 0;
    if (dir && dir[0] != '\0') {
        while (*dir) {
            out[i++] = *dir++;
        }
        if (out[i - 1] != '/' && out[i - 1] != '\\' && out[i - 1] != ':') {
            out[i++] = '/';
        }
    }
    while (*file) {
        out[i++] = *file++;
    }
    out[i] = '\0';
}

static int is_dir_path(const char *path) {
    os_dir_entry_t dent;
    if (!path || path[0] == '\0') return 0;
    if ((path[0] == '/' || path[0] == '\\') && path[1] == '\0') return 1;
    return (os_dir_entry_at(path, 0, (os_dir_entry_t __far *)&dent) != -2);
}

static void get_item_arc_name(const char *disk_path, char *arc_out) {
    const char *p = disk_path;
    const char *colon = disk_path;
    int i = 0;

    /* Check if disk_path starts with a device prefix like "RAM:" or "SYS:" */
    while (*colon && *colon != '/' && *colon != '\\') {
        if (*colon == ':') {
            p = colon + 1;
            break;
        }
        colon++;
    }

    /* Skip leading slashes */
    while (*p == '/' || *p == '\\') p++;

    /* If path is "." or "./", arc_out should be empty */
    if (p[0] == '.' && (p[1] == '\0' || p[1] == '/' || p[1] == '\\')) {
        arc_out[0] = '\0';
        return;
    }

    while (*p && i < 63) {
        arc_out[i++] = *p++;
    }
    if (i > 0 && (arc_out[i - 1] == '/' || arc_out[i - 1] == '\\')) {
        i--;
    }
    arc_out[i] = '\0';
}

static void ensure_parent_dirs(char *path) {
    char *p = path;
    const char *colon = path;

    /* Skip device name if present */
    while (*colon && *colon != '/' && *colon != '\\') {
        if (*colon == ':') {
            p = (char *)colon + 1;
            break;
        }
        colon++;
    }

    /* Skip leading slash */
    while (*p == '/' || *p == '\\') p++;

    while (*p) {
        if (*p == '/' || *p == '\\') {
            char orig = *p;
            *p = '\0';
            os_mkdir(path);
            *p = orig;
        }
        p++;
    }
}

static void print_usage(void) {
    uint16_t free_ems = os_ems_avail();
    uint16_t tot_ems  = os_ems_total();
    uint16_t free_xms = os_xms_avail();
    uint16_t tot_xms  = os_xms_total();

    os_puts("HobbyOS Archiver (HAR) v2.0 - Chunked LZ4 Streaming\n");
    os_puts("Usage:\n");
    os_puts("  har -c <archive.har> <files|dirs...>  Create archive (recursive dirs & wildcards)\n");
    os_puts("  har -a <archive.har> <files|dirs...>  Append files/dirs to archive\n");
    os_puts("  har -x <archive.har> [dest_dir]       Extract files and restore directory trees\n");
    os_puts("  har -t <archive.har>                  Test archive data integrity (CRC)\n");
    os_puts("  har -l <archive.har>                  List archive contents and ratios\n\n");

    os_puts("Memory: ");
    print_uint32((uint32_t)os_mem_avail() << 4);
    os_puts(" bytes conventional free");

    if (tot_ems > 0) {
        os_puts(" | EMS: ");
        print_uint32((uint32_t)free_ems << 4);
        os_puts(" KB available");
    }
    if (tot_xms > 0) {
        os_puts(" | XMS: ");
        print_uint32(free_xms);
        os_puts(" KB available");
    }
    os_putc('\n');
}

/* Working buffers allocated in far memory */
static uint16_t raw_seg = 0;
static uint16_t comp_seg = 0;
static uint16_t hash_seg = 0;

static uint8_t __far *raw_buf = 0;
static uint8_t __far *comp_buf = 0;
static uint16_t __far *hash_tbl = 0;

static int alloc_buffers(void) {
    raw_seg = os_mem_alloc(BUF_PARAS_32K);
    comp_seg = os_mem_alloc(BUF_PARAS_32K);
    hash_seg = os_mem_alloc(HASH_PARAS_8K);

    if (!raw_seg || !comp_seg || !hash_seg) {
        os_puts("Error: Insufficient conventional memory for archiver buffers.\n");
        if (raw_seg)  os_mem_free(raw_seg);
        if (comp_seg) os_mem_free(comp_seg);
        if (hash_seg) os_mem_free(hash_seg);
        return -1;
    }

    raw_buf  = (uint8_t __far *)MK_FP(raw_seg, 0);
    comp_buf = (uint8_t __far *)MK_FP(comp_seg, 0);
    hash_tbl = (uint16_t __far *)MK_FP(hash_seg, 0);
    return 0;
}

static void free_buffers(void) {
    if (raw_seg)  os_mem_free(raw_seg);
    if (comp_seg) os_mem_free(comp_seg);
    if (hash_seg) os_mem_free(hash_seg);
    raw_seg = comp_seg = hash_seg = 0;
}

static int append_single_file(const char *arc_name, const char *file_path,
                              const char *arc_path, uint16_t *total_count)
{
    uint8_t name_len;
    long sz;
    har_entry_t entry;
    uint8_t hdr_buf[80];
    uint16_t i;

    name_len = 0;
    while (arc_path[name_len] && name_len < 64) {
        name_len++;
    }
    if (name_len == 0) return 0;

    sz = os_file_size(file_path);
    if (sz < 0) {
        os_puts("Warning: File not found: ");
        os_puts(file_path);
        os_putc('\n');
        return -1;
    }

    if (sz <= (long)BUF_BYTES_32K) {
        uint16_t read_bytes = (uint16_t)sz;
        uint16_t comp_bytes = 0;
        uint16_t is_comp = 0;
        uint16_t crc = 0xFFFF;
        har_chunk_hdr_t chk;

        if (read_bytes > 0) {
            uint16_t r = os_file_read(file_path, raw_buf, read_bytes);
            if (r == 0xFFFF || r != read_bytes) {
                os_puts("Error reading: ");
                os_puts(file_path);
                os_putc('\n');
                return -1;
            }
            crc = har_crc16(raw_buf, read_bytes);

            if (read_bytes > 12) {
                comp_bytes = lz4_compress_block(raw_buf, read_bytes, comp_buf, BUF_BYTES_32K, hash_tbl);
                if (comp_bytes > 0 && comp_bytes < read_bytes) {
                    is_comp = 1;
                } else {
                    is_comp = 0;
                    comp_bytes = read_bytes;
                }
            } else {
                is_comp = 0;
                comp_bytes = read_bytes;
            }
            chk.comp_len = comp_bytes;
            chk.raw_len  = read_bytes;
            entry.comp_size = (uint32_t)sizeof(har_chunk_hdr_t) + comp_bytes;
        } else {
            entry.comp_size = 0;
        }

        entry.magic[0]    = 'H';
        entry.magic[1]    = 'F';
        entry.attr        = OS_ATTR_ARCHIVE;
        entry.name_len    = name_len;
        entry.uncomp_size = read_bytes;
        entry.flags       = is_comp ? HAR_FLAG_COMPRESSED : 0;
        entry.crc         = crc;

        for (i = 0; i < sizeof(har_entry_t); i++) {
            hdr_buf[i] = ((const uint8_t *)&entry)[i];
        }
        for (i = 0; i < name_len; i++) {
            hdr_buf[sizeof(har_entry_t) + i] = (uint8_t)arc_path[i];
        }

        if (os_file_append(arc_name, (const void __far *)hdr_buf, sizeof(har_entry_t) + name_len) != sizeof(har_entry_t) + name_len) {
            os_puts("Error appending entry header to ");
            os_puts(arc_name);
            os_putc('\n');
            return -1;
        }

        if (read_bytes > 0) {
            const void __far *payload = is_comp ? (const void __far *)comp_buf : (const void __far *)raw_buf;
            if (os_file_append(arc_name, (const void __far *)&chk, sizeof(har_chunk_hdr_t)) != sizeof(har_chunk_hdr_t) ||
                os_file_append(arc_name, payload, comp_bytes) != comp_bytes)
            {
                os_puts("Error appending payload to ");
                os_puts(arc_name);
                os_putc('\n');
                return -1;
            }
        }
    } else {
        /* Multi-chunk streaming via single-pass backpatch */
        uint32_t entry_hdr_off = (uint32_t)os_file_size(arc_name);
        uint32_t in_offset = 0;
        uint32_t total_comp_payload = 0;
        uint16_t crc = 0xFFFF;
        int any_compressed = 0;

        entry.magic[0]    = 'H';
        entry.magic[1]    = 'F';
        entry.attr        = OS_ATTR_ARCHIVE;
        entry.name_len    = name_len;
        entry.uncomp_size = (uint32_t)sz;
        entry.comp_size   = 0;
        entry.flags       = 0;
        entry.crc         = 0;

        for (i = 0; i < sizeof(har_entry_t); i++) {
            hdr_buf[i] = ((const uint8_t *)&entry)[i];
        }
        for (i = 0; i < name_len; i++) {
            hdr_buf[sizeof(har_entry_t) + i] = (uint8_t)arc_path[i];
        }

        if (os_file_append(arc_name, (const void __far *)hdr_buf, sizeof(har_entry_t) + name_len) != sizeof(har_entry_t) + name_len) {
            os_puts("Error appending entry header to ");
            os_puts(arc_name);
            os_putc('\n');
            return -1;
        }

        while (in_offset < (uint32_t)sz) {
            uint16_t to_read = (sz - in_offset > (long)BUF_BYTES_32K) ? BUF_BYTES_32K : (uint16_t)(sz - in_offset);
            uint16_t r = os_file_read_offset(file_path, raw_buf, to_read, in_offset);
            uint16_t comp_bytes = 0;
            uint16_t is_comp = 0;
            har_chunk_hdr_t chk;
            const void __far *payload;

            if (r == 0xFFFF || r != to_read) {
                os_puts("Error reading: ");
                os_puts(file_path);
                os_putc('\n');
                return -1;
            }

            crc = har_crc16_update(crc, raw_buf, r);

            if (r > 12) {
                comp_bytes = lz4_compress_block(raw_buf, r, comp_buf, BUF_BYTES_32K, hash_tbl);
                if (comp_bytes > 0 && comp_bytes < r) {
                    is_comp = 1;
                    any_compressed = 1;
                } else {
                    is_comp = 0;
                    comp_bytes = r;
                }
            } else {
                is_comp = 0;
                comp_bytes = r;
            }

            chk.comp_len = comp_bytes;
            chk.raw_len  = r;
            payload = is_comp ? (const void __far *)comp_buf : (const void __far *)raw_buf;

            if (os_file_append(arc_name, (const void __far *)&chk, sizeof(har_chunk_hdr_t)) != sizeof(har_chunk_hdr_t) ||
                os_file_append(arc_name, payload, comp_bytes) != comp_bytes)
            {
                os_puts("Error appending payload to ");
                os_puts(arc_name);
                os_putc('\n');
                return -1;
            }

            total_comp_payload += (uint32_t)sizeof(har_chunk_hdr_t) + comp_bytes;
            in_offset += r;
        }

        entry.comp_size   = total_comp_payload;
        entry.flags       = any_compressed ? HAR_FLAG_COMPRESSED : 0;
        entry.crc         = crc;

        for (i = 0; i < sizeof(har_entry_t); i++) {
            hdr_buf[i] = ((const uint8_t *)&entry)[i];
        }

        if (os_file_write_offset(arc_name, (const void __far *)hdr_buf, sizeof(har_entry_t), entry_hdr_off) != sizeof(har_entry_t)) {
            os_puts("Error backpatching entry header in ");
            os_puts(arc_name);
            os_putc('\n');
            return -1;
        }
    }

    os_puts("  Adding: ");
    os_puts(arc_path);
    os_puts(" (");
    print_uint32((uint32_t)sz);
    os_puts(" -> ");
    print_uint32(entry.comp_size);
    os_puts(" bytes, ");
    if (sz > 0) {
        uint16_t ratio = calc_ratio(entry.comp_size, (uint32_t)sz);
        print_uint32(ratio);
        os_puts("% ");
    } else {
        os_puts("0% ");
    }
    os_puts((entry.flags & HAR_FLAG_COMPRESSED) ? "LZ4)\n" : "stored)\n");

    (*total_count)++;
    return 0;
}

static void archive_directory(const char *arc_name, const char *disk_dir,
                              const char *arc_prefix, uint16_t *total_count)
{
    os_dir_entry_t dent;
    uint16_t idx = 0;

    while (os_dir_entry_at(disk_dir, idx++, (os_dir_entry_t __far *)&dent) == 0) {
        char full_disk[80];
        char full_arc[80];

        if (dent.name[0] == '.') continue;

        combine_dir_file(disk_dir, dent.name, full_disk);
        if (arc_prefix && arc_prefix[0] != '\0') {
            combine_dir_file(arc_prefix, dent.name, full_arc);
        } else {
            int k = 0;
            while (dent.name[k]) {
                full_arc[k] = dent.name[k];
                k++;
            }
            full_arc[k] = '\0';
        }

        if (dent.attr & OS_ATTR_DIRECTORY) {
            archive_directory(arc_name, full_disk, full_arc, total_count);
        } else {
            append_single_file(arc_name, full_disk, full_arc, total_count);
        }
    }
}

static int archive_item(const char *arc_name, const char *arg, uint16_t *total_added) {
    if (has_wildcards(arg)) {
        char dir_buf[64];
        char pat_buf[16];
        const char *dir_arg;
        os_dir_entry_t dent;
        uint16_t idx = 0;

        split_path_pattern(arg, dir_buf, pat_buf);
        dir_arg = (dir_buf[0] != '\0') ? dir_buf : 0;

        while (os_dir_entry_at(dir_arg, idx++, (os_dir_entry_t __far *)&dent) == 0) {
            if (dent.name[0] == '.') continue;
            if (wildcard_match(pat_buf, dent.name)) {
                char full_src[80];
                char full_arc[80];
                combine_dir_file(dir_buf, dent.name, full_src);
                get_item_arc_name(full_src, full_arc);
                if (dent.attr & OS_ATTR_DIRECTORY) {
                    archive_directory(arc_name, full_src, full_arc, total_added);
                } else {
                    append_single_file(arc_name, full_src, full_arc, total_added);
                }
            }
        }
    } else if (is_dir_path(arg)) {
        char arc_prefix[80];
        get_item_arc_name(arg, arc_prefix);
        archive_directory(arc_name, arg, arc_prefix, total_added);
    } else {
        char full_arc[80];
        get_item_arc_name(arg, full_arc);
        append_single_file(arc_name, arg, full_arc, total_added);
    }
    return 0;
}

static int cmd_create(const char *arc_name, int num_files, const char **files) {
    har_header_t hdr;
    uint16_t total_added = 0;
    int f;

    if (alloc_buffers() != 0) return 1;

    os_file_delete(arc_name);

    hdr.magic[0] = 'H';
    hdr.magic[1] = 'A';
    hdr.magic[2] = 'R';
    hdr.magic[3] = '2';
    hdr.version  = 2;
    hdr.flags    = 0;

    if (os_file_write(arc_name, (const void __far *)&hdr, sizeof(har_header_t)) != sizeof(har_header_t)) {
        os_puts("Error creating archive file: ");
        os_puts(arc_name);
        os_putc('\n');
        free_buffers();
        return 1;
    }

    os_puts("Creating archive: ");
    os_puts(arc_name);
    os_putc('\n');

    for (f = 0; f < num_files; f++) {
        archive_item(arc_name, files[f], &total_added);
    }

    os_puts("Archive created: ");
    print_uint32(total_added);
    os_puts(" file(s) stored.\n");

    free_buffers();
    return 0;
}

static int cmd_append(const char *arc_name, int num_files, const char **files) {
    long sz;
    har_header_t hdr;
    uint16_t total_added = 0;
    int f;

    sz = os_file_size(arc_name);
    if (sz < 0) {
        return cmd_create(arc_name, num_files, files);
    }

    if (sz < (long)sizeof(har_header_t)) {
        os_puts("Error: Invalid archive file: ");
        os_puts(arc_name);
        os_putc('\n');
        return 1;
    }

    if (os_file_read_offset(arc_name, (void __far *)&hdr, sizeof(har_header_t), 0) != sizeof(har_header_t)) {
        os_puts("Error reading archive header.\n");
        return 1;
    }

    if (hdr.magic[0] != 'H' || hdr.magic[1] != 'A' || hdr.magic[2] != 'R') {
        os_puts("Error: Not a valid HAR archive.\n");
        return 1;
    }
    if (hdr.magic[3] == '1' || hdr.version == 1) {
        os_puts("Error: Cannot append to legacy HAR1 archive. Please recreate with 'har -c'.\n");
        return 1;
    }
    if (hdr.magic[3] != '2' && hdr.version != 2) {
        os_puts("Error: Unsupported HAR archive version.\n");
        return 1;
    }

    if (alloc_buffers() != 0) return 1;

    os_puts("Appending to archive: ");
    os_puts(arc_name);
    os_putc('\n');

    for (f = 0; f < num_files; f++) {
        archive_item(arc_name, files[f], &total_added);
    }

    os_puts("Archive updated: ");
    print_uint32(total_added);
    os_puts(" file(s) added.\n");

    free_buffers();
    return 0;
}

static int cmd_list_or_test(const char *arc_name, int is_test) {
    long arc_sz;
    har_header_t hdr;
    uint32_t offset = 0;
    uint16_t total_files = 0;
    uint32_t total_uncomp = 0;
    uint32_t total_comp = 0;
    int has_errors = 0;
    int is_har1 = 0;

    arc_sz = os_file_size(arc_name);
    if (arc_sz < (long)sizeof(har_header_t)) {
        os_puts("Error: Archive not found or empty: ");
        os_puts(arc_name);
        os_putc('\n');
        return 1;
    }

    if (alloc_buffers() != 0) return 1;

    if (os_file_read_offset(arc_name, (void __far *)&hdr, sizeof(har_header_t), 0) != sizeof(har_header_t)) {
        os_puts("Error reading archive header.\n");
        free_buffers();
        return 1;
    }

    if (hdr.magic[0] != 'H' || hdr.magic[1] != 'A' || hdr.magic[2] != 'R') {
        os_puts("Error: Not a valid HAR archive (invalid magic).\n");
        free_buffers();
        return 1;
    }

    if (hdr.magic[3] == '1' || hdr.version == 1) {
        is_har1 = 1;
    } else if (hdr.magic[3] == '2' || hdr.version == 2) {
        is_har1 = 0;
    } else {
        os_puts("Error: Unsupported HAR archive version.\n");
        free_buffers();
        return 1;
    }

    offset = sizeof(har_header_t);

    if (!is_test) {
        os_puts("Archive: ");
        os_puts(arc_name);
        os_putc('\n');
        os_puts("  Length   Method   Size     Ratio   CRC16  Name\n");
        os_puts("  -------- -------- -------- ------  -----  ----------------\n");
    }

    for (;;) {
        har_entry_t entry;
        char name_buf[65];
        int r;

        if (offset >= (uint32_t)arc_sz) break;

        r = os_file_read_offset(arc_name, (void __far *)&entry, sizeof(har_entry_t), offset);
        if (r < (int)sizeof(har_entry_t)) break;

        if (entry.magic[0] == 'H' && entry.magic[1] == 'E') {
            offset += sizeof(har_end_t);
            continue;
        }
        if (entry.magic[0] != 'H' || entry.magic[1] != 'F') {
            os_puts("Error: Archive corrupted at offset ");
            print_uint32(offset);
            os_putc('\n');
            has_errors = 1;
            break;
        }

        offset += sizeof(har_entry_t);

        if (entry.name_len > 64) entry.name_len = 64;
        os_file_read_offset(arc_name, (void __far *)name_buf, entry.name_len, offset);
        name_buf[entry.name_len] = '\0';
        offset += entry.name_len;

        if (is_test) {
            uint16_t crc_calc = 0xFFFF;
            int ok = 1;

            os_puts("Testing: ");
            os_puts(name_buf);

            if (is_har1) {
                if (entry.comp_size > BUF_BYTES_32K) {
                    os_puts(" - FAILED (payload exceeds buffer)\n");
                    ok = 0;
                    has_errors = 1;
                } else if (entry.comp_size > 0) {
                    r = os_file_read_offset(arc_name, comp_buf, (uint16_t)entry.comp_size, offset);
                    if ((uint16_t)r != (uint16_t)entry.comp_size) {
                        os_puts(" - FAILED (read error)\n");
                        ok = 0;
                        has_errors = 1;
                    } else if (entry.flags & HAR_FLAG_COMPRESSED) {
                        uint16_t dlen = lz4_decompress_block(comp_buf, (uint16_t)entry.comp_size,
                                                            raw_buf, BUF_BYTES_32K);
                        if (dlen != (uint16_t)entry.uncomp_size) {
                            os_puts(" - FAILED (decompression size mismatch)\n");
                            ok = 0;
                            has_errors = 1;
                        } else {
                            crc_calc = har_crc16(raw_buf, dlen);
                        }
                    } else {
                        crc_calc = har_crc16(comp_buf, (uint16_t)entry.comp_size);
                    }
                }
            } else {
                /* HAR2 chunked streaming verification */
                uint32_t bytes_done = 0;
                uint32_t payload_off = offset;

                while (bytes_done < entry.uncomp_size) {
                    har_chunk_hdr_t chk;
                    r = os_file_read_offset(arc_name, (void __far *)&chk, sizeof(har_chunk_hdr_t), payload_off);
                    if (r < (int)sizeof(har_chunk_hdr_t)) {
                        os_puts(" - FAILED (chunk header read error)\n");
                        ok = 0;
                        has_errors = 1;
                        break;
                    }
                    payload_off += sizeof(har_chunk_hdr_t);

                    if (chk.comp_len > BUF_BYTES_32K) {
                        os_puts(" - FAILED (chunk size exceeds buffer)\n");
                        ok = 0;
                        has_errors = 1;
                        break;
                    }

                    r = os_file_read_offset(arc_name, comp_buf, chk.comp_len, payload_off);
                    if ((uint16_t)r != chk.comp_len) {
                        os_puts(" - FAILED (chunk payload read error)\n");
                        ok = 0;
                        has_errors = 1;
                        break;
                    }
                    payload_off += chk.comp_len;

                    if (chk.comp_len < chk.raw_len) {
                        uint16_t dlen = lz4_decompress_block(comp_buf, chk.comp_len, raw_buf, BUF_BYTES_32K);
                        if (dlen != chk.raw_len) {
                            os_puts(" - FAILED (decompression error)\n");
                            ok = 0;
                            has_errors = 1;
                            break;
                        }
                        crc_calc = har_crc16_update(crc_calc, raw_buf, dlen);
                    } else {
                        crc_calc = har_crc16_update(crc_calc, comp_buf, chk.raw_len);
                    }
                    bytes_done += chk.raw_len;
                }
            }

            if (ok) {
                if (crc_calc != entry.crc) {
                    os_puts(" - FAILED (CRC16 mismatch: expected ");
                    print_hex16(entry.crc);
                    os_puts(", got ");
                    print_hex16(crc_calc);
                    os_puts(")\n");
                    has_errors = 1;
                } else {
                    os_puts(" - OK\n");
                }
            }
        } else {
            uint16_t ratio = calc_ratio(entry.comp_size, entry.uncomp_size);
            os_puts("  ");
            print_uint32_padded(entry.uncomp_size, 8);
            os_puts((entry.flags & HAR_FLAG_COMPRESSED) ? " LZ4      " : " Stored   ");
            print_uint32_padded(entry.comp_size, 8);
            os_puts("  ");
            print_uint32_padded(ratio, 3);
            os_puts("%   ");
            print_hex16(entry.crc);
            os_puts("  ");
            os_puts(name_buf);
            os_putc('\n');
        }

        total_files++;
        total_uncomp += entry.uncomp_size;
        total_comp += entry.comp_size;
        offset += entry.comp_size;
    }

    if (!is_test) {
        uint16_t total_ratio = calc_ratio(total_comp, total_uncomp);
        os_puts("  -------- -------- -------- ------  -----  ----------------\n");
        os_puts("  ");
        print_uint32_padded(total_uncomp, 8);
        os_puts("          ");
        print_uint32_padded(total_comp, 8);
        os_puts("  ");
        print_uint32_padded(total_ratio, 3);
        os_puts("%          ");
        print_uint32(total_files);
        os_puts(" file(s)\n");
    } else {
        if (!has_errors) {
            os_puts("All ");
            print_uint32(total_files);
            os_puts(" file(s) tested OK.\n");
        } else {
            os_puts("Archive test finished with errors.\n");
        }
    }

    free_buffers();
    return has_errors ? 1 : 0;
}

static int cmd_extract(const char *arc_name, const char *dest_dir) {
    long arc_sz;
    har_header_t hdr;
    uint32_t offset = 0;
    uint16_t total_extracted = 0;
    int has_errors = 0;
    int is_har1 = 0;

    arc_sz = os_file_size(arc_name);
    if (arc_sz < (long)sizeof(har_header_t)) {
        os_puts("Error: Archive not found or empty: ");
        os_puts(arc_name);
        os_putc('\n');
        return 1;
    }

    if (alloc_buffers() != 0) return 1;

    if (os_file_read_offset(arc_name, (void __far *)&hdr, sizeof(har_header_t), 0) != sizeof(har_header_t)) {
        os_puts("Error reading archive header.\n");
        free_buffers();
        return 1;
    }

    if (hdr.magic[0] != 'H' || hdr.magic[1] != 'A' || hdr.magic[2] != 'R') {
        os_puts("Error: Not a valid HAR archive.\n");
        free_buffers();
        return 1;
    }

    if (hdr.magic[3] == '1' || hdr.version == 1) {
        is_har1 = 1;
    } else if (hdr.magic[3] == '2' || hdr.version == 2) {
        is_har1 = 0;
    } else {
        os_puts("Error: Unsupported HAR archive version.\n");
        free_buffers();
        return 1;
    }

    offset = sizeof(har_header_t);

    os_puts("Extracting archive: ");
    os_puts(arc_name);
    if (dest_dir && dest_dir[0] != '\0') {
        os_puts(" to ");
        os_puts(dest_dir);
    }
    os_putc('\n');

    for (;;) {
        har_entry_t entry;
        char name_buf[65];
        char out_path[96];
        int r;

        if (offset >= (uint32_t)arc_sz) break;

        r = os_file_read_offset(arc_name, (void __far *)&entry, sizeof(har_entry_t), offset);
        if (r < (int)sizeof(har_entry_t)) break;

        if (entry.magic[0] == 'H' && entry.magic[1] == 'E') {
            offset += sizeof(har_end_t);
            continue;
        }
        if (entry.magic[0] != 'H' || entry.magic[1] != 'F') {
            os_puts("Error: Archive corrupted at offset ");
            print_uint32(offset);
            os_putc('\n');
            has_errors = 1;
            break;
        }

        offset += sizeof(har_entry_t);

        if (entry.name_len > 64) entry.name_len = 64;
        os_file_read_offset(arc_name, (void __far *)name_buf, entry.name_len, offset);
        name_buf[entry.name_len] = '\0';
        offset += entry.name_len;

        combine_dir_file(dest_dir, name_buf, out_path);

        os_puts("  Extracting: ");
        os_puts(name_buf);

        if (is_har1) {
            uint16_t crc_calc = 0xFFFF;
            if (entry.comp_size > BUF_BYTES_32K) {
                os_puts(" - FAILED (payload exceeds buffer)\n");
                has_errors = 1;
            } else {
                if (entry.comp_size > 0) {
                    r = os_file_read_offset(arc_name, comp_buf, (uint16_t)entry.comp_size, offset);
                    if ((uint16_t)r != (uint16_t)entry.comp_size) {
                        os_puts(" - FAILED (read error)\n");
                        has_errors = 1;
                        offset += entry.comp_size;
                        continue;
                    }

                    if (entry.flags & HAR_FLAG_COMPRESSED) {
                        uint16_t dlen = lz4_decompress_block(comp_buf, (uint16_t)entry.comp_size,
                                                            raw_buf, BUF_BYTES_32K);
                        if (dlen != (uint16_t)entry.uncomp_size) {
                            os_puts(" - FAILED (decompression error)\n");
                            has_errors = 1;
                            offset += entry.comp_size;
                            continue;
                        }
                        crc_calc = har_crc16(raw_buf, dlen);
                    } else {
                        crc_calc = har_crc16(comp_buf, (uint16_t)entry.comp_size);
                    }
                }

                if (crc_calc != entry.crc) {
                    os_puts(" - FAILED (CRC mismatch)\n");
                    has_errors = 1;
                } else {
                    const void __far *out_data = (entry.flags & HAR_FLAG_COMPRESSED) ?
                                                (const void __far *)raw_buf : (const void __far *)comp_buf;
                    ensure_parent_dirs(out_path);
                    if (os_file_write(out_path, out_data, (uint16_t)entry.uncomp_size) != (uint16_t)entry.uncomp_size) {
                        os_puts(" - FAILED (write error)\n");
                        has_errors = 1;
                    } else {
                        os_puts(" (");
                        print_uint32(entry.uncomp_size);
                        os_puts(" bytes) - OK\n");
                        total_extracted++;
                    }
                }
            }
        } else {
            /* HAR2 chunked streaming extraction */
            uint32_t bytes_done = 0;
            uint32_t payload_off = offset;
            uint16_t crc_calc = 0xFFFF;
            int ok = 1;

            ensure_parent_dirs(out_path);

            if (entry.uncomp_size == 0) {
                if (os_file_write(out_path, (const void __far *)raw_buf, 0) != 0) {
                    os_puts(" - FAILED (write error)\n");
                    has_errors = 1;
                    ok = 0;
                }
            }

            while (bytes_done < entry.uncomp_size) {
                har_chunk_hdr_t chk;
                const void __far *out_ptr;
                uint16_t w;

                r = os_file_read_offset(arc_name, (void __far *)&chk, sizeof(har_chunk_hdr_t), payload_off);
                if (r < (int)sizeof(har_chunk_hdr_t)) {
                    os_puts(" - FAILED (chunk header read error)\n");
                    has_errors = 1;
                    ok = 0;
                    break;
                }
                payload_off += sizeof(har_chunk_hdr_t);

                if (chk.comp_len > BUF_BYTES_32K) {
                    os_puts(" - FAILED (chunk size exceeds buffer)\n");
                    has_errors = 1;
                    ok = 0;
                    break;
                }

                r = os_file_read_offset(arc_name, comp_buf, chk.comp_len, payload_off);
                if ((uint16_t)r != chk.comp_len) {
                    os_puts(" - FAILED (chunk payload read error)\n");
                    has_errors = 1;
                    ok = 0;
                    break;
                }
                payload_off += chk.comp_len;

                if (chk.comp_len < chk.raw_len) {
                    uint16_t dlen = lz4_decompress_block(comp_buf, chk.comp_len, raw_buf, BUF_BYTES_32K);
                    if (dlen != chk.raw_len) {
                        os_puts(" - FAILED (decompression error)\n");
                        has_errors = 1;
                        ok = 0;
                        break;
                    }
                    crc_calc = har_crc16_update(crc_calc, raw_buf, dlen);
                    out_ptr = (const void __far *)raw_buf;
                } else {
                    crc_calc = har_crc16_update(crc_calc, comp_buf, chk.raw_len);
                    out_ptr = (const void __far *)comp_buf;
                }

                if (bytes_done == 0) {
                    w = os_file_write(out_path, out_ptr, chk.raw_len);
                } else {
                    w = os_file_append(out_path, out_ptr, chk.raw_len);
                }

                if (w != chk.raw_len) {
                    os_puts(" - FAILED (write error)\n");
                    has_errors = 1;
                    ok = 0;
                    break;
                }

                bytes_done += chk.raw_len;
            }

            if (ok) {
                if (crc_calc != entry.crc) {
                    os_puts(" - FAILED (CRC mismatch)\n");
                    has_errors = 1;
                } else {
                    os_puts(" (");
                    print_uint32(entry.uncomp_size);
                    os_puts(" bytes) - OK\n");
                    total_extracted++;
                }
            }
        }

        offset += entry.comp_size;
    }

    os_puts("Extracted ");
    print_uint32(total_extracted);
    os_puts(" file(s).\n");

    free_buffers();
    return has_errors ? 1 : 0;
}

int app_main(void) {
    const char *args = os_get_args();
    char cmd[4];
    char arc_name[64];
    const char *file_ptrs[32];
    int file_count = 0;
    int i = 0;
    int j;

    /* Skip leading spaces */
    while (args[i] == ' ') i++;

    if (args[i] == '\0') {
        print_usage();
        return 0;
    }

    /* Parse action switch: -c, -a, -x, -t, -l */
    if (args[i] == '-' || args[i] == '/') {
        i++;
        j = 0;
        while (args[i] && args[i] != ' ' && j < 3) {
            cmd[j++] = args[i++];
        }
        cmd[j] = '\0';
    } else {
        print_usage();
        return 1;
    }

    while (args[i] == ' ') i++;

    /* Parse archive name */
    if (args[i] == '\0') {
        print_usage();
        return 1;
    }
    j = 0;
    while (args[i] && args[i] != ' ' && j < 63) {
        arc_name[j++] = args[i++];
    }
    arc_name[j] = '\0';

    /* Parse remaining arguments */
    while (args[i] && file_count < 32) {
        while (args[i] == ' ') i++;
        if (args[i] == '\0') break;

        file_ptrs[file_count++] = &args[i];
        while (args[i] && args[i] != ' ') i++;
        if (args[i] != '\0') {
            /* Note: args buffer is in user space 0xFF00, can null-terminate in-place */
            ((char *)args)[i++] = '\0';
        }
    }

    if (cmd[0] == 'c' || cmd[0] == 'C') {
        if (file_count == 0) {
            os_puts("Error: No files specified to archive.\n");
            return 1;
        }
        return cmd_create(arc_name, file_count, file_ptrs);
    } else if (cmd[0] == 'a' || cmd[0] == 'A') {
        if (file_count == 0) {
            os_puts("Error: No files specified to append.\n");
            return 1;
        }
        return cmd_append(arc_name, file_count, file_ptrs);
    } else if (cmd[0] == 'x' || cmd[0] == 'X') {
        const char *dest = (file_count > 0) ? file_ptrs[0] : 0;
        return cmd_extract(arc_name, dest);
    } else if (cmd[0] == 't' || cmd[0] == 'T') {
        return cmd_list_or_test(arc_name, 1);
    } else if (cmd[0] == 'l' || cmd[0] == 'L') {
        return cmd_list_or_test(arc_name, 0);
    } else {
        print_usage();
        return 1;
    }
}
