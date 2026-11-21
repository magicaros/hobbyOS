/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CPM.BIN - CP/M-80 2.2 Execution Subsystem for hobbyOS.
 * Supports both NEC V20 hardware 8080 emulation (BRKEM/CALLN/RETEM)
 * and Intel 8088 pure software 8080 interpreter fallback.
 * Includes user-space ANSI/VT52/ADM-3A terminal emulation and FAT16 FCB bridge.
 */

#include "hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

uint16_t get_ds(void);
#pragma aux get_ds = "mov ax, ds" value [ax];

uint16_t get_cs(void);
#pragma aux get_cs = "mov ax, cs" value [ax];

#define CPM_TPA_BASE        0x0100
#define CPM_STACK_TOP       0xFC00
#define CPM_BDOS_ENTRY      0xFD00
#define CPM_WARM_BOOT       0x0000
#define CPM_DEFAULT_DMA     0x0080
#define CPM_DEFAULT_FCB1    0x005C
#define CPM_DEFAULT_FCB2    0x006C

#define TERM_STATE_NORMAL   0
#define TERM_STATE_ESC      1
#define TERM_STATE_CSI      2
#define TERM_STATE_ROW      3
#define TERM_STATE_COL      4

/* Global CP/M Runtime State */
static uint16_t s_cpm_seg = 0;
static uint16_t s_cpm_dma = CPM_DEFAULT_DMA;
static uint8_t  s_cpm_cur_disk = 0;
int             s_cpm_running = 0;
static uint8_t  s_cpu_type = 0;

/* Terminal state */
static int      s_term_state = TERM_STATE_NORMAL;
static int      s_term_row = 0;
static int      s_term_col = 0;
static uint8_t  s_term_attr = 0x07;
static int      s_csi_p1 = 0;
static int      s_csi_p2 = 0;
static int      s_csi_has_p2 = 0;
static int      s_term_saved_row = 0;
static int      s_term_saved_col = 0;
static int      s_term_tmp_row = 0;
static uint16_t s_vram_seg = 0xB800;

/* Search First/Next state */
static uint16_t s_search_idx = 0;
static char     s_search_mask[16];

/* Saved IVT vectors for V20 mode */
static uint16_t s_orig_e0_ip = 0, s_orig_e0_cs = 0;
static uint16_t s_orig_e1_ip = 0, s_orig_e1_cs = 0;
static uint16_t s_orig_e2_ip = 0, s_orig_e2_cs = 0;
static uint16_t s_orig_e3_ip = 0, s_orig_e3_cs = 0;

/* 8080 Parity Lookup Table */
static uint8_t s_parity[256];

/* --------------------------------------------------------------------------
 * String and Math Utilities
 * -------------------------------------------------------------------------- */

static int cpm_strlen(const char *s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

static char cpm_toupper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static int cpm_strnicmp(const char *a, const char *b, int n) {
    int i = 0;
    while (i < n && a[i] && b[i]) {
        char ca = cpm_toupper(a[i]);
        char cb = cpm_toupper(b[i]);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        i++;
    }
    if (i == n) return 0;
    return (int)(unsigned char)cpm_toupper(a[i]) - (int)(unsigned char)cpm_toupper(b[i]);
}

static void cpm_init_parity(void) {
    int i;
    for (i = 0; i < 256; i++) {
        uint8_t p = 1;
        uint8_t v = (uint8_t)i;
        while (v) {
            p ^= (v & 1);
            v >>= 1;
        }
        s_parity[i] = p;
    }
}

/* --------------------------------------------------------------------------
 * Terminal Emulation (ANSI / VT52 / ADM-3A)
 * -------------------------------------------------------------------------- */

extern void cpm_vram_init(uint16_t vram_seg, uint16_t has_snow);
extern void cpm_vram_write_cell(uint16_t idx, uint16_t cell);

/* Screen shadow buffer: avoids touching VRAM if unchanged, completely eliminating snow and flicker */
static uint16_t s_term_shadow[80 * 25];

static void cpm_write_cell(int row, int col, uint16_t cell) {
    int idx;
    if (row < 0 || row >= 25 || col < 0 || col >= 80) return;
    idx = row * 80 + col;
    if (s_term_shadow[idx] == cell) return;
    s_term_shadow[idx] = cell;
    cpm_vram_write_cell((uint16_t)idx, cell);
}

static void cpm_term_cls(uint8_t attr) {
    uint16_t blank = ((uint16_t)attr << 8) | ' ';
    int i;
    os_cls(attr);
    for (i = 0; i < 80 * 25; i++) {
        s_term_shadow[i] = blank;
    }
    s_term_row = 0;
    s_term_col = 0;
    os_set_cursor(0, 0);
}

static void cpm_term_erase_eol(void) {
    uint16_t blank = ((uint16_t)s_term_attr << 8) | ' ';
    int c;
    for (c = s_term_col; c < 80; c++) {
        cpm_write_cell(s_term_row, c, blank);
    }
}

static void cpm_term_erase_eos(void) {
    uint16_t blank = ((uint16_t)s_term_attr << 8) | ' ';
    int r, c;
    if (s_term_row == 0 && s_term_col == 0) {
        cpm_term_cls(s_term_attr);
        return;
    }
    for (c = s_term_col; c < 80; c++) {
        cpm_write_cell(s_term_row, c, blank);
    }
    for (r = s_term_row + 1; r < 25; r++) {
        for (c = 0; c < 80; c++) {
            cpm_write_cell(r, c, blank);
        }
    }
}

static void cpm_term_scroll_up(void) {
    uint16_t blank = ((uint16_t)s_term_attr << 8) | ' ';
    int r, c;
    for (r = 0; r < 24; r++) {
        for (c = 0; c < 80; c++) {
            cpm_write_cell(r, c, s_term_shadow[(r + 1) * 80 + c]);
        }
    }
    for (c = 0; c < 80; c++) {
        cpm_write_cell(24, c, blank);
    }
}

static void cpm_term_scroll_down(void) {
    uint16_t blank = ((uint16_t)s_term_attr << 8) | ' ';
    int r, c;
    for (r = 23; r >= 0; r--) {
        for (c = 0; c < 80; c++) {
            cpm_write_cell(r + 1, c, s_term_shadow[r * 80 + c]);
        }
    }
    for (c = 0; c < 80; c++) {
        cpm_write_cell(0, c, blank);
    }
}

/* Televideo / WordStar Line Insert: shifts rows s_term_row..22 down to 23; clears row s_term_row */
static void cpm_term_insert_line(void) {
    uint16_t blank = ((uint16_t)s_term_attr << 8) | ' ';
    int r, c;
    int bottom = 23;
    if (s_term_row > bottom) return;
    for (r = bottom - 1; r >= s_term_row; r--) {
        for (c = 0; c < 80; c++) {
            cpm_write_cell(r + 1, c, s_term_shadow[r * 80 + c]);
        }
    }
    for (c = 0; c < 80; c++) {
        cpm_write_cell(s_term_row, c, blank);
    }
}

/* Televideo / WordStar Line Delete: shifts rows s_term_row+1..23 up to s_term_row..22; clears row 23 */
static void cpm_term_delete_line(void) {
    uint16_t blank = ((uint16_t)s_term_attr << 8) | ' ';
    int r, c;
    int bottom = 23;
    if (s_term_row > bottom) return;
    for (r = s_term_row; r < bottom; r++) {
        for (c = 0; c < 80; c++) {
            cpm_write_cell(r, c, s_term_shadow[(r + 1) * 80 + c]);
        }
    }
    for (c = 0; c < 80; c++) {
        cpm_write_cell(bottom, c, blank);
    }
}

static void cpm_vram_setup(void) {
    uint16_t vtype = os_video_type();
    uint16_t seg = (vtype == VIDEO_TYPE_MDA) ? 0xB000 : 0xB800;
    uint16_t has_snow = (vtype == VIDEO_TYPE_CGA) ? 1 : 0;
    uint16_t cpos;

    s_vram_seg = seg;
    cpm_vram_init(seg, has_snow);

    os_console_get_shadow(s_term_shadow);

    cpos = os_get_cursor();
    s_term_row = (int)(cpos >> 8);
    s_term_col = (int)(cpos & 0xFF);
    if (s_term_row > 24) s_term_row = 24;
    if (s_term_col > 79) s_term_col = 79;
    s_term_attr = 0x07;
    s_term_state = TERM_STATE_NORMAL;
}

static void cpm_term_putc(char ch) {
    if (s_term_state == TERM_STATE_NORMAL) {
        if (ch == 0x1B) {
            s_term_state = TERM_STATE_ESC;
        } else if (ch == 0x07) {
            /* Bell - ignore */
        } else if (ch == 0x08) {
            /* Backspace */
            if (s_term_col > 0) {
                s_term_col--;
                os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            }
        } else if (ch == 0x09) {
            /* Tab */
            s_term_col = (s_term_col & ~7) + 8;
            if (s_term_col > 79) s_term_col = 79;
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
        } else if (ch == 0x0A) {
            /* Line Feed */
            if (s_term_row < 24) {
                s_term_row++;
            } else {
                cpm_term_scroll_up();
            }
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
        } else if (ch == 0x0C || ch == 0x1A) {
            /* Form Feed / CP/M ADM-3A Clear Screen */
            cpm_term_cls(s_term_attr);
        } else if (ch == 0x0D) {
            /* Carriage Return */
            s_term_col = 0;
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
        } else if ((unsigned char)ch >= 32) {
            cpm_write_cell(s_term_row, s_term_col, ((uint16_t)s_term_attr << 8) | (uint8_t)ch);
            if (s_term_col < 79) {
                s_term_col++;
            } else {
                /* Wrap to next line column 0 */
                s_term_col = 0;
                if (s_term_row < 24) {
                    s_term_row++;
                } else {
                    cpm_term_scroll_up();
                }
            }
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
        }
    } else if (s_term_state == TERM_STATE_ESC) {
        if (ch == '[') {
            s_term_state = TERM_STATE_CSI;
            s_csi_p1 = 0;
            s_csi_p2 = 0;
            s_csi_has_p2 = 0;
        } else if (ch == '=' || ch == 'Y') {
            /* ADM-3A (ESC =) and DEC VT52 (ESC Y) cursor addressing: row+32 col+32 */
            s_term_state = TERM_STATE_ROW;
        } else if (ch == 'K' || ch == 'T') {
            /* Erase to end of line (VT52 ESC K / Televideo ESC T) */
            cpm_term_erase_eol();
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'J') {
            /* Erase to end of screen (VT52 ESC J) */
            cpm_term_erase_eos();
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'E') {
            /* Insert Line (Zenith Z-19 ESC E) */
            cpm_term_insert_line();
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'R') {
            /* Delete Line (Zenith Z-19 ESC R) */
            cpm_term_delete_line();
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == '*') {
            /* Clear screen */
            cpm_term_cls(s_term_attr);
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'H') {
            /* Cursor Home */
            s_term_row = 0;
            s_term_col = 0;
            os_set_cursor(0, 0);
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'A') {
            /* Cursor Up */
            if (s_term_row > 0) s_term_row--;
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'B') {
            /* Cursor Down */
            if (s_term_row < 24) s_term_row++;
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'C') {
            /* Cursor Right */
            if (s_term_col < 79) s_term_col++;
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'D') {
            /* Cursor Left */
            if (s_term_col > 0) s_term_col--;
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == 'I') {
            /* Reverse Line Feed (scroll down) */
            cpm_term_scroll_down();
            os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == '4' || ch == '(' || ch == 'p') {
            /* Bright / Reverse Video ON */
            s_term_attr = 0x0F;
            s_term_state = TERM_STATE_NORMAL;
        } else if (ch == '5' || ch == ')' || ch == 'q') {
            /* Normal Video */
            s_term_attr = 0x07;
            s_term_state = TERM_STATE_NORMAL;
        } else {
            /* Drop unknown/unsupported 2-byte escape safely */
            s_term_state = TERM_STATE_NORMAL;
        }
    } else if (s_term_state == TERM_STATE_ROW) {
        s_term_tmp_row = (int)((unsigned char)ch) - 32;
        if (s_term_tmp_row < 0) s_term_tmp_row = 0;
        if (s_term_tmp_row > 24) s_term_tmp_row = 24;
        s_term_state = TERM_STATE_COL;
    } else if (s_term_state == TERM_STATE_COL) {
        int target_col = (int)((unsigned char)ch) - 32;
        if (target_col < 0) target_col = 0;
        if (target_col > 79) target_col = 79;
        s_term_row = s_term_tmp_row;
        s_term_col = target_col;
        os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
        s_term_state = TERM_STATE_NORMAL;
    } else if (s_term_state == TERM_STATE_CSI) {
        if (ch >= '0' && ch <= '9') {
            if (!s_csi_has_p2) {
                s_csi_p1 = (s_csi_p1 * 10) + (ch - '0');
            } else {
                s_csi_p2 = (s_csi_p2 * 10) + (ch - '0');
            }
        } else if (ch == ';') {
            s_csi_has_p2 = 1;
        } else {
            /* ANSI Command character */
            if (ch == 'H' || ch == 'f') {
                int r = s_csi_p1 ? s_csi_p1 - 1 : 0;
                int c = s_csi_p2 ? s_csi_p2 - 1 : 0;
                if (r < 0) r = 0; if (r > 24) r = 24;
                if (c < 0) c = 0; if (c > 79) c = 79;
                s_term_row = r;
                s_term_col = c;
                os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            } else if (ch == 'J') {
                if (s_csi_p1 == 2) {
                    cpm_term_cls(s_term_attr);
                } else if (s_csi_p1 == 0) {
                    cpm_term_erase_eos();
                }
            } else if (ch == 'K') {
                cpm_term_erase_eol();
            } else if (ch == 's') {
                s_term_saved_row = s_term_row;
                s_term_saved_col = s_term_col;
            } else if (ch == 'u') {
                s_term_row = s_term_saved_row;
                s_term_col = s_term_saved_col;
                os_set_cursor((uint8_t)s_term_row, (uint8_t)s_term_col);
            } else if (ch == 'm') {
                if (s_csi_p1 == 0) s_term_attr = 0x07;
                else if (s_csi_p1 == 1) s_term_attr = 0x0F;
                else if (s_csi_p1 == 7) s_term_attr = 0x70;
            }
            s_term_state = TERM_STATE_NORMAL;
        }
    }
}

/* --------------------------------------------------------------------------
 * FCB (File Control Block) & Filesystem Bridge
 * -------------------------------------------------------------------------- */

static uint8_t cpm_fcb_search_next(void);

static void fcb_to_dos_name(const uint8_t __far *fcb, char *dos_name) {
    int i, j = 0;
    for (i = 1; i <= 8; i++) {
        char c = (char)(fcb[i] & 0x7F);
        if (c != ' ') {
            dos_name[j++] = cpm_toupper(c);
        }
    }
    if ((fcb[9] & 0x7F) != ' ') {
        dos_name[j++] = '.';
        for (i = 9; i <= 11; i++) {
            char c = (char)(fcb[i] & 0x7F);
            if (c != ' ') {
                dos_name[j++] = cpm_toupper(c);
            }
        }
    }
    dos_name[j] = '\0';
}

static void dos_name_to_fcb(const char *dos_name, uint8_t __far *fcb) {
    int i, j;
    for (i = 0; i < 36; i++) fcb[i] = 0;
    for (i = 1; i <= 11; i++) fcb[i] = ' ';

    if (!dos_name || !*dos_name) {
        fcb[0] = ' '; /* Empty FCB in CP/M starts with space (0x20) */
        return;
    }

    /* Check for drive letter prefix e.g. A: or B: */
    if (dos_name[0] && dos_name[1] == ':') {
        char d = cpm_toupper(dos_name[0]);
        if (d >= 'A' && d <= 'P') {
            fcb[0] = (uint8_t)(d - 'A' + 1);
        }
        dos_name += 2;
    } else {
        fcb[0] = 0; /* Default drive */
    }

    j = 1;
    while (*dos_name && *dos_name != '.' && j <= 8) {
        fcb[j++] = (uint8_t)cpm_toupper(*dos_name++);
    }
    while (*dos_name && *dos_name != '.') dos_name++;
    if (*dos_name == '.') {
        dos_name++;
        j = 9;
        while (*dos_name && j <= 11) {
            fcb[j++] = (uint8_t)cpm_toupper(*dos_name++);
        }
    }
}

static uint8_t cpm_fcb_open(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    char dos_name[16];
    long sz;
    uint32_t total_recs;
    uint32_t start_rec;
    uint32_t recs_in_extent;
    uint8_t extent;

    fcb_to_dos_name(fcb, dos_name);
    sz = os_file_size(dos_name);
    if (sz < 0) return 0xFF; /* Not found */

    extent = fcb[12] & 0x1F;
    total_recs = (uint32_t)((sz + 127) >> 7);
    start_rec = (uint32_t)extent << 7; /* extent * 128 */

    if (total_recs > 0 && start_rec >= total_recs) {
        return 0xFF; /* Extent beyond EOF */
    }

    recs_in_extent = (total_recs > start_rec) ? (total_recs - start_rec) : 0;
    if (recs_in_extent > 128) recs_in_extent = 128;
    fcb[15] = (uint8_t)recs_in_extent; /* Record count for this extent */
    fcb[16] = 1; /* Dummy allocation block so extent is marked allocated */
    fcb[32] = 0; /* Current record within extent */
    return 0;
}

static uint8_t cpm_fcb_close(uint16_t fcb_addr) {
    (void)fcb_addr;
    return 0;
}

static uint8_t cpm_fcb_read_seq(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    uint8_t __far *dma = (uint8_t __far *)MK_FP(s_cpm_seg, s_cpm_dma);
    char dos_name[16];
    uint32_t rec_num;
    uint32_t byte_off;
    uint16_t nread;
    int k;

    fcb_to_dos_name(fcb, dos_name);
    rec_num = ((uint32_t)(fcb[12] & 0x1F) << 7) | (fcb[32] & 0x7F);
    byte_off = rec_num << 7; /* rec_num * 128 */

    nread = os_file_read_offset(dos_name, dma, 128, byte_off);
    if ((int16_t)nread <= 0) return 1; /* EOF */

    /* Pad remaining bytes with 0x1A (EOF) */
    for (k = nread; k < 128; k++) {
        dma[k] = 0x1A;
    }

    /* Advance current record */
    fcb[32]++;
    if (fcb[32] >= 128) {
        long sz;
        fcb[32] = 0;
        fcb[12]++;
        sz = os_file_size(dos_name);
        if (sz > 0) {
            uint32_t total_recs = (uint32_t)((sz + 127) >> 7);
            uint32_t start_rec = (uint32_t)(fcb[12] & 0x1F) << 7;
            if (start_rec < total_recs) {
                uint32_t recs = total_recs - start_rec;
                if (recs > 128) recs = 128;
                fcb[15] = (uint8_t)recs;
            } else {
                fcb[15] = 0;
            }
        }
    }
    return 0;
}

static uint8_t cpm_fcb_write_seq(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    uint8_t __far *dma = (uint8_t __far *)MK_FP(s_cpm_seg, s_cpm_dma);
    char dos_name[16];
    long fsize;

    fcb_to_dos_name(fcb, dos_name);
    fsize = os_file_size(dos_name);
    if (fsize <= 0) {
        os_file_write(dos_name, dma, 128);
    } else {
        os_file_append(dos_name, dma, 128);
    }

    fcb[32]++;
    if (fcb[32] >= 128) {
        fcb[32] = 0;
        fcb[12]++;
    }
    return 0;
}

static uint8_t cpm_fcb_read_rand(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    uint8_t __far *dma = (uint8_t __far *)MK_FP(s_cpm_seg, s_cpm_dma);
    char dos_name[16];
    uint32_t rec_num;
    uint32_t byte_off;
    long fsize;
    uint16_t nread;
    int k;

    fcb_to_dos_name(fcb, dos_name);
    fsize = os_file_size(dos_name);
    if (fsize < 0) return 1; /* File not found or unwritten */

    rec_num = (uint32_t)fcb[33] | ((uint32_t)fcb[34] << 8) | ((uint32_t)fcb[35] << 16);
    byte_off = rec_num << 7; /* rec_num * 128 */

    /* Update extent and current record fields in FCB */
    fcb[12] = (uint8_t)((rec_num >> 7) & 0x1F);
    fcb[32] = (uint8_t)(rec_num & 0x7F);
    fcb[16] = 1;
    if (fsize > 0) {
        uint32_t total_recs = (uint32_t)((fsize + 127) >> 7);
        uint32_t start_rec = (uint32_t)(fcb[12] & 0x1F) << 7;
        if (start_rec < total_recs) {
            uint32_t recs = total_recs - start_rec;
            if (recs > 128) recs = 128;
            fcb[15] = (uint8_t)recs;
        } else {
            fcb[15] = 0;
        }
    }
    fcb[16] = 1;

    if (byte_off >= (uint32_t)fsize) {
        return 1; /* Reading unwritten data / EOF */
    }

    nread = os_file_read_offset(dos_name, dma, 128, byte_off);
    if ((int16_t)nread <= 0) return 1; /* EOF */

    /* Pad remaining bytes with 0x1A (EOF) */
    for (k = nread; k < 128; k++) {
        dma[k] = 0x1A;
    }
    return 0;
}

static uint8_t cpm_fcb_write_rand(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    uint8_t __far *dma = (uint8_t __far *)MK_FP(s_cpm_seg, s_cpm_dma);
    char dos_name[16];
    uint32_t rec_num;
    uint32_t byte_off;
    long fsize;

    fcb_to_dos_name(fcb, dos_name);
    fsize = os_file_size(dos_name);
    if (fsize < 0) {
        /* File doesn't exist yet, create it */
        os_file_write(dos_name, (const void __far *)0, 0);
        fsize = 0;
    }

    rec_num = (uint32_t)fcb[33] | ((uint32_t)fcb[34] << 8) | ((uint32_t)fcb[35] << 16);
    byte_off = rec_num << 7; /* rec_num * 128 */

    /* Update extent and current record fields in FCB */
    fcb[12] = (uint8_t)((rec_num >> 7) & 0x1F);
    fcb[32] = (uint8_t)(rec_num & 0x7F);
    fcb[16] = 1;

    if (byte_off == (uint32_t)fsize) {
        /* Appending exactly at end of file */
        if (fsize == 0) {
            os_file_write(dos_name, dma, 128);
        } else {
            os_file_append(dos_name, dma, 128);
        }
    } else if (byte_off > (uint32_t)fsize) {
        /* Seeking past end of file: pad gap with zeros */
        uint8_t zero_buf[128];
        uint32_t gap = byte_off - (uint32_t)fsize;
        int i;
        for (i = 0; i < 128; i++) zero_buf[i] = 0;
        while (gap > 0) {
            uint16_t chunk = (gap > 128) ? 128 : (uint16_t)gap;
            if (fsize == 0) {
                os_file_write(dos_name, (const void __far *)FP_HERE(zero_buf), chunk);
                fsize = chunk;
            } else {
                os_file_append(dos_name, (const void __far *)FP_HERE(zero_buf), chunk);
            }
            gap -= chunk;
        }
        os_file_append(dos_name, dma, 128);
    } else {
        /* In-place overwrite within existing file */
        uint32_t new_sz = (uint32_t)fsize;
        uint16_t paras;
        uint16_t scratch_seg;

        if (byte_off + 128 > new_sz) {
            new_sz = byte_off + 128;
        }
        paras = (uint16_t)((new_sz + 15) >> 4);
        scratch_seg = os_mem_alloc(paras);
        if (scratch_seg != 0) {
            int k;
            uint8_t __far *dst;
            if (fsize > 0) {
                os_file_read(dos_name, MK_FP(scratch_seg, 0), (uint16_t)fsize);
            }
            dst = (uint8_t __far *)MK_FP(scratch_seg, (uint16_t)byte_off);
            for (k = 0; k < 128; k++) dst[k] = dma[k];
            os_file_write(dos_name, MK_FP(scratch_seg, 0), (uint16_t)new_sz);
            os_mem_free(scratch_seg);
        } else {
            return 2; /* Out of disk/memory space error */
        }
    }
    return 0;
}

static void cpm_fcb_set_rand_rec(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    uint32_t rec_num = ((uint32_t)(fcb[12] & 0x1F) << 7) | (uint32_t)(fcb[32] & 0x7F);
    fcb[33] = (uint8_t)(rec_num & 0xFF);
    fcb[34] = (uint8_t)((rec_num >> 8) & 0xFF);
    fcb[35] = (uint8_t)((rec_num >> 16) & 0xFF);
}

static uint8_t cpm_fcb_delete(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    char dos_name[16];
    fcb_to_dos_name(fcb, dos_name);
    if (os_file_delete(dos_name) == 0) return 0;
    return 0xFF;
}

static uint8_t cpm_fcb_search_first(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    fcb_to_dos_name(fcb, s_search_mask);
    s_search_idx = 0;
    return cpm_fcb_search_next();
}

static uint8_t cpm_fcb_search_next(void) {
    os_dir_entry_t dent;
    while (os_dir_entry_at(".", s_search_idx++, (os_dir_entry_t __far *)FP_HERE(&dent)) == 0) {
        if (dent.attr & (OS_ATTR_DIRECTORY | OS_ATTR_VOLUME_ID)) continue;
        if (s_search_mask[0] == '\0' || s_search_mask[0] == '?' ||
            cpm_strnicmp(dent.name, s_search_mask, cpm_strlen(s_search_mask)) == 0) {
            /* Format 32-byte CP/M directory entry into DMA buffer */
            uint8_t __far *dma = (uint8_t __far *)MK_FP(s_cpm_seg, s_cpm_dma);
            uint32_t recs;
            dos_name_to_fcb(dent.name, dma);
            recs = (dent.size + 127) >> 7;
            if (recs > 128) recs = 128;
            dma[15] = (uint8_t)recs;
            dma[16] = 1;
            return 0; /* Index 0 in DMA buffer */
        }
    }
    return 0xFF; /* No more files */
}

static uint8_t cpm_fcb_make(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    char dos_name[16];
    fcb_to_dos_name(fcb, dos_name);
    os_file_delete(dos_name);
    os_file_write(dos_name, (void __far *)0, 0);
    fcb[12] = 0;
    fcb[14] = 0;
    fcb[15] = 0;
    fcb[32] = 0;
    return 0;
}

static uint8_t cpm_fcb_rename(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    char old_name[16];
    char new_name[16];
    fcb_to_dos_name(fcb, old_name);
    fcb_to_dos_name(fcb + 16, new_name);
    if (os_file_rename(old_name, (const char __far *)FP_HERE(new_name)) == 0) return 0;
    return 0xFF;
}

static uint8_t cpm_fcb_file_size(uint16_t fcb_addr) {
    uint8_t __far *fcb = (uint8_t __far *)MK_FP(s_cpm_seg, fcb_addr);
    char dos_name[16];
    long sz;
    fcb_to_dos_name(fcb, dos_name);
    sz = os_file_size(dos_name);
    if (sz < 0) return 0xFF;
    {
        uint32_t recs = (uint32_t)((sz + 127) >> 7);
        fcb[33] = (uint8_t)(recs & 0xFF);
        fcb[34] = (uint8_t)((recs >> 8) & 0xFF);
        fcb[35] = (uint8_t)((recs >> 16) & 0xFF);
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Direct CP/M BIOS Dispatcher (WBOOT, CONST, CONIN, CONOUT)
 * -------------------------------------------------------------------------- */

uint8_t __cdecl cpm_bios_dispatch(uint8_t func, uint8_t c);

uint8_t __cdecl cpm_bios_dispatch(uint8_t func, uint8_t c) {
    if (func == 2) {
        /* CONST: Console Status (0xFF if ready, 0x00 if not) */
        return os_kbhit() ? 0xFF : 0x00;
    } else if (func == 3) {
        /* CONIN: Console Input (raw char, no echo) */
        uint16_t key = os_getc();
        return (uint8_t)(key & 0xFF);
    } else if (func == 4) {
        /* CONOUT: Console Output */
        cpm_term_putc((char)c);
        return 0;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * BDOS System Call Dispatcher (Shared by V20 and Software Emulator)
 * -------------------------------------------------------------------------- */

uint16_t __cdecl cpm_bdos_dispatch(uint8_t func, uint16_t de, uint16_t hl, uint8_t a);

uint16_t __cdecl cpm_bdos_dispatch(uint8_t func, uint16_t de, uint16_t hl, uint8_t a) {
    (void)hl;
    (void)a;

    switch (func) {
    case 0: /* System Reset / Warm Boot */
        s_cpm_running = 0;
        return 0;

    case 1: /* Console Input */
        {
            uint16_t key = os_getc();
            uint8_t ch = (uint8_t)(key & 0xFF);
            cpm_term_putc((char)ch);
            return (uint16_t)ch;
        }

    case 2: /* Console Output */
        cpm_term_putc((char)de);
        return 0;

    case 6: /* Direct Console I/O */
        if ((uint8_t)de == 0xFF) {
            if (os_kbhit()) {
                uint16_t key = os_getc();
                return (uint16_t)(key & 0xFF);
            }
            return 0;
        } else {
            cpm_term_putc((char)de);
            return 0;
        }

    case 9: /* Print String ($ terminated) */
        {
            uint8_t __far *mem = (uint8_t __far *)MK_FP(s_cpm_seg, 0);
            uint16_t ptr = de;
            while (mem[ptr] != '$') {
                cpm_term_putc((char)mem[ptr++]);
            }
            return 0;
        }

    case 10: /* Buffered Console Input */
        {
            uint8_t __far *mem = (uint8_t __far *)MK_FP(s_cpm_seg, 0);
            uint8_t max_len = mem[de];
            uint8_t len = 0;
            while (1) {
                uint16_t key = os_getc();
                uint8_t ch = (uint8_t)(key & 0xFF);
                if (ch == '\r' || ch == '\n') {
                    cpm_term_putc('\r');
                    cpm_term_putc('\n');
                    break;
                } else if (ch == '\b') {
                    if (len > 0) {
                        len--;
                        cpm_term_putc('\b');
                        cpm_term_putc(' ');
                        cpm_term_putc('\b');
                    }
                } else if (ch >= 32 && len < max_len) {
                    mem[de + 2 + len++] = ch;
                    cpm_term_putc((char)ch);
                }
            }
            mem[de + 1] = len;
            return 0;
        }

    case 11: /* Console Status */
        return os_kbhit() ? 0x00FF : 0x0000;

    case 12: /* Return CP/M Version */
        return 0x0022; /* CP/M 2.2 on 8080 */

    case 13: /* Reset Disk System */
        s_cpm_dma = CPM_DEFAULT_DMA;
        return 0;

    case 14: /* Select Disk */
        s_cpm_cur_disk = (uint8_t)de;
        return 0;

    case 15: /* Open File */
        return (uint16_t)cpm_fcb_open(de);

    case 16: /* Close File */
        return (uint16_t)cpm_fcb_close(de);

    case 17: /* Search First */
        return (uint16_t)cpm_fcb_search_first(de);

    case 18: /* Search Next */
        return (uint16_t)cpm_fcb_search_next();

    case 19: /* Delete File */
        return (uint16_t)cpm_fcb_delete(de);

    case 20: /* Read Sequential */
        return (uint16_t)cpm_fcb_read_seq(de);

    case 21: /* Write Sequential */
        return (uint16_t)cpm_fcb_write_seq(de);

    case 22: /* Make File */
        return (uint16_t)cpm_fcb_make(de);

    case 23: /* Rename File */
        return (uint16_t)cpm_fcb_rename(de);

    case 24: /* Return Login Vector */
        return 0x0003; /* Drives A: and B: online */

    case 25: /* Return Current Disk */
        return (uint16_t)s_cpm_cur_disk;

    case 26: /* Set DMA Address */
        s_cpm_dma = de;
        return 0;

    case 27: /* Get Allocation Vector */
        return 0xFE90; /* Pointer to static allocation vector */

    case 28: /* Write Protect Disk */
    case 29: /* Get R/O Vector */
    case 30: /* Set File Attributes */
    case 32: /* Set/Get User Code */
        return 0;

    case 31: /* Get Disk Parameter Block (DPB) */
        return 0xFE80; /* Pointer to static DPB */

    case 33: /* Read Random */
        return (uint16_t)cpm_fcb_read_rand(de);

    case 34: /* Write Random */
    case 40: /* Write Random with Zero Fill */
        return (uint16_t)cpm_fcb_write_rand(de);

    case 35: /* Compute File Size */
        return (uint16_t)cpm_fcb_file_size(de);

    case 36: /* Set Random Record */
        cpm_fcb_set_rand_rec(de);
        return 0;

    default:
        break;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * NEC V20 Hardware 8080 Mode Engine (BRKEM / CALLN / RETEM)
 * -------------------------------------------------------------------------- */

extern void v20_bdos_isr(void);
extern void v20_bios_isr(void);
extern void v20_exit_isr(void);
extern void v20_enter_8080(uint16_t cpm_seg);

static void v20_setup_ivt(uint16_t cpm_seg) {
    uint16_t __far *ivt = (uint16_t __far *)MK_FP(0, 0);

    /* Save original vectors 0xE0, 0xE1, 0xE2, 0xE3 */
    s_orig_e0_ip = ivt[0xE0 * 2 + 0];
    s_orig_e0_cs = ivt[0xE0 * 2 + 1];
    s_orig_e1_ip = ivt[0xE1 * 2 + 0];
    s_orig_e1_cs = ivt[0xE1 * 2 + 1];
    s_orig_e2_ip = ivt[0xE2 * 2 + 0];
    s_orig_e2_cs = ivt[0xE2 * 2 + 1];
    s_orig_e3_ip = ivt[0xE3 * 2 + 0];
    s_orig_e3_cs = ivt[0xE3 * 2 + 1];

    /* Vector 0xE0: 8080 TPA entry for BRKEM */
    ivt[0xE0 * 2 + 0] = CPM_TPA_BASE; /* IP = 0x0100 */
    ivt[0xE0 * 2 + 1] = cpm_seg;      /* CS = cpm_seg */

    /* Vector 0xE1: 8086 native BDOS handler for CALLN */
    ivt[0xE1 * 2 + 0] = (uint16_t)&v20_bdos_isr;
    ivt[0xE1 * 2 + 1] = get_cs();

    /* Vector 0xE2: 8086 native Exit handler for Warm Boot */
    ivt[0xE2 * 2 + 0] = (uint16_t)&v20_exit_isr;
    ivt[0xE2 * 2 + 1] = get_cs();

    /* Vector 0xE3: 8086 native Direct BIOS handler */
    ivt[0xE3 * 2 + 0] = (uint16_t)&v20_bios_isr;
    ivt[0xE3 * 2 + 1] = get_cs();
}

static void v20_restore_ivt(void) {
    uint16_t __far *ivt = (uint16_t __far *)MK_FP(0, 0);
    ivt[0xE0 * 2 + 0] = s_orig_e0_ip;
    ivt[0xE0 * 2 + 1] = s_orig_e0_cs;
    ivt[0xE1 * 2 + 0] = s_orig_e1_ip;
    ivt[0xE1 * 2 + 1] = s_orig_e1_cs;
    ivt[0xE2 * 2 + 0] = s_orig_e2_ip;
    ivt[0xE2 * 2 + 1] = s_orig_e2_cs;
    ivt[0xE3 * 2 + 0] = s_orig_e3_ip;
    ivt[0xE3 * 2 + 1] = s_orig_e3_cs;
}

/* --------------------------------------------------------------------------
 * Intel 8088 Software 8080 Interpreter Fallback
 * -------------------------------------------------------------------------- */

static uint8_t s_8080_a, s_8080_b, s_8080_c, s_8080_d, s_8080_e, s_8080_h, s_8080_l;
static uint8_t s_8080_fs, s_8080_fz, s_8080_fac, s_8080_fp, s_8080_fcy;
static uint16_t s_8080_sp, s_8080_pc;

static void set_szp(uint8_t val) {
    s_8080_fs = (val & 0x80) != 0;
    s_8080_fz = (val == 0);
    s_8080_fp = s_parity[val];
}

static uint8_t get_reg(uint8_t r, uint16_t hl, uint8_t __far *mem) {
    switch (r) {
    case 0: return s_8080_b;
    case 1: return s_8080_c;
    case 2: return s_8080_d;
    case 3: return s_8080_e;
    case 4: return s_8080_h;
    case 5: return s_8080_l;
    case 6: return mem[hl];
    case 7: return s_8080_a;
    }
    return 0;
}

static void set_reg(uint8_t r, uint8_t val, uint16_t hl, uint8_t __far *mem) {
    switch (r) {
    case 0: s_8080_b = val; break;
    case 1: s_8080_c = val; break;
    case 2: s_8080_d = val; break;
    case 3: s_8080_e = val; break;
    case 4: s_8080_h = val; break;
    case 5: s_8080_l = val; break;
    case 6: mem[hl] = val; break;
    case 7: s_8080_a = val; break;
    }
}

static void do_alu(uint8_t op, uint8_t val) {
    uint16_t res;
    switch (op) {
    case 0: /* ADD */
        res = (uint16_t)s_8080_a + val;
        s_8080_fac = ((s_8080_a & 0x0F) + (val & 0x0F)) > 0x0F;
        s_8080_fcy = (res > 0xFF);
        s_8080_a = (uint8_t)res;
        set_szp(s_8080_a);
        break;
    case 1: /* ADC */
        res = (uint16_t)s_8080_a + val + s_8080_fcy;
        s_8080_fac = ((s_8080_a & 0x0F) + (val & 0x0F) + s_8080_fcy) > 0x0F;
        s_8080_fcy = (res > 0xFF);
        s_8080_a = (uint8_t)res;
        set_szp(s_8080_a);
        break;
    case 2: /* SUB */
        res = (uint16_t)s_8080_a - val;
        s_8080_fac = ((s_8080_a & 0x0F) < (val & 0x0F));
        s_8080_fcy = (s_8080_a < val);
        s_8080_a = (uint8_t)res;
        set_szp(s_8080_a);
        break;
    case 3: /* SBB */
        res = (uint16_t)s_8080_a - val - s_8080_fcy;
        s_8080_fac = ((s_8080_a & 0x0F) < ((val & 0x0F) + s_8080_fcy));
        s_8080_fcy = ((uint16_t)s_8080_a < ((uint16_t)val + s_8080_fcy));
        s_8080_a = (uint8_t)res;
        set_szp(s_8080_a);
        break;
    case 4: /* ANA */
        s_8080_a &= val;
        s_8080_fcy = 0;
        s_8080_fac = 1; /* 8080 sets AC on ANA */
        set_szp(s_8080_a);
        break;
    case 5: /* XRA */
        s_8080_a ^= val;
        s_8080_fcy = 0;
        s_8080_fac = 0;
        set_szp(s_8080_a);
        break;
    case 6: /* ORA */
        s_8080_a |= val;
        s_8080_fcy = 0;
        s_8080_fac = 0;
        set_szp(s_8080_a);
        break;
    case 7: /* CMP */
        res = (uint16_t)s_8080_a - val;
        s_8080_fac = ((s_8080_a & 0x0F) < (val & 0x0F));
        s_8080_fcy = (s_8080_a < val);
        set_szp((uint8_t)res);
        break;
    }
}

static int eval_cond(uint8_t cond) {
    switch (cond) {
    case 0: return !s_8080_fz;  /* NZ */
    case 1: return s_8080_fz;   /* Z */
    case 2: return !s_8080_fcy; /* NC */
    case 3: return s_8080_fcy;  /* C */
    case 4: return !s_8080_fp;  /* PO */
    case 5: return s_8080_fp;   /* PE */
    case 6: return !s_8080_fs;  /* P */
    case 7: return s_8080_fs;   /* M */
    }
    return 0;
}

static void daa(void) {
    uint8_t add = 0;
    uint8_t carry = s_8080_fcy;
    if ((s_8080_a & 0x0F) > 9 || s_8080_fac) {
        add |= 0x06;
    }
    if (s_8080_a > 0x99 || s_8080_fcy) {
        add |= 0x60;
        carry = 1;
    }
    s_8080_fac = ((s_8080_a & 0x0F) + (add & 0x0F)) > 0x0F;
    s_8080_a += add;
    s_8080_fcy = carry;
    set_szp(s_8080_a);
}

static void run_software_8080(void) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_cpm_seg, 0);

    s_8080_pc = CPM_TPA_BASE;
    s_8080_sp = CPM_STACK_TOP;
    s_8080_a = s_8080_b = s_8080_c = s_8080_d = s_8080_e = s_8080_h = s_8080_l = 0;
    s_8080_fs = s_8080_fz = s_8080_fac = s_8080_fp = s_8080_fcy = 0;

    s_cpm_running = 1;
    while (s_cpm_running) {
        uint8_t op;

        /* Check for BDOS entry trap */
        if (s_8080_pc == CPM_BDOS_ENTRY || s_8080_pc == 0x0005 || s_8080_pc == 0xFD00) {
            uint16_t de = ((uint16_t)s_8080_d << 8) | s_8080_e;
            uint16_t hl = ((uint16_t)s_8080_h << 8) | s_8080_l;
            uint16_t res = cpm_bdos_dispatch(s_8080_c, de, hl, s_8080_a);
            s_8080_a = (uint8_t)(res & 0xFF);
            if (s_8080_c == 27 || s_8080_c == 31) {
                s_8080_l = (uint8_t)(res & 0xFF);
                s_8080_h = (uint8_t)(res >> 8);
            }

            /* Simulate RET from BDOS */
            s_8080_pc = mem[s_8080_sp] | ((uint16_t)mem[s_8080_sp + 1] << 8);
            s_8080_sp += 2;
            continue;
        } else if (s_8080_pc == CPM_WARM_BOOT || s_8080_pc == 0xFE00 || s_8080_pc == 0xFE03 || s_8080_pc == 0xFE40) {
            /* Warm Boot / Exit */
            s_cpm_running = 0;
            break;
        } else if (s_8080_pc == 0xFE44) {
            /* BIOS CONST: Console Status */
            s_8080_a = os_kbhit() ? 0xFF : 0x00;
            s_8080_pc = mem[s_8080_sp] | ((uint16_t)mem[s_8080_sp + 1] << 8);
            s_8080_sp += 2;
            continue;
        } else if (s_8080_pc == 0xFE4A) {
            /* BIOS CONIN: Console Input */
            uint16_t key = os_getc();
            s_8080_a = (uint8_t)(key & 0xFF);
            s_8080_pc = mem[s_8080_sp] | ((uint16_t)mem[s_8080_sp + 1] << 8);
            s_8080_sp += 2;
            continue;
        } else if (s_8080_pc == 0xFE50) {
            /* BIOS CONOUT: Console Output */
            cpm_term_putc((char)s_8080_c);
            s_8080_pc = mem[s_8080_sp] | ((uint16_t)mem[s_8080_sp + 1] << 8);
            s_8080_sp += 2;
            continue;
        } else if (s_8080_pc == 0xFE56) {
            /* BIOS LIST / Dummy */
            s_8080_a = 0;
            s_8080_pc = mem[s_8080_sp] | ((uint16_t)mem[s_8080_sp + 1] << 8);
            s_8080_sp += 2;
            continue;
        }

        op = mem[s_8080_pc++];

        /* MOV r1, r2 */
        if (op >= 0x40 && op <= 0x7F) {
            if (op == 0x76) { /* HLT */ break; }
            {
                uint8_t dst = (op >> 3) & 7;
                uint8_t src = op & 7;
                uint16_t hl = ((uint16_t)s_8080_h << 8) | s_8080_l;
                set_reg(dst, get_reg(src, hl, mem), hl, mem);
            }
            continue;
        }

        /* ALU r */
        if (op >= 0x80 && op <= 0xBF) {
            uint8_t alu_op = (op >> 3) & 7;
            uint8_t src = op & 7;
            uint16_t hl = ((uint16_t)s_8080_h << 8) | s_8080_l;
            do_alu(alu_op, get_reg(src, hl, mem));
            continue;
        }

        /* Immediate ALU */
        if ((op & 0xC7) == 0xC6) {
            uint8_t alu_op = (op >> 3) & 7;
            uint8_t val = mem[s_8080_pc++];
            do_alu(alu_op, val);
            continue;
        }

        /* MVI r, d8 */
        if ((op & 0xC7) == 0x06) {
            uint8_t r = (op >> 3) & 7;
            uint8_t val = mem[s_8080_pc++];
            uint16_t hl = ((uint16_t)s_8080_h << 8) | s_8080_l;
            set_reg(r, val, hl, mem);
            continue;
        }

        /* INR r */
        if ((op & 0xC7) == 0x04) {
            uint8_t r = (op >> 3) & 7;
            uint16_t hl = ((uint16_t)s_8080_h << 8) | s_8080_l;
            uint8_t val = get_reg(r, hl, mem);
            s_8080_fac = ((val & 0x0F) + 1) > 0x0F;
            val++;
            set_szp(val);
            set_reg(r, val, hl, mem);
            continue;
        }

        /* DCR r */
        if ((op & 0xC7) == 0x05) {
            uint8_t r = (op >> 3) & 7;
            uint16_t hl = ((uint16_t)s_8080_h << 8) | s_8080_l;
            uint8_t val = get_reg(r, hl, mem);
            s_8080_fac = (val & 0x0F) == 0;
            val--;
            set_szp(val);
            set_reg(r, val, hl, mem);
            continue;
        }

        /* J<cond> */
        if ((op & 0xC7) == 0xC2) {
            uint8_t cond = (op >> 3) & 7;
            uint16_t target = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
            s_8080_pc += 2;
            if (eval_cond(cond)) s_8080_pc = target;
            continue;
        }

        /* C<cond> */
        if ((op & 0xC7) == 0xC4) {
            uint8_t cond = (op >> 3) & 7;
            uint16_t target = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
            s_8080_pc += 2;
            if (eval_cond(cond)) {
                s_8080_sp -= 2;
                mem[s_8080_sp] = (uint8_t)(s_8080_pc & 0xFF);
                mem[s_8080_sp + 1] = (uint8_t)(s_8080_pc >> 8);
                s_8080_pc = target;
            }
            continue;
        }

        /* R<cond> */
        if ((op & 0xC7) == 0xC0) {
            uint8_t cond = (op >> 3) & 7;
            if (eval_cond(cond)) {
                s_8080_pc = mem[s_8080_sp] | ((uint16_t)mem[s_8080_sp + 1] << 8);
                s_8080_sp += 2;
            }
            continue;
        }

        /* Specific instructions */
        switch (op) {
        case 0x00: /* NOP */ break;
        case 0xC3: /* JMP a16 */
            s_8080_pc = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
            break;
        case 0xCD: /* CALL a16 */
            {
                uint16_t target = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
                s_8080_pc += 2;
                s_8080_sp -= 2;
                mem[s_8080_sp] = (uint8_t)(s_8080_pc & 0xFF);
                mem[s_8080_sp + 1] = (uint8_t)(s_8080_pc >> 8);
                s_8080_pc = target;
            }
            break;
        case 0xC9: /* RET */
            s_8080_pc = mem[s_8080_sp] | ((uint16_t)mem[s_8080_sp + 1] << 8);
            s_8080_sp += 2;
            break;

        case 0x01: /* LXI B, d16 */
            s_8080_c = mem[s_8080_pc++];
            s_8080_b = mem[s_8080_pc++];
            break;
        case 0x11: /* LXI D, d16 */
            s_8080_e = mem[s_8080_pc++];
            s_8080_d = mem[s_8080_pc++];
            break;
        case 0x21: /* LXI H, d16 */
            s_8080_l = mem[s_8080_pc++];
            s_8080_h = mem[s_8080_pc++];
            break;
        case 0x31: /* LXI SP, d16 */
            s_8080_sp = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
            s_8080_pc += 2;
            break;

        case 0x02: /* STAX B */ mem[((uint16_t)s_8080_b << 8) | s_8080_c] = s_8080_a; break;
        case 0x12: /* STAX D */ mem[((uint16_t)s_8080_d << 8) | s_8080_e] = s_8080_a; break;
        case 0x0A: /* LDAX B */ s_8080_a = mem[((uint16_t)s_8080_b << 8) | s_8080_c]; break;
        case 0x1A: /* LDAX D */ s_8080_a = mem[((uint16_t)s_8080_d << 8) | s_8080_e]; break;

        case 0x32: /* STA a16 */
            {
                uint16_t addr = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
                s_8080_pc += 2;
                mem[addr] = s_8080_a;
            }
            break;
        case 0x3A: /* LDA a16 */
            {
                uint16_t addr = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
                s_8080_pc += 2;
                s_8080_a = mem[addr];
            }
            break;

        case 0x22: /* SHLD a16 */
            {
                uint16_t addr = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
                s_8080_pc += 2;
                mem[addr] = s_8080_l;
                mem[addr + 1] = s_8080_h;
            }
            break;
        case 0x2A: /* LHLD a16 */
            {
                uint16_t addr = mem[s_8080_pc] | ((uint16_t)mem[s_8080_pc + 1] << 8);
                s_8080_pc += 2;
                s_8080_l = mem[addr];
                s_8080_h = mem[addr + 1];
            }
            break;

        case 0x03: /* INX B */ s_8080_c++; if (s_8080_c == 0) s_8080_b++; break;
        case 0x13: /* INX D */ s_8080_e++; if (s_8080_e == 0) s_8080_d++; break;
        case 0x23: /* INX H */ s_8080_l++; if (s_8080_l == 0) s_8080_h++; break;
        case 0x33: /* INX SP */ s_8080_sp++; break;

        case 0x0B: /* DCX B */ if (s_8080_c == 0) s_8080_b--; s_8080_c--; break;
        case 0x1B: /* DCX D */ if (s_8080_e == 0) s_8080_d--; s_8080_e--; break;
        case 0x2B: /* DCX H */ if (s_8080_l == 0) s_8080_h--; s_8080_l--; break;
        case 0x3B: /* DCX SP */ s_8080_sp--; break;

        case 0x09: /* DAD B */
            {
                uint32_t hl = ((uint32_t)s_8080_h << 8) | s_8080_l;
                uint32_t bc = ((uint32_t)s_8080_b << 8) | s_8080_c;
                uint32_t res = hl + bc;
                s_8080_fcy = (res > 0xFFFF);
                s_8080_h = (uint8_t)((res >> 8) & 0xFF);
                s_8080_l = (uint8_t)(res & 0xFF);
            }
            break;
        case 0x19: /* DAD D */
            {
                uint32_t hl = ((uint32_t)s_8080_h << 8) | s_8080_l;
                uint32_t de = ((uint32_t)s_8080_d << 8) | s_8080_e;
                uint32_t res = hl + de;
                s_8080_fcy = (res > 0xFFFF);
                s_8080_h = (uint8_t)((res >> 8) & 0xFF);
                s_8080_l = (uint8_t)(res & 0xFF);
            }
            break;
        case 0x29: /* DAD H */
            {
                uint32_t hl = ((uint32_t)s_8080_h << 8) | s_8080_l;
                uint32_t res = hl + hl;
                s_8080_fcy = (res > 0xFFFF);
                s_8080_h = (uint8_t)((res >> 8) & 0xFF);
                s_8080_l = (uint8_t)(res & 0xFF);
            }
            break;
        case 0x39: /* DAD SP */
            {
                uint32_t hl = ((uint32_t)s_8080_h << 8) | s_8080_l;
                uint32_t res = hl + s_8080_sp;
                s_8080_fcy = (res > 0xFFFF);
                s_8080_h = (uint8_t)((res >> 8) & 0xFF);
                s_8080_l = (uint8_t)(res & 0xFF);
            }
            break;

        case 0xEB: /* XCHG */
            {
                uint8_t th = s_8080_h, tl = s_8080_l;
                s_8080_h = s_8080_d; s_8080_l = s_8080_e;
                s_8080_d = th; s_8080_e = tl;
            }
            break;
        case 0xE3: /* XTHL */
            {
                uint8_t tl = mem[s_8080_sp];
                uint8_t th = mem[s_8080_sp + 1];
                mem[s_8080_sp] = s_8080_l;
                mem[s_8080_sp + 1] = s_8080_h;
                s_8080_l = tl; s_8080_h = th;
            }
            break;
        case 0xE9: /* PCHL */
            s_8080_pc = ((uint16_t)s_8080_h << 8) | s_8080_l;
            break;
        case 0xF9: /* SPHL */
            s_8080_sp = ((uint16_t)s_8080_h << 8) | s_8080_l;
            break;

        case 0xC5: /* PUSH B */
            s_8080_sp -= 2; mem[s_8080_sp] = s_8080_c; mem[s_8080_sp + 1] = s_8080_b;
            break;
        case 0xD5: /* PUSH D */
            s_8080_sp -= 2; mem[s_8080_sp] = s_8080_e; mem[s_8080_sp + 1] = s_8080_d;
            break;
        case 0xE5: /* PUSH H */
            s_8080_sp -= 2; mem[s_8080_sp] = s_8080_l; mem[s_8080_sp + 1] = s_8080_h;
            break;
        case 0xF5: /* PUSH PSW */
            {
                uint8_t psw = (s_8080_fs ? 0x80 : 0) | (s_8080_fz ? 0x40 : 0) |
                              (s_8080_fac ? 0x10 : 0) | (s_8080_fp ? 0x04 : 0) |
                              0x02 | (s_8080_fcy ? 0x01 : 0);
                s_8080_sp -= 2; mem[s_8080_sp] = psw; mem[s_8080_sp + 1] = s_8080_a;
            }
            break;

        case 0xC1: /* POP B */
            s_8080_c = mem[s_8080_sp++]; s_8080_b = mem[s_8080_sp++];
            break;
        case 0xD1: /* POP D */
            s_8080_e = mem[s_8080_sp++]; s_8080_d = mem[s_8080_sp++];
            break;
        case 0xE1: /* POP H */
            s_8080_l = mem[s_8080_sp++]; s_8080_h = mem[s_8080_sp++];
            break;
        case 0xF1: /* POP PSW */
            {
                uint8_t psw = mem[s_8080_sp++];
                s_8080_a = mem[s_8080_sp++];
                s_8080_fs = (psw & 0x80) != 0;
                s_8080_fz = (psw & 0x40) != 0;
                s_8080_fac = (psw & 0x10) != 0;
                s_8080_fp = (psw & 0x04) != 0;
                s_8080_fcy = (psw & 0x01) != 0;
            }
            break;

        case 0x07: /* RLC */
            s_8080_fcy = (s_8080_a >> 7);
            s_8080_a = (uint8_t)((s_8080_a << 1) | s_8080_fcy);
            break;
        case 0x0F: /* RRC */
            s_8080_fcy = (s_8080_a & 1);
            s_8080_a = (uint8_t)((s_8080_a >> 1) | (s_8080_fcy << 7));
            break;
        case 0x17: /* RAL */
            {
                uint8_t old_cy = s_8080_fcy;
                s_8080_fcy = (s_8080_a >> 7);
                s_8080_a = (uint8_t)((s_8080_a << 1) | old_cy);
            }
            break;
        case 0x1F: /* RAR */
            {
                uint8_t old_cy = s_8080_fcy;
                s_8080_fcy = (s_8080_a & 1);
                s_8080_a = (uint8_t)((s_8080_a >> 1) | (old_cy << 7));
            }
            break;

        case 0x27: daa(); break;
        case 0x2F: s_8080_a = ~s_8080_a; break; /* CMA */
        case 0x37: s_8080_fcy = 1; break;       /* STC */
        case 0x3F: s_8080_fcy = !s_8080_fcy; break; /* CMC */

        case 0xDB: /* IN port */ s_8080_pc++; s_8080_a = 0xFF; break;
        case 0xD3: /* OUT port */ s_8080_pc++; break;
        case 0xFB: /* EI */ break;
        case 0xF3: /* DI */ break;

        default:
            /* Undefined opcode */
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * Program Loader & Setup
 * -------------------------------------------------------------------------- */

static void cpm_setup_zeropage(uint16_t cpm_seg, const char *tail) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(cpm_seg, 0);
    int i;

    /* Clear bottom 256 bytes */
    for (i = 0; i < 256; i++) {
        mem[i] = 0;
    }

    /* 0x0000: Warm boot vector -> JMP 0xFE03 (WBOOT in BIOS Jump Table) */
    mem[0] = 0xC3; /* JMP */
    mem[1] = 0x03;
    mem[2] = 0xFE;

    /* 0x0003: IOBYTE */
    mem[3] = 0x00;

    /* 0x0004: Default drive (0 = A:) */
    mem[4] = 0x00;

    /* 0x0005: BDOS entry point -> JMP 0xFD00 */
    mem[5] = 0xC3; /* JMP */
    mem[6] = 0x00;
    mem[7] = 0xFD;

    /* 0xFD00: BDOS entry trampoline (CALLN 0xE1 / RET) */
    mem[0xFD00] = 0xED;
    mem[0xFD01] = 0xED;
    mem[0xFD02] = 0xE1; /* CALLN 0xE1 */
    mem[0xFD03] = 0xC9; /* RET */

    /* 0xFE00..0xFE33: CP/M 2.2 Standard BIOS Jump Table (17 entries) */
    /* 0: BOOT (Cold Boot) -> JMP 0xFE40 */
    mem[0xFE00] = 0xC3; mem[0xFE01] = 0x40; mem[0xFE02] = 0xFE;
    /* 1: WBOOT (Warm Boot) -> JMP 0xFE40 */
    mem[0xFE03] = 0xC3; mem[0xFE04] = 0x40; mem[0xFE05] = 0xFE;
    /* 2: CONST (Console Status) -> JMP 0xFE44 */
    mem[0xFE06] = 0xC3; mem[0xFE07] = 0x44; mem[0xFE08] = 0xFE;
    /* 3: CONIN (Console Input) -> JMP 0xFE4A */
    mem[0xFE09] = 0xC3; mem[0xFE0A] = 0x4A; mem[0xFE0B] = 0xFE;
    /* 4: CONOUT (Console Output) -> JMP 0xFE50 */
    mem[0xFE0C] = 0xC3; mem[0xFE0D] = 0x50; mem[0xFE0E] = 0xFE;
    /* 5: LIST -> JMP 0xFE56 */
    mem[0xFE0F] = 0xC3; mem[0xFE10] = 0x56; mem[0xFE11] = 0xFE;
    for (i = 6; i <= 16; i++) {
        uint16_t ent = 0xFE00 + (i * 3);
        mem[ent] = 0xC3;
        mem[ent + 1] = 0x56;
        mem[ent + 2] = 0xFE;
    }

    /* BIOS Handlers at 0xFE40..0xFE59 */
    /* 0xFE40: WBOOT / Exit -> CALLN 0xE2; RET */
    mem[0xFE40] = 0xED; mem[0xFE41] = 0xED; mem[0xFE42] = 0xE2; mem[0xFE43] = 0xC9;

    /* 0xFE44: CONST -> MVI A, 2; CALLN 0xE3; RET */
    mem[0xFE44] = 0x3E; mem[0xFE45] = 0x02;
    mem[0xFE46] = 0xED; mem[0xFE47] = 0xED; mem[0xFE48] = 0xE3;
    mem[0xFE49] = 0xC9;

    /* 0xFE4A: CONIN -> MVI A, 3; CALLN 0xE3; RET */
    mem[0xFE4A] = 0x3E; mem[0xFE4B] = 0x03;
    mem[0xFE4C] = 0xED; mem[0xFE4D] = 0xED; mem[0xFE4E] = 0xE3;
    mem[0xFE4F] = 0xC9;

    /* 0xFE50: CONOUT -> MVI A, 4; CALLN 0xE3; RET */
    mem[0xFE50] = 0x3E; mem[0xFE51] = 0x04;
    mem[0xFE52] = 0xED; mem[0xFE53] = 0xED; mem[0xFE54] = 0xE3;
    mem[0xFE55] = 0xC9;

    /* 0xFE56: Dummy / List -> XRA A; RET */
    mem[0xFE56] = 0xAF; mem[0xFE57] = 0xC9;

    /* 0xFE90: CP/M Allocation Vector (32 bytes = 256 blocks) */
    {
        uint8_t __far *alv = (uint8_t __far *)MK_FP(cpm_seg, 0xFE90);
        alv[0] = 0xC0;
        for (i = 1; i < 32; i++) {
            alv[i] = 0x00;
        }
    }

    /* 0x005C & 0x006C: Parse default FCBs from tail */
    {
        char arg1[16];
        char arg2[16];
        int t = 0, p = 0;
        arg1[0] = '\0';
        arg2[0] = '\0';

        if (tail) {
            while (tail[t] == ' ') t++;
            while (tail[t] && tail[t] != ' ' && p < 15) arg1[p++] = tail[t++];
            arg1[p] = '\0';

            while (tail[t] == ' ') t++;
            p = 0;
            while (tail[t] && tail[t] != ' ' && p < 15) arg2[p++] = tail[t++];
            arg2[p] = '\0';
        }

        dos_name_to_fcb(arg1, (uint8_t __far *)MK_FP(cpm_seg, CPM_DEFAULT_FCB1));
        dos_name_to_fcb(arg2, (uint8_t __far *)MK_FP(cpm_seg, CPM_DEFAULT_FCB2));
    }

    /* 0x0080: Command tail in default DMA buffer */
    {
        int len = 0;
        int t = 0;
        if (tail) {
            while (tail[t] == ' ') t++;
            if (tail[t]) {
                mem[0x81] = ' ';
                len = 1;
                while (tail[t] && len < 126) {
                    mem[0x81 + len] = (uint8_t)cpm_toupper(tail[t]);
                    len++;
                    t++;
                }
            }
        }
        mem[0x80] = (uint8_t)len;
        mem[0x81 + len] = 0x00;
    }

    /* 0xFE80: CP/M Disk Parameter Block (DPB) */
    {
        uint8_t __far *dpb = (uint8_t __far *)MK_FP(cpm_seg, 0xFE80);
        dpb[0] = 26; dpb[1] = 0; /* SPT = 26 */
        dpb[2] = 3;              /* BSH = 3 (1K) */
        dpb[3] = 7;              /* BLM = 7 */
        dpb[4] = 0;              /* EXM = 0 */
        dpb[5] = 242; dpb[6] = 0;/* DSM = 242 */
        dpb[7] = 63; dpb[8] = 0; /* DRM = 63 */
        dpb[9] = 0xC0; dpb[10] = 0x00; /* AL0, AL1 */
        dpb[11] = 16; dpb[12] = 0;     /* CKS = 16 */
        dpb[13] = 2; dpb[14] = 0;      /* OFF = 2 */
    }
}

/* --------------------------------------------------------------------------
 * Main Entry Point
 * -------------------------------------------------------------------------- */

int app_main(void) {
    const char *args = os_get_args();
    char prog_name[64];
    const char *tail = 0;
    int i = 0, j = 0;
    long fsize;

    cpm_init_parity();
    s_cpu_type = (uint8_t)os_cpu_type();

    /* Parse command line */
    while (args && args[i] == ' ') i++;
    if (!args || args[i] == '\0') {
        os_puts("hobbyOS CP/M-80 Subsystem v1.0\n");
        if (s_cpu_type == CPU_TYPE_V20) {
            os_puts("Engine: NEC V20 Native 8080 (Hardware Mode)\n");
        } else {
            os_puts("Engine: Intel 8088 Emulation (Software Interpreter Fallback)\n");
        }
        os_puts("Usage: cpm <program.com> [arguments]\n");
        return 1;
    }

    while (args[i] && args[i] != ' ' && j < sizeof(prog_name) - 1) {
        prog_name[j++] = args[i++];
    }
    prog_name[j] = '\0';
    while (args[i] == ' ') i++;
    if (args[i]) tail = &args[i];

    /* Check file */
    fsize = os_file_size(prog_name);
    if (fsize < 0) {
        os_puts("CPM: Cannot open file: ");
        os_puts(prog_name);
        os_puts("\n");
        return 1;
    }
    if (fsize > (65536L - CPM_TPA_BASE - 1024L)) {
        os_puts("CPM: Program too large for 64K CP/M TPA\n");
        return 1;
    }

    /* Allocate 64 KB segment (4096 paragraphs) */
    s_cpm_seg = os_mem_alloc(4096);
    if (s_cpm_seg == 0) {
        os_puts("CPM: Out of conventional memory (64KB required)\n");
        return 1;
    }

    /* Setup Zero page and command tail */
    cpm_setup_zeropage(s_cpm_seg, tail);

    /* Load program into TPA at offset 0x0100 */
    {
        uint8_t __far *tpa = (uint8_t __far *)MK_FP(s_cpm_seg, CPM_TPA_BASE);
        if (os_file_read_offset(prog_name, tpa, (uint16_t)fsize, 0) != (uint16_t)fsize) {
            os_puts("CPM: Error reading program image\n");
            os_mem_free(s_cpm_seg);
            return 1;
        }
    }

    os_set_break_mode(BREAK_MODE_RAW);
    cpm_vram_setup();
    s_cpm_running = 1;

    /* Execute via NEC V20 hardware emulation or 8088 software fallback */
    if (s_cpu_type == CPU_TYPE_V20) {
        v20_setup_ivt(s_cpm_seg);
        v20_enter_8080(s_cpm_seg);
        v20_restore_ivt();
    } else {
        run_software_8080();
    }

    os_set_break_mode(BREAK_MODE_NORMAL);
    os_console_set_shadow(s_term_shadow, (uint8_t)s_term_row, (uint8_t)s_term_col);
    os_set_cursor_visible(1);
    os_mem_free(s_cpm_seg);
    return 0;
}
