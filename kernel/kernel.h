/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Internal kernel declarations for hobbyOS.
 */

#ifndef KERNEL_H
#define KERNEL_H

#include "../include/hobbyos.h"

#define KERNEL_SEG       0x9000
#define MEM_START_SEG    0x0060
#define MEM_END_SEG      0x9000
#define USER_PROC_PARAS  0x1000  /* 64 KB (4096 paragraphs) per process segment */
#define USER_INIT_SP     0xFEFE

#define COM1_BASE        0x3F8
#define EMS_IO_BASE      0x260
#define EMS_FRAME_SEG    0xE000
#define EMS_TOTAL_PAGES  128
#define EMS_NUM_SLOTS    4

#define MK_FP(seg, off)  ((void __far *)(((unsigned long)(seg) << 16) | (uint16_t)(off)))

/* Low-level assembly helpers (kernel/entry.asm) */
extern void     outb(uint16_t port, uint8_t val);
extern uint8_t  inb(uint16_t port);
extern int      fpu_8087_init(void);
extern int      bios_disk_rw(uint8_t is_write, uint16_t cyl, uint8_t head, uint8_t sec, void __far *buf);
extern void     cpu_halt(void);
extern uint16_t cpu_detect_type(void);
uint16_t        cpu_get_type(void);
const char     *cpu_get_name(void);
extern void     kbd_install_vector(void);
extern void     cga_write_cell(uint16_t cell_idx, uint16_t cell_val);
extern void     cga_sync_copy(const uint16_t *src, uint16_t word_count);
extern void     bios_set_video_mode(uint8_t mode);
extern void     cga_wait_vsync(void);
extern void     cga_blit_linear(const void __far *src);
extern void     syscall_install_vector(void);
extern uint16_t bios_get_ext_mem_kb(void);
extern int      bios_move_ext_mem(void __far *gdt_table, uint16_t word_count);
extern int      a20_test(void);
extern int      a20_enable(void);
extern void     a20_disable(void);
extern void     xms_install_vector(void);
extern int      proc_exec_far(uint16_t user_seg, uint16_t user_sp);
extern void     proc_exit_usermode(uint8_t code);
extern uint16_t get_usermode_active(void);

/* Custom IRQ 1 PC/XT Keyboard Driver & Layout Mapper (kernel/kbd.c) */
void     kbd_init(void);
void     kbd_irq_handler(uint8_t raw);
int      kbd_kbhit(void);
uint16_t kbd_getc(void);
int      kbd_set_map(const os_keymap_t __far *map);
int      kbd_get_name(char __far *out_name);
int      kbd_get_state(uint8_t scancode);

/* Console & Serial subsystem (kernel/console.c) */
void     serial_init(void);

void     con_init(void);
void     con_clear(uint8_t attr);
void     con_set_attr(uint8_t attr);
void     con_set_cursor(uint8_t row, uint8_t col);
void     con_get_cursor(uint8_t *row, uint8_t *col);
void     con_set_cursor_visible(uint8_t visible);
void     con_putc(char c);
void     con_puts(const char *s);
void     con_puts_far(const char __far *s);
void     con_put_uint(uint32_t val);
void     con_put_hex16(uint16_t val);
int      con_kbhit(void);
uint16_t con_getc(void);
uint16_t con_getline(char *buf, uint16_t max_len);

/* Conventional Memory MCB Allocator (kernel/mem.c) */
void     mem_init(void);
uint16_t mem_alloc(uint16_t paragraphs, uint16_t owner);
int      mem_free(uint16_t seg);
void     mem_free_by_owner(uint16_t owner);
uint16_t mem_avail_paragraphs(void);

/* Lo-tech 2MB EMS Manager (kernel/ems.c) */
int      ems_init(void);
int      ems_alloc_page(uint16_t owner);
int      ems_free_page(uint16_t page);
void     ems_free_by_owner(uint16_t owner);
uint16_t ems_map_page(uint8_t slot, uint16_t page);
uint16_t ems_avail_pages(void);
uint16_t ems_total_pages(void);
void     ems_save_slots(uint8_t out_slots[EMS_NUM_SLOTS]);
void     ems_restore_slots(const uint8_t in_slots[EMS_NUM_SLOTS]);

/* XMS 2.0 Manager (kernel/xms.c) */
int      xms_init(void);
int      xms_is_present(void);
uint16_t xms_total_kb(void);
uint16_t xms_avail_kb(void);
uint16_t xms_largest_free_kb(void);
uint16_t xms_alloc(uint16_t size_kb, uint16_t owner_seg);
int      xms_free(uint16_t handle);
void     xms_free_by_owner(uint16_t owner_seg);
int      xms_move(const os_xms_move_t __far *req);
void     xms_dispatch(syscall_regs_t __far *regs);

/* FAT16 filesystem driver (kernel/fat16.c) */
int      fat16_init(void);
int32_t  fat16_file_size(const char __far *filename);
int      fat16_read_file(const char __far *filename, void __far *dest, uint16_t max_bytes);
int      fat16_load_executable(const char __far *filename, uint16_t dest_seg, uint32_t *out_size);
int      fat16_write_file(const char __far *filename, const void __far *src, uint16_t size);
int      fat16_append_file(const char __far *filename, const void __far *src, uint16_t size);
int      fat16_delete_file(const char __far *filename);
int      fat16_dir_entry(const char __far *path, uint16_t index, os_dir_entry_t __far *out_info);
int      fat16_mkdir(const char __far *path);
int      fat16_rmdir(const char __far *path);
int      fat16_chdir(const char __far *path);
int      fat16_getcwd(char __far *buf, uint16_t max_len);
int      fat16_copy_file(const char __far *src_path, const char __far *dst_path, uint8_t append);
int      fat16_rename_file(const char __far *src_path, const char __far *dst_path);

/* CGA Graphics Subsystem (kernel/gfx.c) */
int      gfx_set_mode(uint8_t mode);
uint8_t  gfx_get_mode(void);
void     gfx_set_palette(uint8_t palette, uint8_t intensity, uint8_t color);
void     gfx_vsync(void);
void     gfx_clear(uint8_t color);
void     gfx_putpixel(uint16_t x, uint16_t y, uint8_t color);
void     gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
void     gfx_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color, uint8_t filled);
void     gfx_draw_text(uint16_t x, uint16_t y, const char __far *str, uint8_t color);
void     gfx_blit(const void __far *linear_buf);

/* Syscall & Executable loader (kernel/syscall.c) */
void     syscall_init(void);
void     syscall_dispatch(syscall_regs_t __far *regs);
int      exec_program(const char __far *filename, const char __far *args);
int      fpu_is_present(void);
int      path_get(char __far *buf, uint16_t max_len);
int      path_set(const char __far *new_path);

#endif /* KERNEL_H */
