/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * hobbyOS public system call interface and user-space API (INT 0x21).
 */

#ifndef HOBBYOS_H
#define HOBBYOS_H

typedef unsigned char  uint8_t;
typedef signed char    int8_t;
typedef unsigned short uint16_t;
typedef signed short   int16_t;
typedef unsigned long  uint32_t;
typedef signed long    int32_t;

/* INT 0x21 System Call Numbers (passed in AH) */
#define SYS_EXIT         0x00
#define SYS_PUTC         0x01
#define SYS_PUTS         0x02
#define SYS_GETC         0x03
#define SYS_KBHIT        0x04
#define SYS_CLS          0x05
#define SYS_SET_CURSOR   0x06
#define SYS_GET_CURSOR   0x07
#define SYS_GET_ARGS     0x08
#define SYS_FPU_PRESENT  0x09
#define SYS_CURSOR_VISIBLE 0x0A
#define SYS_CPU_TYPE     0x0B

/* CPU Types detected by hobbyOS */
#define CPU_TYPE_8088     0  /* Intel 8088 (8-bit bus, 4-byte prefetch queue) */
#define CPU_TYPE_8086     1  /* Intel 8086 (16-bit bus, 6-byte prefetch queue) */
#define CPU_TYPE_V20      2  /* NEC V20 (8-bit bus, 4-byte prefetch queue) */
#define CPU_TYPE_V30      3  /* NEC V30 (16-bit bus, 6-byte prefetch queue) */
#define CPU_TYPE_80188    4  /* Intel 80188 (8-bit bus, 4-byte prefetch queue) */
#define CPU_TYPE_80186    5  /* Intel 80186 (16-bit bus, 6-byte prefetch queue) */
#define CPU_TYPE_80286    6  /* Intel 80286 */
#define CPU_TYPE_80386    7  /* Intel 80386+ */

#define SYS_FILE_READ    0x10
#define SYS_FILE_WRITE   0x11
#define SYS_FILE_SIZE    0x12
#define SYS_FILE_DELETE  0x13
#define SYS_DIR_ENTRY    0x14
#define SYS_MKDIR        0x15
#define SYS_RMDIR        0x16
#define SYS_CHDIR        0x17
#define SYS_GETCWD       0x18
#define SYS_FILE_APPEND  0x19
#define SYS_FILE_COPY    0x1A
#define SYS_FILE_RENAME  0x1B

#define SYS_EXEC         0x20
#define SYS_GET_PATH     0x21
#define SYS_SET_PATH     0x22

#define SYS_MEM_ALLOC    0x30
#define SYS_MEM_FREE     0x31
#define SYS_MEM_AVAIL    0x32

#define SYS_EMS_ALLOC    0x40
#define SYS_EMS_FREE     0x41
#define SYS_EMS_MAP      0x42
#define SYS_EMS_AVAIL    0x43

#define SYS_XMS_ALLOC    0x44
#define SYS_XMS_FREE     0x45
#define SYS_XMS_MOVE     0x46
#define SYS_XMS_AVAIL    0x47

#define SYS_KEYB_SET_MAP 0x50
#define SYS_KEYB_GET_NAME 0x51
#define SYS_KEYB_STATE   0x52

#define SYS_GFX_SET_MODE 0x60
#define SYS_GFX_GET_MODE 0x61
#define SYS_GFX_PALETTE  0x62
#define SYS_GFX_VSYNC    0x63
#define SYS_GFX_CLEAR    0x64
#define SYS_GFX_PUTPIXEL 0x65
#define SYS_GFX_LINE     0x66
#define SYS_GFX_RECT     0x67
#define SYS_GFX_TEXT     0x68
#define SYS_GFX_BLIT     0x69

/* CGA Video Modes */
#define OS_MODE_TEXT_80x25      0x03  /* 80x25 16-color text (default) */
#define OS_MODE_CGA_320x200     0x04  /* 320x200 4-color graphics (Palettes 0 & 1) */
#define OS_MODE_CGA_320x200_ALT 0x05  /* 320x200 4-color graphics (Palette 2 Cyan/Red/White) */
#define OS_MODE_CGA_640x200     0x06  /* 640x200 2-color high-res graphics */
#define OS_CGA_LINEAR_BUF_BYTES 16000 /* 200 rows * 80 bytes/row linear backbuffer */
#define OS_CGA_LINEAR_BUF_PARAS 1000  /* 16,000 bytes = 1,000 paragraphs */

/* Standard CGA Text Attributes */
#define ATTR_DEFAULT     0x07  /* Light grey on black */
#define ATTR_BRIGHT      0x0F  /* White on black */
#define ATTR_GREEN       0x0A  /* Light green on black */
#define ATTR_CYAN        0x0B  /* Light cyan on black */
#define ATTR_YELLOW      0x0E  /* Yellow on black */
#define ATTR_RED         0x0C  /* Light red on black */
#define ATTR_BANNER      0x1F  /* Bright white on blue */

/* FAT Directory Entry Attribute Flags */
#define OS_ATTR_READ_ONLY 0x01
#define OS_ATTR_HIDDEN    0x02
#define OS_ATTR_SYSTEM    0x04
#define OS_ATTR_VOLUME_ID 0x08
#define OS_ATTR_DIRECTORY 0x10
#define OS_ATTR_ARCHIVE   0x20

/* Directory entry returned by SYS_DIR_ENTRY */
typedef struct {
    char     name[13];   /* Null-terminated 8.3 name ("FILE.EXT") */
    uint8_t  attr;       /* FAT attribute byte */
    uint32_t size;       /* File size in bytes */
} os_dir_entry_t;

/* Keyboard layout translation table (.MAP file format, 392 bytes) */
typedef struct {
    char     name[8];     /* Null-padded layout name (e.g. "US", "FR", "BE") */
    uint8_t  normal[128]; /* Unshifted ASCII table indexed by Set 1 scancode */
    uint8_t  shift[128];  /* Shifted ASCII table indexed by Set 1 scancode */
    uint8_t  altgr[128];  /* AltGr / Alt ASCII table indexed by Set 1 scancode */
} os_keymap_t;

/* Graphics line descriptor for SYS_GFX_LINE */
typedef struct {
    int16_t  x0;
    int16_t  y0;
    int16_t  x1;
    int16_t  y1;
    uint8_t  color;
} os_gfx_line_t;

/* Graphics rectangle descriptor for SYS_GFX_RECT */
typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint8_t  color;
    uint8_t  filled;
} os_gfx_rect_t;

/* Saved register frame during INT 0x21 dispatch */
typedef struct {
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;
    uint16_t si;
    uint16_t di;
    uint16_t bp;
    uint16_t ds;
    uint16_t es;
    uint16_t ip;
    uint16_t cs;
    uint16_t flags;
} syscall_regs_t;

/* User-space command line buffer offset in user segment */
#define USER_ARGS_OFFSET 0xFF00

#ifdef __WATCOMC__

/* Inline INT 0x21 syscall wrappers for Open Watcom C */

void os_exit(uint8_t code);
#pragma aux os_exit = \
    "mov ah, 0x00" \
    "int 0x21" \
    parm [al] aborts;

void os_putc(char c);
#pragma aux os_putc = \
    "mov ah, 0x01" \
    "int 0x21" \
    parm [al] modify [ax];

void os_puts(const char *s);
#pragma aux os_puts = \
    "mov ah, 0x02" \
    "int 0x21" \
    parm [bx] modify [ax];

uint16_t os_getc(void);
#pragma aux os_getc = \
    "mov ah, 0x03" \
    "int 0x21" \
    value [ax];

int os_kbhit(void);
#pragma aux os_kbhit = \
    "mov ah, 0x04" \
    "int 0x21" \
    value [ax];

void os_cls(uint8_t attr);
#pragma aux os_cls = \
    "mov ah, 0x05" \
    "int 0x21" \
    parm [al] modify [ax];

void os_set_cursor(uint8_t row, uint8_t col);
#pragma aux os_set_cursor = \
    "mov ah, 0x06" \
    "int 0x21" \
    parm [dh] [dl] modify [ax];

uint16_t os_get_cursor(void);
#pragma aux os_get_cursor = \
    "mov ah, 0x07" \
    "int 0x21" \
    value [dx] modify [ax];

const char *os_get_args(void);
#pragma aux os_get_args = \
    "mov ah, 0x08" \
    "int 0x21" \
    value [ax];

int os_fpu_present(void);
#pragma aux os_fpu_present = \
    "mov ah, 0x09" \
    "int 0x21" \
    value [ax];

void os_set_cursor_visible(uint8_t visible);
#pragma aux os_set_cursor_visible = \
    "mov ah, 0x0A" \
    "int 0x21" \
    parm [al] modify [ax];

uint16_t os_cpu_type(void);
#pragma aux os_cpu_type = \
    "mov ah, 0x0B" \
    "int 0x21" \
    value [ax];

static __inline const char *os_cpu_name(uint16_t type) {
    switch (type) {
    case CPU_TYPE_8088:  return "Intel 8088";
    case CPU_TYPE_8086:  return "Intel 8086";
    case CPU_TYPE_V20:   return "NEC V20";
    case CPU_TYPE_V30:   return "NEC V30";
    case CPU_TYPE_80188: return "Intel 80188";
    case CPU_TYPE_80186: return "Intel 80186";
    case CPU_TYPE_80286: return "Intel 80286";
    case CPU_TYPE_80386: return "Intel 80386+";
    default:             return "Unknown x86";
    }
}

static __inline void os_require_fpu(void) {
    if (!os_fpu_present()) {
        os_puts("FPU not present\n");
        os_exit(1);
    }
}

int os_file_read(const char *filename, void __far *buf, uint16_t max_len);
#pragma aux os_file_read = \
    "mov ah, 0x10" \
    "int 0x21" \
    parm [bx] [es di] [cx] value [ax];

int os_file_write(const char *filename, const void __far *buf, uint16_t len);
#pragma aux os_file_write = \
    "mov ah, 0x11" \
    "int 0x21" \
    parm [bx] [es di] [cx] value [ax];

int os_file_append(const char *filename, const void __far *buf, uint16_t len);
#pragma aux os_file_append = \
    "mov ah, 0x19" \
    "int 0x21" \
    parm [bx] [es di] [cx] value [ax];

long os_file_size(const char *filename);
#pragma aux os_file_size = \
    "mov ah, 0x12" \
    "int 0x21" \
    parm [bx] value [dx ax];

int os_file_delete(const char *filename);
#pragma aux os_file_delete = \
    "mov ah, 0x13" \
    "int 0x21" \
    parm [bx] value [ax];

int os_dir_entry(uint16_t index, os_dir_entry_t __far *entry);
#pragma aux os_dir_entry = \
    "xor bx, bx" \
    "mov ah, 0x14" \
    "int 0x21" \
    parm [cx] [es di] value [ax] modify [bx];

int os_dir_entry_at(const char *path, uint16_t index, os_dir_entry_t __far *entry);
#pragma aux os_dir_entry_at = \
    "mov ah, 0x14" \
    "int 0x21" \
    parm [bx] [cx] [es di] value [ax];

int os_mkdir(const char *path);
#pragma aux os_mkdir = \
    "mov ah, 0x15" \
    "int 0x21" \
    parm [bx] value [ax];

int os_rmdir(const char *path);
#pragma aux os_rmdir = \
    "mov ah, 0x16" \
    "int 0x21" \
    parm [bx] value [ax];

int os_chdir(const char *path);
#pragma aux os_chdir = \
    "mov ah, 0x17" \
    "int 0x21" \
    parm [bx] value [ax];

int os_getcwd(char __far *buf, uint16_t max_len);
#pragma aux os_getcwd = \
    "mov ah, 0x18" \
    "int 0x21" \
    parm [es di] [cx] value [ax];

int os_file_copy(const char *src_path, const char __far *dst_path, uint8_t append);
#pragma aux os_file_copy = \
    "mov ah, 0x1A" \
    "int 0x21" \
    parm [bx] [es di] [al] value [ax];

int os_file_rename(const char *src_path, const char __far *dst_path);
#pragma aux os_file_rename = \
    "mov ah, 0x1B" \
    "int 0x21" \
    parm [bx] [es di] value [ax];

int os_exec(const char *filename, const char *args);
#pragma aux os_exec = \
    "push ds" \
    "pop es" \
    "mov ah, 0x20" \
    "int 0x21" \
    parm [bx] [di] value [ax] modify [es];

int os_get_path(char __far *buf, uint16_t max_len);
#pragma aux os_get_path = \
    "mov ah, 0x21" \
    "int 0x21" \
    parm [es di] [cx] value [ax];

int os_set_path(const char __far *path);
#pragma aux os_set_path = \
    "mov ah, 0x22" \
    "int 0x21" \
    parm [es di] value [ax];

uint16_t os_mem_alloc(uint16_t paragraphs);
#pragma aux os_mem_alloc = \
    "mov ah, 0x30" \
    "int 0x21" \
    parm [bx] value [ax];

int os_mem_free(uint16_t seg);
#pragma aux os_mem_free = \
    "mov ah, 0x31" \
    "int 0x21" \
    parm [bx] value [ax];

uint16_t os_mem_avail(void);
#pragma aux os_mem_avail = \
    "mov ah, 0x32" \
    "int 0x21" \
    value [ax];

int os_ems_alloc(void);
#pragma aux os_ems_alloc = \
    "mov ah, 0x40" \
    "int 0x21" \
    value [ax];

int os_ems_free(uint16_t page);
#pragma aux os_ems_free = \
    "mov ah, 0x41" \
    "int 0x21" \
    parm [bx] value [ax];

uint16_t os_ems_map(uint8_t slot, uint16_t page);
#pragma aux os_ems_map = \
    "mov ah, 0x42" \
    "int 0x21" \
    parm [al] [bx] value [ax];

uint16_t os_ems_avail(void);
#pragma aux os_ems_avail = \
    "mov ah, 0x43" \
    "int 0x21" \
    value [ax] modify [dx];

uint16_t os_ems_total(void);
#pragma aux os_ems_total = \
    "mov ah, 0x43" \
    "int 0x21" \
    value [dx] modify [ax];

#pragma pack(push, 1)
typedef struct {
    uint32_t length;       /* Number of bytes to copy (even) */
    uint16_t src_handle;   /* 0 = conventional memory */
    uint32_t src_offset;   /* Far ptr seg:off if handle==0; offset if handle!=0 */
    uint16_t dest_handle;  /* 0 = conventional memory */
    uint32_t dest_offset;  /* Far ptr seg:off if handle==0; offset if handle!=0 */
} os_xms_move_t;
#pragma pack(pop)

uint16_t os_xms_alloc(uint16_t size_kb);
#pragma aux os_xms_alloc = \
    "mov ah, 0x44" \
    "int 0x21" \
    parm [bx] value [ax];

int os_xms_free(uint16_t handle);
#pragma aux os_xms_free = \
    "mov ah, 0x45" \
    "int 0x21" \
    parm [bx] value [ax];

int os_xms_move(const os_xms_move_t __far *move_req);
#pragma aux os_xms_move = \
    "mov ah, 0x46" \
    "int 0x21" \
    parm [es di] value [ax];

uint16_t os_xms_avail(void);
#pragma aux os_xms_avail = \
    "mov ah, 0x47" \
    "int 0x21" \
    value [ax] modify [dx];

uint16_t os_xms_total(void);
#pragma aux os_xms_total = \
    "mov ah, 0x47" \
    "int 0x21" \
    value [dx] modify [ax];

int os_keyb_set_map(const os_keymap_t __far *map);
#pragma aux os_keyb_set_map = \
    "mov ah, 0x50" \
    "int 0x21" \
    parm [es di] value [ax];

int os_keyb_get_name(char __far *name_buf);
#pragma aux os_keyb_get_name = \
    "mov ah, 0x51" \
    "int 0x21" \
    parm [es di] value [ax];

int os_keyb_state(uint8_t scancode);
#pragma aux os_keyb_state = \
    "mov ah, 0x52" \
    "int 0x21" \
    parm [bl] value [ax];

int os_gfx_set_mode(uint8_t mode);
#pragma aux os_gfx_set_mode = \
    "mov ah, 0x60" \
    "int 0x21" \
    parm [al] value [ax];

uint8_t os_gfx_get_mode(void);
#pragma aux os_gfx_get_mode = \
    "mov ah, 0x61" \
    "int 0x21" \
    value [al] modify [ax];

void os_gfx_set_palette(uint8_t palette, uint8_t intensity, uint8_t color);
#pragma aux os_gfx_set_palette = \
    "mov ah, 0x62" \
    "int 0x21" \
    parm [bl] [bh] [cl] modify [ax];

void os_gfx_vsync(void);
#pragma aux os_gfx_vsync = \
    "mov ah, 0x63" \
    "int 0x21" \
    modify [ax];

void os_gfx_clear(uint8_t color);
#pragma aux os_gfx_clear = \
    "mov ah, 0x64" \
    "int 0x21" \
    parm [al] modify [ax];

void os_gfx_putpixel(uint16_t x, uint16_t y, uint8_t color);
#pragma aux os_gfx_putpixel = \
    "mov ah, 0x65" \
    "int 0x21" \
    parm [cx] [dx] [al] modify [ax];

void os_gfx_line_raw(const os_gfx_line_t __far *line);
#pragma aux os_gfx_line_raw = \
    "mov ah, 0x66" \
    "int 0x21" \
    parm [es di] modify [ax];

void os_gfx_rect_raw(const os_gfx_rect_t __far *rect);
#pragma aux os_gfx_rect_raw = \
    "mov ah, 0x67" \
    "int 0x21" \
    parm [es di] modify [ax];

void os_gfx_text(uint16_t x, uint16_t y, const char __far *str, uint8_t color);
#pragma aux os_gfx_text = \
    "mov ah, 0x68" \
    "int 0x21" \
    parm [cx] [dx] [es di] [al] modify [ax];

void os_gfx_blit(const void __far *linear_buf);
#pragma aux os_gfx_blit = \
    "mov ah, 0x69" \
    "int 0x21" \
    parm [es di] modify [ax];

/* Fast inline pixel plotting into a 16,000-byte linear backbuffer (80 bytes/row) */
static __inline void os_buf_putpixel_320(uint8_t __far *buf, uint16_t x, uint16_t y, uint8_t color) {
    if (x < 320 && y < 200) {
        uint16_t off   = (y << 6) + (y << 4) + (x >> 2); /* y * 80 + (x / 4) */
        uint8_t  shift = (uint8_t)((3 - (x & 3)) << 1);
        buf[off] = (uint8_t)((buf[off] & ~(0x03 << shift)) | ((color & 0x03) << shift));
    }
}

static __inline void os_buf_putpixel_640(uint8_t __far *buf, uint16_t x, uint16_t y, uint8_t color) {
    if (x < 640 && y < 200) {
        uint16_t off  = (y << 6) + (y << 4) + (x >> 3); /* y * 80 + (x / 8) */
        uint8_t  mask = (uint8_t)(0x80 >> (x & 7));
        if (color) {
            buf[off] |= mask;
        } else {
            buf[off] &= (uint8_t)~mask;
        }
    }
}

#endif /* __WATCOMC__ */

#endif /* HOBBYOS_H */
