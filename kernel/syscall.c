/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * INT 0x21 system call dispatcher and dynamic MCB-backed executable loader.
 */

#include "kernel.h"

void syscall_init(void) {
    syscall_install_vector();
}

#define KERNEL_PATH_MAX 128
static char kernel_path[KERNEL_PATH_MAX] = ".;/BIN";
static void redir_stdout_flush(void);

int path_get(char __far *buf, uint16_t max_len) {
    uint16_t i = 0;
    if (!buf || max_len == 0) return -1;
    while (kernel_path[i] != '\0' && i + 1 < max_len) {
        buf[i] = kernel_path[i];
        i++;
    }
    buf[i] = '\0';
    return 0;
}

int path_set(const char __far *new_path) {
    uint16_t i = 0;
    if (!new_path) return -1;
    while (new_path[i] != '\0' && i < KERNEL_PATH_MAX - 1) {
        kernel_path[i] = new_path[i];
        i++;
    }
    kernel_path[i] = '\0';
    return 0;
}

typedef struct {
    uint16_t e_magic;         /* 0x00: 0x5A4D ("MZ") or 0x4D5A ("ZM") */
    uint16_t e_cblp;          /* 0x02: Bytes on last page of file */
    uint16_t e_cp;            /* 0x04: Pages in file (512-byte blocks) */
    uint16_t e_crlc;          /* 0x06: Relocations count */
    uint16_t e_cparhdr;       /* 0x08: Header size in paragraphs */
    uint16_t e_minalloc;      /* 0x0A: Minimum extra paragraphs */
    uint16_t e_maxalloc;      /* 0x0C: Maximum extra paragraphs */
    uint16_t e_ss;            /* 0x0E: Initial SS relative to load segment */
    uint16_t e_sp;            /* 0x10: Initial SP */
    uint16_t e_csum;          /* 0x12: Checksum */
    uint16_t e_ip;            /* 0x14: Initial IP */
    uint16_t e_cs;            /* 0x16: Initial CS relative to load segment */
    uint16_t e_lfarlc;        /* 0x18: Offset of relocation table */
    uint16_t e_ovno;          /* 0x1A: Overlay number */
} mz_header_t;

typedef struct {
    uint16_t offset;
    uint16_t segment;
} mz_reloc_t;

static int try_path_candidate(const char *dir, uint16_t dir_len,
                              const char __far *cmd, uint16_t cmd_len,
                              const char *ext,
                              char out_buf[48], const char __far **out_target) {
    uint16_t pos = 0;
    uint16_t i;

    /* If dir is "." or empty, don't prepend dir/ */
    if (dir_len > 0 && !(dir_len == 1 && dir[0] == '.')) {
        for (i = 0; i < dir_len && pos < 38; i++) {
            out_buf[pos++] = dir[i];
        }
        if (pos > 0 && out_buf[pos - 1] != '/' && out_buf[pos - 1] != '\\') {
            out_buf[pos++] = '/';
        }
    }

    for (i = 0; i < cmd_len && pos < 40; i++) {
        out_buf[pos++] = cmd[i];
    }

    if (ext) {
        while (*ext != '\0' && pos < 47) {
            out_buf[pos++] = *ext++;
        }
    }
    out_buf[pos] = '\0';

    *out_target = (const char __far *)MK_FP(KERNEL_SEG, out_buf);
    return (fat16_file_size(*out_target) > 0) ? 0 : -1;
}

static int resolve_exec_target(const char __far *input, char out_buf[48], const char __far **out_target) {
    int has_slash = 0;
    int has_dot   = 0;
    uint16_t len  = 0;
    uint16_t i;

    if (!input || input[0] == '\0') {
        return -1;
    }

    /* 1. Try exact path as given */
    if (fat16_file_size(input) > 0) {
        *out_target = input;
        return 0;
    }

    while (input[len] != '\0' && len < 24) {
        if (input[len] == '/' || input[len] == '\\' || input[len] == ':') {
            has_slash = 1;
            has_dot   = 0;
        } else if (input[len] == '.') {
            has_dot = 1;
        }
        len++;
    }
    if (input[len] != '\0') {
        return -1;
    }

    /* 2. If path has a directory component and no extension, try appending .BIN then .EXE */
    if (has_slash && !has_dot && len <= 24) {
        for (i = 0; i < len; i++) {
            out_buf[i] = input[i];
        }
        out_buf[len + 0] = '.';
        out_buf[len + 1] = 'B';
        out_buf[len + 2] = 'I';
        out_buf[len + 3] = 'N';
        out_buf[len + 4] = '\0';
        *out_target = (const char __far *)MK_FP(KERNEL_SEG, out_buf);
        if (fat16_file_size(*out_target) > 0) {
            return 0;
        }
        out_buf[len + 1] = 'E';
        out_buf[len + 2] = 'X';
        out_buf[len + 3] = 'E';
        if (fat16_file_size(*out_target) > 0) {
            return 0;
        }
        return -1;
    }

    /* 3. If bare command name (no slash), search along kernel_path */
    if (!has_slash && len <= 12) {
        uint16_t p = 0;
        while (kernel_path[p] != '\0') {
            uint16_t d_start = p;
            uint16_t d_len;
            while (kernel_path[p] != '\0' && kernel_path[p] != ';') {
                p++;
            }
            d_len = p - d_start;
            if (kernel_path[p] == ';') {
                p++;
            }

            /* Try <dir>/<cmd> */
            if (try_path_candidate(kernel_path + d_start, d_len, input, len, 0, out_buf, out_target) == 0) {
                return 0;
            }
            /* Try <dir>/<cmd>.BIN and <dir>/<cmd>.EXE if no extension */
            if (!has_dot) {
                if (try_path_candidate(kernel_path + d_start, d_len, input, len, ".BIN", out_buf, out_target) == 0) {
                    return 0;
                }
                if (try_path_candidate(kernel_path + d_start, d_len, input, len, ".EXE", out_buf, out_target) == 0) {
                    return 0;
                }
            }
        }
    }

    return -1;
}

static uint16_t s_current_proc_owner = 0;

int exec_program(const char __far *filename, const char __far *args) {
    char resolved_buf[48];
    const char __far *target_name = 0;
    uint32_t bin_size = 0;
    uint16_t user_seg;
    char __far *arg_dst;
    uint16_t i = 0;
    int rc;
    mz_header_t mz_hdr;
    int is_mz = 0;
    int32_t hdr_read;

    if (resolve_exec_target(filename, resolved_buf, &target_name) != 0) {
        return -1;
    }

    /* Probe for MS-DOS MZ Executable header */
    hdr_read = fat16_read_file_offset(target_name, MK_FP(KERNEL_SEG, &mz_hdr), sizeof(mz_hdr), 0);
    if (hdr_read == (int32_t)sizeof(mz_hdr)) {
        if (mz_hdr.e_magic == 0x5A4D || mz_hdr.e_magic == 0x4D5A) {
            is_mz = 1;
        }
    }

    if (is_mz) {
        uint32_t total_file_bytes;
        uint32_t header_bytes = (uint32_t)mz_hdr.e_cparhdr * 16L;
        uint32_t image_bytes;
        uint16_t image_paras;
        uint16_t alloc_paras;
        uint16_t base_seg;
        uint16_t psp_seg;
        uint16_t load_seg;
        uint16_t dgroup_seg;

        if (mz_hdr.e_cblp == 0) {
            total_file_bytes = (uint32_t)mz_hdr.e_cp * 512L;
        } else {
            total_file_bytes = (uint32_t)(mz_hdr.e_cp - 1) * 512L + (uint32_t)mz_hdr.e_cblp;
        }
        if (total_file_bytes <= header_bytes) {
            return -1;
        }
        image_bytes = total_file_bytes - header_bytes;
        image_paras = (uint16_t)((image_bytes + 15L) >> 4);

        /* 16 paragraphs for PSP (256 bytes) + image paragraphs + full 64 KB (4096 paras) for DGROUP */
        {
            uint16_t dgroup_para = mz_hdr.e_ss;
            uint16_t code_paras = (dgroup_para > image_paras) ? dgroup_para : image_paras;
            alloc_paras = 16 + code_paras + 4096;
        }

        base_seg = mem_alloc(alloc_paras, 0);
        if (base_seg == 0) {
            con_puts("Out of memory for executable\n");
            return -1;
        }

        psp_seg = base_seg;
        load_seg = base_seg + 16;
        dgroup_seg = load_seg + mz_hdr.e_ss;

        /* Setup PSP at psp_seg:0x0000 */
        {
            uint8_t __far *psp = (uint8_t __far *)MK_FP(psp_seg, 0);
            uint16_t k;
            for (k = 0; k < 256; k++) psp[k] = 0;

            /* CD 20 (int 0x20 exit) */
            psp[0] = 0xCD;
            psp[1] = 0x20;

            /* Top of memory segment at offset 0x0002 */
            psp[2] = (uint8_t)((base_seg + alloc_paras) & 0xFF);
            psp[3] = (uint8_t)((base_seg + alloc_paras) >> 8);

            /* Command-line args at psp_seg:0x0080 */
            if (args) {
                uint8_t alen = 0;
                while (args[alen] != '\0' && alen < 126) {
                    psp[0x81 + alen] = (uint8_t)args[alen];
                    alen++;
                }
                psp[0x80] = alen;
                psp[0x81 + alen] = 0x0D;
                psp[0x82 + alen] = 0x00;
            } else {
                psp[0x80] = 0;
                psp[0x81] = 0x0D;
            }
        }

        /* Also copy args to dgroup_seg:USER_ARGS_OFFSET for os_get_args() */
        {
            char __far *dgroup_args = (char __far *)MK_FP(dgroup_seg, USER_ARGS_OFFSET);
            uint16_t ai = 0;
            if (args) {
                while (args[ai] != '\0' && ai < 254) {
                    dgroup_args[ai] = args[ai];
                    ai++;
                }
            }
            dgroup_args[ai] = '\0';
        }

        /* Read image_bytes from file offset header_bytes into load_seg:0x0000 */
        {
            uint32_t file_pos = header_bytes;
            uint16_t cur_seg = load_seg;
            uint16_t cur_off = 0;
            uint32_t remaining = image_bytes;

            while (remaining > 0) {
                uint16_t chunk = (remaining > 32768L) ? 32768 : (uint16_t)remaining;
                int32_t rd = fat16_read_file_offset(target_name, MK_FP(cur_seg, cur_off), chunk, file_pos);
                if (rd <= 0) {
                    mem_free_by_owner(base_seg);
                    return -1;
                }
                file_pos += (uint32_t)rd;
                remaining -= (uint32_t)rd;
                cur_off += (uint16_t)rd;
                if (cur_off >= 0x8000) {
                    cur_seg += (cur_off >> 4);
                    cur_off &= 0x000F;
                }
            }
        }

        /* Relocation Table Fixups */
        if (mz_hdr.e_crlc > 0) {
            uint16_t rel_left = mz_hdr.e_crlc;
            uint32_t rel_file_off = (uint32_t)mz_hdr.e_lfarlc;

            while (rel_left > 0) {
                mz_reloc_t r_buf[32];
                uint16_t count = (rel_left > 32) ? 32 : rel_left;
                uint16_t r_bytes = count * sizeof(mz_reloc_t);
                int32_t rd = fat16_read_file_offset(target_name, MK_FP(KERNEL_SEG, r_buf), r_bytes, rel_file_off);
                uint16_t r;
                if (rd != (int32_t)r_bytes) {
                    mem_free_by_owner(base_seg);
                    return -1;
                }
                for (r = 0; r < count; r++) {
                    uint16_t patch_seg = load_seg + r_buf[r].segment;
                    uint16_t patch_off = r_buf[r].offset;
                    uint16_t __far *patch = (uint16_t __far *)MK_FP(patch_seg, patch_off);
                    *patch += load_seg;
                }
                rel_file_off += (uint32_t)r_bytes;
                rel_left -= count;
            }
        }

        /* Transfer control to MZ executable */
        {
            uint8_t saved_ems_slots[EMS_NUM_SLOTS];
            uint8_t saved_break_mode = kbd_get_break_mode();
            uint16_t prev_owner = s_current_proc_owner;
            uint16_t target_cs = load_seg + mz_hdr.e_cs;
            uint16_t target_ip = mz_hdr.e_ip;
            uint16_t target_ss = load_seg + mz_hdr.e_ss;
            uint16_t target_sp = mz_hdr.e_sp;
            uint16_t target_ds_es = psp_seg;

            s_current_proc_owner = base_seg;
            kbd_set_break_mode(BREAK_MODE_NORMAL);
            ctrl_c_abort_pending = 0;
            ems_save_slots(saved_ems_slots);

            rc = proc_exec_mz(target_cs, target_ip, target_ss, target_sp, target_ds_es);

            kbd_set_break_mode(saved_break_mode);
            ctrl_c_abort_pending = 0;
            kbd_set_break_mode(BREAK_MODE_NORMAL);

            if (gfx_get_mode() != OS_MODE_TEXT_80x25 && gfx_get_mode() != 0x07) {
                gfx_set_mode(OS_MODE_TEXT_80x25);
            } else {
                con_set_cursor_visible(1);
            }

            redir_stdout_flush();
            sound_stop();

            ems_free_by_owner(base_seg);
            if (target_cs != base_seg) ems_free_by_owner(target_cs);
            ems_restore_slots(saved_ems_slots);
            xms_free_by_owner(base_seg);
            if (target_cs != base_seg) xms_free_by_owner(target_cs);
            mem_free_by_owner(base_seg);
            if (target_cs != base_seg) mem_free_by_owner(target_cs);
            s_current_proc_owner = prev_owner;
        }

        return rc;
    }

    /* Allocate a 64 KB segment (4096 paragraphs) from the MCB allocator */
    user_seg = mem_alloc(USER_PROC_PARAS, 0);
    if (user_seg == 0) {
        return -1;
    }

    if (fat16_load_executable(target_name, user_seg, &bin_size) != 0) {
        mem_free(user_seg);
        return -1;
    }

    /* Safety guard: prevent 8080 CP/M code from executing as native 8086 code */
    if (bin_size >= 3) {
        uint8_t __far *code = (uint8_t __far *)MK_FP(user_seg, 0);
        uint16_t tlen = 0;
        while (target_name[tlen] != '\0') tlen++;
        if (tlen >= 4 && target_name[tlen - 4] == '.' &&
            (target_name[tlen - 3] == 'c' || target_name[tlen - 3] == 'C') &&
            (target_name[tlen - 2] == 'o' || target_name[tlen - 2] == 'O') &&
            (target_name[tlen - 1] == 'm' || target_name[tlen - 1] == 'M')) {
            if (code[0] == 0xC3 || code[0] == 0x31 || code[0] == 0x21 ||
                code[0] == 0x01 || code[0] == 0x11 || code[0] == 0x2A) {
                con_puts("Cannot execute CP/M-80 binary directly (use 'cpm <file.com>'): ");
                con_puts(resolved_buf);
                con_putc('\n');
                mem_free(user_seg);
                return -1;
            }
        }
    }

    /* Copy optional command-line argument string into user_seg:0xFF00 */
    arg_dst = (char __far *)MK_FP(user_seg, USER_ARGS_OFFSET);
    if (args) {
        while (args[i] != '\0' && i < 254) {
            arg_dst[i] = args[i];
            i++;
        }
    }
    arg_dst[i] = '\0';

    /* Save parent process EMS page frame slot mappings & break mode */
    {
        uint8_t saved_ems_slots[EMS_NUM_SLOTS];
        uint8_t saved_break_mode = kbd_get_break_mode();
        uint16_t prev_owner = s_current_proc_owner;
        s_current_proc_owner = user_seg;
        kbd_set_break_mode(BREAK_MODE_NORMAL);
        ctrl_c_abort_pending = 0;
        ems_save_slots(saved_ems_slots);

        /* Transfer control to user executable at user_seg:0000 */
        rc = proc_exec_far(user_seg, USER_INIT_SP);

        kbd_set_break_mode(saved_break_mode);
        ctrl_c_abort_pending = 0;

        /* Restore break mode to normal in case child process altered it */
        kbd_set_break_mode(BREAK_MODE_NORMAL);

        /* Restore 80x25 text console if the process exited while in CGA graphics mode */
        if (gfx_get_mode() != OS_MODE_TEXT_80x25 && gfx_get_mode() != 0x07) {
            gfx_set_mode(OS_MODE_TEXT_80x25);
        } else {
            con_set_cursor_visible(1);
        }

        /* Flush any redirected stdout data written by child process */
        redir_stdout_flush();

        /* Mute any active sound output from child process */
        sound_stop();

        /* Reclaim all conventional MCB blocks, EMS pages, and XMS blocks owned by the process */
        ems_free_by_owner(user_seg);
        ems_restore_slots(saved_ems_slots);
        xms_free_by_owner(user_seg);
        mem_free_by_owner(user_seg);
        s_current_proc_owner = prev_owner;
    }

    return rc;
}

/* Standard I/O Redirection Subsystem */
#define REDIR_BUF_SIZE 512

static uint8_t  s_stdout_redir_active = 0;
static char     s_stdout_path[64];
static uint8_t  s_stdout_append       = 0;
static uint8_t  s_stdout_buf[REDIR_BUF_SIZE];
static uint16_t s_stdout_buf_pos      = 0;

static uint8_t  s_stdin_redir_active  = 0;
static char     s_stdin_path[64];
static uint32_t s_stdin_file_offset   = 0;
static uint8_t  s_stdin_buf[REDIR_BUF_SIZE];
static uint16_t s_stdin_buf_pos       = 0;
static uint16_t s_stdin_buf_len       = 0;
static uint8_t  s_stdin_eof           = 0;

static void redir_stdout_flush(void) {
    if (!s_stdout_redir_active) return;
    if (s_stdout_buf_pos > 0) {
        fat16_append_file(s_stdout_path, MK_FP(KERNEL_SEG, s_stdout_buf), s_stdout_buf_pos);
        s_stdout_buf_pos = 0;
    }
}

static void redir_stdout_putc(char c) {
    s_stdout_buf[s_stdout_buf_pos++] = (uint8_t)c;
    if (s_stdout_buf_pos >= REDIR_BUF_SIZE) {
        redir_stdout_flush();
    }
}

static int redir_set_stdout(const char __far *path, uint8_t append) {
    uint16_t i = 0;
    if (!path) return -1;
    redir_stdout_flush();
    while (path[i] != '\0' && i < sizeof(s_stdout_path) - 1) {
        s_stdout_path[i] = path[i];
        i++;
    }
    s_stdout_path[i] = '\0';
    s_stdout_append = append;
    s_stdout_buf_pos = 0;
    s_stdout_redir_active = 1;

    /* If truncate/overwrite mode (append == 0), clear file to 0 bytes */
    if (!append) {
        fat16_write_file(s_stdout_path, MK_FP(KERNEL_SEG, s_stdout_buf), 0);
    }
    return 0;
}

static int redir_set_stdin(const char __far *path) {
    uint16_t i = 0;
    if (!path) return -1;
    while (path[i] != '\0' && i < sizeof(s_stdin_path) - 1) {
        s_stdin_path[i] = path[i];
        i++;
    }
    s_stdin_path[i] = '\0';
    s_stdin_file_offset = 0;
    s_stdin_buf_pos = 0;
    s_stdin_buf_len = 0;
    s_stdin_eof = 0;
    s_stdin_redir_active = 1;
    return 0;
}

static void redir_reset_all(void) {
    if (s_stdout_redir_active) {
        redir_stdout_flush();
        s_stdout_redir_active = 0;
    }
    s_stdin_redir_active = 0;
}

static uint16_t redir_stdin_getc(void) {
    if (!s_stdin_redir_active) return con_getc();
    if (s_stdin_buf_pos >= s_stdin_buf_len) {
        int32_t rd;
        if (s_stdin_eof) return 0xFFFF;
        rd = fat16_read_file_offset(s_stdin_path, MK_FP(KERNEL_SEG, s_stdin_buf), REDIR_BUF_SIZE, s_stdin_file_offset);
        if (rd <= 0) {
            s_stdin_eof = 1;
            return 0xFFFF;
        }
        s_stdin_file_offset += (uint16_t)rd;
        s_stdin_buf_len = (uint16_t)rd;
        s_stdin_buf_pos = 0;
    }
    return (uint16_t)s_stdin_buf[s_stdin_buf_pos++];
}

static int redir_stdin_kbhit(void) {
    if (!s_stdin_redir_active) return con_kbhit();
    if (s_stdin_buf_pos < s_stdin_buf_len) return 1;
    if (s_stdin_eof) return 0;
    {
        int32_t rd = fat16_read_file_offset(s_stdin_path, MK_FP(KERNEL_SEG, s_stdin_buf), REDIR_BUF_SIZE, s_stdin_file_offset);
        if (rd <= 0) {
            s_stdin_eof = 1;
            return 0;
        }
        s_stdin_file_offset += (uint16_t)rd;
        s_stdin_buf_len = (uint16_t)rd;
        s_stdin_buf_pos = 0;
        return 1;
    }
}

void syscall_dispatch(syscall_regs_t __far *regs) {
    uint8_t func = (uint8_t)(regs->ax >> 8);
    uint8_t al   = (uint8_t)(regs->ax & 0xFF);

    switch (func) {
    case SYS_EXIT:
        redir_stdout_flush();
        con_set_scroll_rows(25);
        if (get_usermode_active()) {
            proc_exit_usermode(al);
        }
        break;

    case SYS_PUTC:
        if (s_stdout_redir_active) {
            redir_stdout_putc((char)al);
        } else {
            con_putc((char)al);
        }
        break;

    case SYS_PUTS: {
        const char __far *str = (const char __far *)MK_FP(regs->ds, regs->bx);
        if (s_stdout_redir_active) {
            while (*str) {
                redir_stdout_putc(*str++);
            }
        } else {
            con_puts_far(str);
        }
        break;
    }

    case SYS_GETC:
        if (s_stdin_redir_active) {
            regs->ax = redir_stdin_getc();
        } else {
            regs->ax = con_getc();
        }
        break;

    case SYS_KBHIT:
        if (s_stdin_redir_active) {
            regs->ax = (uint16_t)redir_stdin_kbhit();
        } else {
            regs->ax = (uint16_t)con_kbhit();
        }
        break;

    case SYS_CLS:
        con_clear(al ? al : ATTR_DEFAULT);
        break;

    case SYS_SET_CURSOR:
        con_set_cursor((uint8_t)(regs->dx >> 8), (uint8_t)(regs->dx & 0xFF));
        break;

    case SYS_GET_CURSOR: {
        uint8_t r = 0, c = 0;
        con_get_cursor(&r, &c);
        regs->dx = ((uint16_t)r << 8) | c;
        break;
    }

    case SYS_GET_ARGS:
        regs->ax = USER_ARGS_OFFSET;
        break;

    case SYS_FPU_PRESENT:
        regs->ax = (uint16_t)fpu_is_present();
        break;

    case SYS_CURSOR_VISIBLE:
        con_set_cursor_visible(al);
        break;

    case SYS_CPU_TYPE:
        regs->ax = cpu_get_type();
        break;

    case SYS_VIDEO_TYPE:
        regs->ax = video_get_type();
        break;

    case SYS_BREAK_MODE:
        kbd_set_break_mode(al);
        regs->ax = 0;
        break;

    case SYS_REDIR:
        switch (al) {
        case REDIR_STDOUT: {
            const char __far *path = (const char __far *)MK_FP(regs->ds, regs->dx);
            regs->ax = (redir_set_stdout(path, (uint8_t)(regs->cx & 0xFF)) == 0) ? 0 : 0xFFFF;
            break;
        }
        case REDIR_STDIN: {
            const char __far *path = (const char __far *)MK_FP(regs->ds, regs->dx);
            regs->ax = (redir_set_stdin(path) == 0) ? 0 : 0xFFFF;
            break;
        }
        case REDIR_RESET:
            redir_reset_all();
            regs->ax = 0;
            break;
        case REDIR_KEYB_GETC:
            regs->ax = con_getc();
            break;
        case REDIR_KEYB_KBHIT:
            regs->ax = (uint16_t)con_kbhit();
            break;
        case REDIR_FLUSH:
            redir_stdout_flush();
            regs->ax = 0;
            break;
        default:
            regs->ax = 0xFFFF;
            break;
        }
        break;

    case SYS_CONSOLE_SHADOW:
        switch (al) {
        case CONSOLE_SHADOW_GET: {
            uint16_t __far *dst = (uint16_t __far *)MK_FP(regs->es, regs->di);
            con_get_shadow(dst);
            regs->ax = 0;
            break;
        }
        case CONSOLE_SHADOW_SET: {
            const uint16_t __far *src = (const uint16_t __far *)MK_FP(regs->es, regs->di);
            con_sync_shadow(src, (uint8_t)(regs->dx >> 8), (uint8_t)(regs->dx & 0xFF));
            regs->ax = 0;
            break;
        }
        case CONSOLE_SHADOW_REDRAW:
            con_redraw();
            regs->ax = 0;
            break;
        case CONSOLE_SET_SCROLL_ROWS:
            con_set_scroll_rows((uint8_t)(regs->dx & 0xFF));
            regs->ax = 0;
            break;
        case CONSOLE_WRITE_CELL:
            con_write_cell((uint8_t)(regs->dx >> 8), (uint8_t)(regs->dx & 0xFF), regs->bx);
            regs->ax = 0;
            break;
        default:
            regs->ax = 0xFFFF;
            break;
        }
        break;

    case SYS_FILE_READ: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        void __far *dest        = MK_FP(regs->es, regs->di);
        int32_t res = fat16_read_file(fname, dest, regs->cx);
        regs->ax = (uint16_t)res;
        break;
    }

    case SYS_FILE_WRITE: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        const void __far *src   = MK_FP(regs->es, regs->di);
        int32_t res = fat16_write_file(fname, src, regs->cx);
        regs->ax = (uint16_t)res;
        break;
    }

    case SYS_FILE_APPEND: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        const void __far *src   = MK_FP(regs->es, regs->di);
        int32_t res = fat16_append_file(fname, src, regs->cx);
        regs->ax = (uint16_t)res;
        break;
    }

    case SYS_FILE_SIZE: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        int32_t sz = fat16_file_size(fname);
        if (sz < 0) {
            regs->ax = 0xFFFF;
            regs->dx = 0xFFFF;
        } else {
            regs->ax = (uint16_t)(sz & 0xFFFF);
            regs->dx = (uint16_t)((uint32_t)sz >> 16);
        }
        break;
    }

    case SYS_FILE_DELETE: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        int res = fat16_delete_file(fname);
        regs->ax = (res < 0) ? 0xFFFF : 0;
        break;
    }

    case SYS_DIR_ENTRY: {
        const char __far *path    = regs->bx ? (const char __far *)MK_FP(regs->ds, regs->bx) : 0;
        os_dir_entry_t __far *ent = (os_dir_entry_t __far *)MK_FP(regs->es, regs->di);
        int res = fat16_dir_entry(path, regs->cx, ent);
        regs->ax = (res < 0) ? (uint16_t)res : 0;
        break;
    }

    case SYS_MKDIR: {
        const char __far *path = (const char __far *)MK_FP(regs->ds, regs->bx);
        regs->ax = (fat16_mkdir(path) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_RMDIR: {
        const char __far *path = (const char __far *)MK_FP(regs->ds, regs->bx);
        regs->ax = (fat16_rmdir(path) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_CHDIR: {
        const char __far *path = (const char __far *)MK_FP(regs->ds, regs->bx);
        int res = fat16_chdir(path);
        regs->ax = (res < 0) ? (uint16_t)res : 0;
        break;
    }

    case SYS_GETCWD: {
        char __far *buf = (char __far *)MK_FP(regs->es, regs->di);
        regs->ax = (fat16_getcwd(buf, regs->cx) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_FILE_COPY: {
        const char __far *src_path = (const char __far *)MK_FP(regs->ds, regs->bx);
        const char __far *dst_path = (const char __far *)MK_FP(regs->es, regs->di);
        regs->ax = (fat16_copy_file(src_path, dst_path, al) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_FILE_RENAME: {
        const char __far *src_path = (const char __far *)MK_FP(regs->ds, regs->bx);
        const char __far *dst_path = (const char __far *)MK_FP(regs->es, regs->di);
        regs->ax = (fat16_rename_file(src_path, dst_path) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_VOL_INFO: {
        os_vol_info_t __far *info = (os_vol_info_t __far *)MK_FP(regs->es, regs->di);
        int res = vol_get_info((uint8_t)(regs->bx & 0xFF), info);
        regs->ax = (res < 0) ? 0xFFFF : 0;
        break;
    }

    case SYS_DISKCHANGE: {
        const char __far *dev = regs->bx ? (const char __far *)MK_FP(regs->ds, regs->bx) : 0;
        regs->ax = (vol_diskchange(dev) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_VOL_FORMAT: {
        const char __far *dev   = regs->bx ? (const char __far *)MK_FP(regs->ds, regs->bx) : 0;
        const char __far *label = (regs->es != 0 && regs->di != 0) ? (const char __far *)MK_FP(regs->es, regs->di) : 0;
        uint16_t flags = regs->cx;
        int res = fat16_format_volume(dev, label, flags);
        regs->ax = (res < 0) ? (uint16_t)res : 0;
        break;
    }

    case SYS_EXEC: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        const char __far *args  = regs->di ? (const char __far *)MK_FP(regs->es, regs->di) : 0;
        int res = exec_program(fname, args);
        con_set_scroll_rows(25);
        regs->ax = (res < 0) ? 0xFFFF : (uint16_t)res;
        break;
    }

    case SYS_GET_PATH: {
        char __far *buf = (char __far *)MK_FP(regs->es, regs->di);
        regs->ax = (path_get(buf, regs->cx) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_SET_PATH: {
        const char __far *p = (const char __far *)MK_FP(regs->es, regs->di);
        regs->ax = (path_set(p) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_DISK_GET_GEOM: {
        os_disk_geom_t __far *geom = (os_disk_geom_t __far *)MK_FP(regs->es, regs->di);
        int res = disk_get_geom((uint8_t)(regs->bx & 0xFF), geom);
        regs->ax = (res < 0) ? 0xFFFF : 0;
        break;
    }

    case SYS_PART_READ: {
        os_part_table_t __far *table = (os_part_table_t __far *)MK_FP(regs->es, regs->di);
        int res = disk_part_read((uint8_t)(regs->bx & 0xFF), table);
        regs->ax = (res < 0) ? 0xFFFF : 0;
        break;
    }

    case SYS_PART_WRITE: {
        const os_part_table_t __far *table = (const os_part_table_t __far *)MK_FP(regs->es, regs->di);
        uint16_t write_mbr = regs->cx;
        int res = disk_part_write((uint8_t)(regs->bx & 0xFF), table, write_mbr);
        regs->ax = (res < 0) ? 0xFFFF : 0;
        break;
    }

    case SYS_VOL_READ_SECTOR: {
        const char __far *vol = regs->bx ? (const char __far *)MK_FP(regs->ds, regs->bx) : 0;
        uint32_t lba = ((uint32_t)regs->cx << 16) | regs->dx;
        void __far *buf = MK_FP(regs->es, regs->di);
        int res = vol_read_sector(vol, lba, buf);
        regs->ax = (res == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_VOL_WRITE_SECTOR: {
        const char __far *vol = regs->bx ? (const char __far *)MK_FP(regs->ds, regs->bx) : 0;
        uint32_t lba = ((uint32_t)regs->cx << 16) | regs->dx;
        const void __far *buf = MK_FP(regs->es, regs->di);
        int res = vol_write_sector(vol, lba, buf);
        regs->ax = (res == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_RAMDISK_MOUNT: {
        uint16_t out_size = 0;
        int res = ramdisk_mount((uint8_t)(regs->bx & 0xFF), regs->dx, (uint16_t __far *)&out_size);
        regs->ax = (uint16_t)res;
        regs->dx = out_size;
        if (regs->es || regs->di) {
            uint16_t __far *out_ptr = (uint16_t __far *)MK_FP(regs->es, regs->di);
            *out_ptr = out_size;
        }
        break;
    }

    case SYS_RAMDISK_UNMOUNT:
        regs->ax = (ramdisk_unmount() == 0) ? 0 : 0xFFFF;
        break;

    case SYS_RAMDISK_STATUS: {
        os_ramdisk_status_t __far *st = (regs->es || regs->di) ?
            (os_ramdisk_status_t __far *)MK_FP(regs->es, regs->di) : 0;
        regs->ax = (uint16_t)ramdisk_status(st);
        break;
    }

    case SYS_FILE_READ_OFFSET: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        void __far *dest        = MK_FP(regs->es, regs->di);
        uint32_t offset         = ((uint32_t)regs->dx << 16) | regs->si;
        int32_t res = fat16_read_file_offset(fname, dest, regs->cx, offset);
        regs->ax = (uint16_t)res;
        break;
    }

    case SYS_FILE_WRITE_OFFSET: {
        const char __far *fname = (const char __far *)MK_FP(regs->ds, regs->bx);
        const void __far *src   = MK_FP(regs->es, regs->di);
        uint32_t offset         = ((uint32_t)regs->dx << 16) | regs->si;
        int32_t res = fat16_write_file_offset(fname, src, regs->cx, offset);
        regs->ax = (res < 0) ? 0xFFFF : (uint16_t)res;
        break;
    }

    case SYS_MEM_ALLOC: {
        uint16_t owner = (s_current_proc_owner != 0) ? s_current_proc_owner : regs->cs;
        regs->ax = mem_alloc(regs->bx, owner);
        break;
    }

    case SYS_MEM_FREE:
        regs->ax = (mem_free(regs->bx) == 0) ? 0 : 0xFFFF;
        break;

    case SYS_MEM_AVAIL:
        regs->ax = mem_avail_paragraphs();
        regs->dx = mem_max_avail_paragraphs();
        break;

    case SYS_MEM_RESIZE:
        regs->ax = (mem_resize(regs->bx, regs->cx) == 0) ? 0 : 0xFFFF;
        break;

    case SYS_MEM_REALLOC: {
        uint16_t owner = (s_current_proc_owner != 0) ? s_current_proc_owner : regs->cs;
        regs->ax = mem_realloc(regs->bx, regs->cx, owner);
        break;
    }

    case SYS_EMS_ALLOC: {
        uint16_t owner = (s_current_proc_owner != 0) ? s_current_proc_owner : regs->cs;
        int page = ems_alloc_page(owner);
        regs->ax = (page < 0) ? 0xFFFF : (uint16_t)page;
        break;
    }

    case SYS_EMS_FREE:
        regs->ax = (ems_free_page(regs->bx) == 0) ? 0 : 0xFFFF;
        break;

    case SYS_EMS_MAP:
        regs->ax = ems_map_page(al, regs->bx);
        break;

    case SYS_EMS_AVAIL:
        regs->ax = ems_avail_pages();
        regs->dx = ems_total_pages();
        break;

    case SYS_XMS_ALLOC: {
        uint16_t owner = (s_current_proc_owner != 0) ? s_current_proc_owner : regs->cs;
        uint16_t h = xms_alloc(regs->bx, owner);
        regs->ax = (h == 0) ? 0xFFFF : h;
        break;
    }

    case SYS_XMS_FREE:
        regs->ax = (xms_free(regs->bx) == 0) ? 0 : 0xFFFF;
        break;

    case SYS_XMS_MOVE: {
        const os_xms_move_t __far *req = (const os_xms_move_t __far *)MK_FP(regs->es, regs->di);
        regs->ax = (xms_move(req) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_XMS_AVAIL:
        regs->ax = xms_avail_kb();
        regs->dx = xms_total_kb();
        break;

    case SYS_KEYB_SET_MAP: {
        const os_keymap_t __far *map = (const os_keymap_t __far *)MK_FP(regs->es, regs->di);
        regs->ax = (kbd_set_map(map) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_KEYB_GET_NAME: {
        char __far *name_buf = (char __far *)MK_FP(regs->es, regs->di);
        regs->ax = (kbd_get_name(name_buf) == 0) ? 0 : 0xFFFF;
        break;
    }

    case SYS_KEYB_STATE:
        regs->ax = (uint16_t)kbd_get_state((uint8_t)(regs->bx & 0xFF));
        break;

    case SYS_GFX_SET_MODE:
        regs->ax = (gfx_set_mode(al) == 0) ? 0 : 0xFFFF;
        break;

    case SYS_GFX_GET_MODE:
        regs->ax = (uint16_t)gfx_get_mode();
        break;

    case SYS_GFX_PALETTE:
        gfx_set_palette((uint8_t)(regs->bx & 0xFF),
                        (uint8_t)(regs->bx >> 8),
                        (uint8_t)(regs->cx & 0xFF));
        break;

    case SYS_GFX_VSYNC:
        gfx_vsync();
        break;

    case SYS_GFX_CLEAR:
        gfx_clear(al);
        break;

    case SYS_GFX_PUTPIXEL:
        gfx_putpixel(regs->cx, regs->dx, al);
        break;

    case SYS_GFX_LINE: {
        const os_gfx_line_t __far *ln = (const os_gfx_line_t __far *)MK_FP(regs->es, regs->di);
        if (ln) {
            gfx_line(ln->x0, ln->y0, ln->x1, ln->y1, ln->color);
        }
        break;
    }

    case SYS_GFX_RECT: {
        const os_gfx_rect_t __far *rc = (const os_gfx_rect_t __far *)MK_FP(regs->es, regs->di);
        if (rc) {
            gfx_rect(rc->x, rc->y, rc->w, rc->h, rc->color, rc->filled);
        }
        break;
    }

    case SYS_GFX_TEXT: {
        const char __far *str = (const char __far *)MK_FP(regs->es, regs->di);
        gfx_draw_text(regs->cx, regs->dx, str, al);
        break;
    }

    case SYS_GFX_BLIT: {
        const void __far *buf = MK_FP(regs->es, regs->di);
        gfx_blit(buf);
        break;
    }

    case SYS_GFX_SET_DAC:
        gfx_set_dac((uint8_t)(regs->bx & 0xFF),
                    (uint8_t)(regs->cx & 0xFF),
                    (uint8_t)(regs->cx >> 8),
                    (uint8_t)(regs->dx & 0xFF));
        break;

    case SYS_GFX_SYNC:
        gfx_cga_lcd_sync();
        regs->ax = 0;
        break;

    case SYS_SOUND_INFO:
        regs->ax = (uint16_t)sound_get_device();
        regs->bx = sound_get_port();
        regs->cx = sound_get_channels();
        regs->dx = ((uint16_t)sound_get_sb_type() << 8) | (sound_get_sb_base() & 0xFF);
        break;

    case SYS_SOUND_TONE:
        sound_tone(regs->bx, regs->cx, (uint8_t)(regs->dx & 0xFF));
        break;

    case SYS_SOUND_STOP:
        sound_stop();
        break;

    case SYS_SOUND_OPL_WRITE:
        sound_opl_write(regs->bx, (uint8_t)(regs->cx & 0xFF));
        break;

    case SYS_SOUND_GET_VOL:
        regs->ax = (uint16_t)sound_get_volume((uint8_t)(regs->bx & 0xFF));
        break;

    case SYS_SOUND_SET_VOL:
        regs->ax = (uint16_t)sound_set_volume((uint8_t)(regs->bx & 0xFF), (uint8_t)(regs->cx & 0xFF));
        break;

    case SYS_SOUND_PCM_PLAY:
        regs->ax = (uint16_t)sound_pcm_play(regs->bx, regs->cx,
            (const uint8_t __far *)MK_FP(regs->es, regs->di));
        break;

    case SYS_SOUND_PCM_STATUS:
        regs->ax = (uint16_t)sound_pcm_status();
        break;

    case SYS_SOUND_PCM_STOP:
        sound_pcm_stop();
        regs->ax = 0;
        break;

    case SYS_SOUND_BG_PLAY:
        regs->ax = (uint16_t)sound_bg_play(regs->bx, (uint8_t)(regs->cx & 0xFF), regs->dx,
            (const uint8_t __far *)MK_FP(regs->es, regs->di),
            (const char __far *)MK_FP(regs->ds, regs->si));
        break;

    case SYS_SOUND_BG_STOP:
        sound_bg_stop();
        regs->ax = 0;
        break;

    case SYS_SOUND_BG_STATUS:
        regs->ax = (uint16_t)sound_bg_get_status((os_sound_bg_status_t __far *)MK_FP(regs->es, regs->di));
        break;

    default:
        regs->ax = 0xFFFF;
        break;
    }
}
