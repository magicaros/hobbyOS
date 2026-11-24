/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * FM.BIN - Dual-Panel File Manager for hobbyOS (Norton Commander inspired).
 * Pure user-space application with zero kernel modifications required.
 */

#include "hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

#define MAX_PANEL_FILES  128
#define PATH_MAX_LEN     80
#define VISIBLE_ROWS     19

#define LEFT_COL_START   0
#define LEFT_COL_END     39
#define RIGHT_COL_START  40
#define RIGHT_COL_END    79

#define PANEL_ROW_TOP    1
#define PANEL_ROW_BOT    21

/* PC/XT Scan Codes */
#define SC_HOME   0x47
#define SC_UP     0x48
#define SC_PGUP   0x49
#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_END    0x4F
#define SC_DOWN   0x50
#define SC_PGDN   0x51
#define SC_INS    0x52
#define SC_DEL    0x53

#define SC_F1     0x3B
#define SC_F2     0x3C
#define SC_F3     0x3D
#define SC_F4     0x3E
#define SC_F5     0x3F
#define SC_F6     0x40
#define SC_F7     0x41
#define SC_F8     0x42
#define SC_F9     0x43
#define SC_F10    0x44

#define SC_ALT_F1 0x68
#define SC_ALT_F2 0x69

/* CP437 Box Drawing Characters */
#define CH_D_TL   (char)0xC9  /* ╔ */
#define CH_D_HOR  (char)0xCD  /* ═ */
#define CH_D_TR   (char)0xBB  /* ╗ */
#define CH_D_VER  (char)0xBA  /* ║ */
#define CH_D_BL   (char)0xC8  /* ╚ */
#define CH_D_BR   (char)0xBC  /* ╝ */
#define CH_D_TJ   (char)0xCB  /* ╦ */
#define CH_D_BJ   (char)0xCA  /* ╩ */

#define CH_S_TL   (char)0xDA  /* ┌ */
#define CH_S_HOR  (char)0xC4  /* ─ */
#define CH_S_TR   (char)0xBF  /* ┐ */
#define CH_S_VER  (char)0xB3  /* │ */
#define CH_S_BL   (char)0xC0  /* └ */
#define CH_S_BR   (char)0xD9  /* ┘ */

typedef struct {
    char     name[13];
    uint32_t size;
    uint8_t  attr;
    uint8_t  tagged;
} file_item_t;

typedef struct {
    char        path[PATH_MAX_LEN];
    file_item_t files[MAX_PANEL_FILES];
    int         file_count;
    int         cursor;
    int         top_index;
    int         sort_mode; /* 0 = Name, 1 = Size, 2 = Ext */
} panel_t;

typedef struct {
    char name[8];
} vol_item_t;

/* Global state */
static uint16_t   s_vram_seg = 0xB800;
static uint16_t   s_video_type = VIDEO_TYPE_CGA;
static uint16_t   s_screen[80 * 25];
static uint16_t   s_cache[80 * 25];

static panel_t    s_left;
static panel_t    s_right;
static int        s_active = 0; /* 0 = left, 1 = right */
static int        s_running = 1;
static int        s_mouse_avail = 0;
static int        s_mouse_row = -1;
static int        s_mouse_col = -1;
static int        s_needs_redraw = 1;

static uint16_t   s_last_click_tick = 0;
static int        s_last_click_idx = -1;
static int        s_last_click_panel = -1;

static vol_item_t s_volumes[16];
static int        s_vol_count = 0;

/* Live Command Prompt State (Row 23) */
static char       s_cmd_buf[80];
static int        s_cmd_len = 0;
static int        s_cmd_pos = 0;

/* F9 Pull-Down Menu State */
static int        s_menu_active = 0;
static int        s_menu_cat = 0;  /* 0 = Left, 1 = Files, 2 = Disk, 3 = Right */
static int        s_menu_item = 0;

static const struct {
    const char *title;
    int col_start;
    int col_end;
} s_menu_headers[4] = {
    { " Left ",  1,  6 },
    { " Files ", 8, 14 },
    { " Disk ", 16, 21 },
    { " Right ", 23, 29 }
};

/* Color scheme attributes */
static uint8_t ATTR_PANEL_FRAME;
static uint8_t ATTR_PANEL_BG;
static uint8_t ATTR_PANEL_DIR;
static uint8_t ATTR_PANEL_EXE;
static uint8_t ATTR_PANEL_SEL;
static uint8_t ATTR_PANEL_TITLE;
static uint8_t ATTR_STATUS_BAR;
static uint8_t ATTR_FN_NUM;
static uint8_t ATTR_FN_LBL;
static uint8_t ATTR_DLG_FRAME;
static uint8_t ATTR_DLG_BG;
static uint8_t ATTR_DLG_SEL;
static uint8_t ATTR_DLG_BTN;
static uint8_t ATTR_DLG_BTN_SEL;

/* --------------------------------------------------------------------------
 * Freestanding String and Math Utilities (Pure 16-bit)
 * -------------------------------------------------------------------------- */

static int str_len(const char *s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

static void str_copy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void str_copy_n(char *dst, const char *src, int n) {
    int i = 0;
    while (*src && i < n - 1) {
        dst[i++] = *src++;
    }
    dst[i] = '\0';
}

static void str_cat(char *dst, const char *src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static int str_icmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = to_upper(*a);
        char cb = to_upper(*b);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++;
        b++;
    }
    return (int)(unsigned char)to_upper(*a) - (int)(unsigned char)to_upper(*b);
}

static int str_nicmp(const char *a, const char *b, int n) {
    int i = 0;
    while (i < n && a[i] && b[i]) {
        char ca = to_upper(a[i]);
        char cb = to_upper(b[i]);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        i++;
    }
    if (i == n) return 0;
    return (int)(unsigned char)to_upper(a[i]) - (int)(unsigned char)to_upper(b[i]);
}

static int str_ends_with_ic(const char *s, const char *ext) {
    int sl = str_len(s);
    int el = str_len(ext);
    if (sl < el) return 0;
    return (str_icmp(s + (sl - el), ext) == 0);
}

static const char *get_ext(const char *name) {
    int i = str_len(name) - 1;
    while (i >= 0 && name[i] != '.') i--;
    if (i >= 0) return name + i + 1;
    return "";
}

static int fm_is_forbidden_bin(const char *name) {
    if (str_ends_with_ic(name, "KERNEL.BIN") || str_ends_with_ic(name, "BOOT.BIN") ||
        str_ends_with_ic(name, "KERNEL") || str_ends_with_ic(name, "BOOT")) {
        return 1;
    }
    return 0;
}

static int is_binary_name(const char *name) {
    return ((str_ends_with_ic(name, ".BIN") || str_ends_with_ic(name, ".COM") ||
             str_ends_with_ic(name, ".EXE") || str_ends_with_ic(name, ".SH")) &&
            !fm_is_forbidden_bin(name));
}

static int is_cpm_binary(const char *path) {
    uint8_t buf[256];
    uint16_t n;
    uint32_t off = 0;
    int i;
    int clen = str_len(path);
    if (clen < 4) return 0;
    if (!str_ends_with_ic(path, ".COM")) return 0;

    n = os_file_read_offset(path, (void __far *)FP_HERE(buf), sizeof(buf), 0);
    if (n < 4) return 0;

    /* 8080 CP/M common entry opcodes: JMP, LXI SP, LXI H, LXI B, LXI D, LHLD */
    if (buf[0] == 0xC3 || buf[0] == 0x31 || buf[0] == 0x21 ||
        buf[0] == 0x01 || buf[0] == 0x11 || buf[0] == 0x2A) {
        return 1;
    }

    /* Scan up to 32KB for CALL 0005h (CD 05 00) or JMP 0005h (C3 05 00) */
    while (off < 32768UL) {
        if (off > 0) {
            n = os_file_read_offset(path, (void __far *)FP_HERE(buf), sizeof(buf), off);
            if (n < 3) break;
        }
        for (i = 0; i < (int)n - 2; i++) {
            if ((buf[i] == 0xCD || buf[i] == 0xC3) && buf[i+1] == 0x05 && buf[i+2] == 0x00) {
                return 1;
            }
        }
        if (n < sizeof(buf)) break;
        off += (sizeof(buf) - 2);
    }
    return 0;
}

static int wildcard_match(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            while (*str) {
                if (wildcard_match(pat, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pat == '?') {
            if (!*str) return 0;
            pat++;
            str++;
        } else {
            if (to_upper(*pat) != to_upper(*str)) return 0;
            pat++;
            str++;
        }
    }
    return (*str == '\0');
}

static int fm_matches_mask(const char *pat, const char *name) {
    if (pat[0] == '\0' || str_icmp(pat, "*") == 0 || str_icmp(pat, "*.*") == 0) return 1;
    return wildcard_match(pat, name);
}

static uint32_t mul_u16(uint16_t a, uint16_t b);
#pragma aux mul_u16 = \
    "mul dx" \
    parm [ax] [dx] \
    value [dx ax];

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

static void format_size(uint32_t sz, char *out, int max_len) {
    char buf[12];
    int i = 0;
    int j;
    if (sz == 0) {
        buf[i++] = '0';
    } else {
        uint32_t v = sz;
        while (v > 0 && i < 11) {
            buf[i++] = (char)('0' + u32_div10(&v));
        }
    }
    buf[i] = '\0';
    /* Reverse into out */
    j = 0;
    while (i > 0 && j < max_len - 1) {
        out[j++] = buf[--i];
    }
    out[j] = '\0';
}

/* --------------------------------------------------------------------------
 * Screen Buffer & Video Rendering
 * -------------------------------------------------------------------------- */

static void fm_init_colors(void) {
    s_video_type = os_video_type();
    s_vram_seg = (s_video_type == VIDEO_TYPE_MDA) ? 0xB000 : 0xB800;

    if (s_video_type == VIDEO_TYPE_MDA) {
        ATTR_PANEL_FRAME = 0x07;
        ATTR_PANEL_BG    = 0x07;
        ATTR_PANEL_DIR   = 0x0F;
        ATTR_PANEL_EXE   = 0x0F;
        ATTR_PANEL_SEL   = 0x70;
        ATTR_PANEL_TITLE = 0x70;
        ATTR_STATUS_BAR  = 0x07;
        ATTR_FN_NUM      = 0x0F;
        ATTR_FN_LBL      = 0x70;
        ATTR_DLG_FRAME   = 0x70;
        ATTR_DLG_BG      = 0x70;
        ATTR_DLG_SEL     = 0x0F; /* Bright White on Black (inverse of 0x70 DLG_BG) */
        ATTR_DLG_BTN     = 0x07;
        ATTR_DLG_BTN_SEL = 0x0F;
    } else {
        ATTR_PANEL_FRAME = 0x1B; /* Cyan on Blue */
        ATTR_PANEL_BG    = 0x1F; /* White on Blue */
        ATTR_PANEL_DIR   = 0x1E; /* Yellow on Blue */
        ATTR_PANEL_EXE   = 0x1A; /* Light Green on Blue */
        ATTR_PANEL_SEL   = 0x30; /* Black on Cyan */
        ATTR_PANEL_TITLE = 0x1E; /* Yellow on Blue */
        ATTR_STATUS_BAR  = 0x17; /* Light Gray on Blue */
        ATTR_FN_NUM      = 0x0F; /* Bright White on Black */
        ATTR_FN_LBL      = 0x30; /* Black on Cyan */
        ATTR_DLG_FRAME   = 0x70; /* Black on Light Gray */
        ATTR_DLG_BG      = 0x70; /* Black on Light Gray */
        ATTR_DLG_SEL     = 0x30; /* Black on Cyan */
        ATTR_DLG_BTN     = 0x1F; /* White on Blue */
        ATTR_DLG_BTN_SEL = 0x2F; /* White on Green */
    }
}

static void fm_put_cell(int row, int col, char ch, uint8_t attr) {
    if (row >= 0 && row < 25 && col >= 0 && col < 80) {
        s_screen[row * 80 + col] = ((uint16_t)attr << 8) | (uint8_t)ch;
    }
}

static void fm_put_str(int row, int col, const char *s, uint8_t attr, int max_len) {
    int i = 0;
    while (s && s[i] && i < max_len && (col + i) < 80) {
        fm_put_cell(row, col + i, s[i], attr);
        i++;
    }
}

static void fm_fill_rect(int r1, int c1, int r2, int c2, char ch, uint8_t attr) {
    int r, c;
    for (r = r1; r <= r2; r++) {
        for (c = c1; c <= c2; c++) {
            fm_put_cell(r, c, ch, attr);
        }
    }
}

static void fm_draw_box(int r1, int c1, int r2, int c2, uint8_t attr, int is_double, const char *title) {
    int c, r;
    char tl = is_double ? CH_D_TL  : CH_S_TL;
    char tr = is_double ? CH_D_TR  : CH_S_TR;
    char bl = is_double ? CH_D_BL  : CH_S_BL;
    char br = is_double ? CH_D_BR  : CH_S_BR;
    char h  = is_double ? CH_D_HOR : CH_S_HOR;
    char v  = is_double ? CH_D_VER : CH_S_VER;

    fm_fill_rect(r1 + 1, c1 + 1, r2 - 1, c2 - 1, ' ', attr);

    for (c = c1 + 1; c < c2; c++) {
        fm_put_cell(r1, c, h, attr);
        fm_put_cell(r2, c, h, attr);
    }
    for (r = r1 + 1; r < r2; r++) {
        fm_put_cell(r, c1, v, attr);
        fm_put_cell(r, c2, v, attr);
    }
    fm_put_cell(r1, c1, tl, attr);
    fm_put_cell(r1, c2, tr, attr);
    fm_put_cell(r2, c1, bl, attr);
    fm_put_cell(r2, c2, br, attr);

    if (title && title[0]) {
        int tlen = str_len(title);
        int avail = (c2 - c1) - 4;
        if (tlen > avail) tlen = avail;
        if (tlen > 0) {
            int start = c1 + ((c2 - c1 - tlen) / 2);
            fm_put_cell(r1, start - 1, ' ', attr);
            fm_put_str(r1, start, title, attr, tlen);
            fm_put_cell(r1, start + tlen, ' ', attr);
        }
    }
}

static void fm_invalidate_cache(void) {
    int i;
    for (i = 0; i < 80 * 25; i++) {
        s_screen[i] = ((uint16_t)ATTR_DEFAULT << 8) | ' ';
        s_cache[i] = 0xFFFF;
    }
}

static void fm_flush(void) {
    uint16_t __far *vram = (uint16_t __far *)MK_FP(s_vram_seg, 0);
    int i;
    for (i = 0; i < 80 * 25; i++) {
        uint16_t val = s_screen[i];
        if (val != s_cache[i]) {
            vram[i] = val;
            s_cache[i] = val;
        }
    }
}

static void fm_hide_cursor(void) {
    if (s_mouse_row >= 0 && s_mouse_row < 25 && s_mouse_col >= 0 && s_mouse_col < 80) {
        s_screen[s_mouse_row * 80 + s_mouse_col] ^= 0x7700;
        s_mouse_row = -1;
        s_mouse_col = -1;
    }
}

static void fm_show_cursor(int row, int col) {
    if (row >= 0 && row < 25 && col >= 0 && col < 80) {
        s_screen[row * 80 + col] ^= 0x7700;
        s_mouse_row = row;
        s_mouse_col = col;
    }
}

/* --------------------------------------------------------------------------
 * Directory Reading, Sorting & Tagging Helpers
 * -------------------------------------------------------------------------- */

static int fm_count_tagged(panel_t *p, uint32_t *out_bytes) {
    int count = 0;
    int i;
    if (out_bytes) *out_bytes = 0;
    for (i = 0; i < p->file_count; i++) {
        if (p->files[i].tagged) {
            count++;
            if (out_bytes && !(p->files[i].attr & OS_ATTR_DIRECTORY)) {
                *out_bytes += p->files[i].size;
            }
        }
    }
    return count;
}

static int fm_compare_items(const file_item_t *a, const file_item_t *b, int sort_mode) {
    int a_is_dir = (a->attr & OS_ATTR_DIRECTORY) ? 1 : 0;
    int b_is_dir = (b->attr & OS_ATTR_DIRECTORY) ? 1 : 0;
    if (a_is_dir && !b_is_dir) return -1;
    if (!a_is_dir && b_is_dir) return 1;
    if (a_is_dir && b_is_dir) {
        return str_icmp(a->name, b->name);
    }
    /* Both are files */
    if (sort_mode == 1) { /* Size descending */
        if (a->size > b->size) return -1;
        if (a->size < b->size) return 1;
        return str_icmp(a->name, b->name);
    } else if (sort_mode == 2) { /* Extension */
        const char *ea = get_ext(a->name);
        const char *eb = get_ext(b->name);
        int c = str_icmp(ea, eb);
        if (c != 0) return c;
        return str_icmp(a->name, b->name);
    }
    /* Default: Name */
    return str_icmp(a->name, b->name);
}

static void fm_sort_panel(panel_t *p) {
    int is_root = 0;
    int len = str_len(p->path);
    int start;
    int i, j;

    if (len == 0 || (len >= 2 && p->path[len - 1] == '/' && p->path[len - 2] == ':')) {
        is_root = 1;
    }
    start = is_root ? 0 : 1;

    for (i = start + 1; i < p->file_count; i++) {
        file_item_t key = p->files[i];
        j = i - 1;
        while (j >= start && fm_compare_items(&key, &p->files[j], p->sort_mode) < 0) {
            p->files[j + 1] = p->files[j];
            j--;
        }
        p->files[j + 1] = key;
    }
}

static void fm_refresh_volumes(void) {
    os_vol_info_t vi;
    uint8_t v;
    s_vol_count = 0;
    for (v = 0; v < 16; v++) {
        if (os_vol_info(v, (os_vol_info_t __far *)FP_HERE(&vi)) == 0) {
            str_copy_n(s_volumes[s_vol_count].name, vi.name, sizeof(s_volumes[0].name));
            s_vol_count++;
        }
    }
}

static void fm_load_dir(panel_t *p) {
    os_dir_entry_t dent;
    uint16_t idx = 0;
    int is_root = 0;
    int len = str_len(p->path);

    p->file_count = 0;
    p->cursor = 0;
    p->top_index = 0;

    /* Check if current path is root */
    if (len == 0 || (len >= 2 && p->path[len - 1] == '/' && p->path[len - 2] == ':')) {
        is_root = 1;
    }

    /* Add '..' parent directory if not at root */
    if (!is_root) {
        str_copy(p->files[0].name, "..");
        p->files[0].size = 0;
        p->files[0].attr = OS_ATTR_DIRECTORY;
        p->files[0].tagged = 0;
        p->file_count = 1;
    }

    /* Read directory entries */
    while (os_dir_entry_at(p->path, idx++, (os_dir_entry_t __far *)FP_HERE(&dent)) == 0) {
        if (dent.attr & OS_ATTR_VOLUME_ID) continue;
        if (dent.name[0] == '.' && (dent.name[1] == '\0' || (dent.name[1] == '.' && dent.name[2] == '\0'))) {
            continue;
        }
        if (p->file_count < MAX_PANEL_FILES) {
            str_copy(p->files[p->file_count].name, dent.name);
            p->files[p->file_count].size = dent.size;
            p->files[p->file_count].attr = dent.attr;
            p->files[p->file_count].tagged = 0;
            p->file_count++;
        }
    }

    fm_sort_panel(p);

    if (p->cursor >= p->file_count) {
        p->cursor = (p->file_count > 0) ? p->file_count - 1 : 0;
    }
    p->top_index = 0;
}

static void fm_change_dir(panel_t *p, const char *child) {
    if (str_icmp(child, "..") == 0) {
        /* Go up one level */
        int len = str_len(p->path);
        int i = len - 1;
        while (i >= 0 && p->path[i] != '/') i--;
        if (i >= 0) {
            if (i > 0 && p->path[i - 1] == ':') {
                /* Root e.g. "SYS:/" */
                p->path[i + 1] = '\0';
            } else {
                p->path[i] = '\0';
            }
        }
    } else {
        /* Append child directory */
        int len = str_len(p->path);
        if (len > 0 && p->path[len - 1] != '/') {
            str_cat(p->path, "/");
        }
        str_cat(p->path, child);
    }
    fm_load_dir(p);
}

/* --------------------------------------------------------------------------
 * Dialogs & Popups
 * -------------------------------------------------------------------------- */

static int fm_confirm_box(const char *title, const char *msg) {
    int w = 48, h = 7;
    int r1 = 8, c1 = 16, r2 = r1 + h - 1, c2 = c1 + w - 1;
    int sel = 0; /* 0 = Yes, 1 = No */

    fm_hide_cursor();
    fm_draw_box(r1, c1, r2, c2, ATTR_DLG_FRAME, 1, title);
    fm_fill_rect(r1 + 1, c1 + 1, r2 - 1, c2 - 1, ' ', ATTR_DLG_BG);
    fm_put_str(r1 + 2, c1 + 4, msg, ATTR_DLG_BG, w - 8);

    while (1) {
        fm_put_str(r1 + 4, c1 + 12, " [ Yes ] ", (sel == 0) ? ATTR_DLG_BTN_SEL : ATTR_DLG_BTN, 9);
        fm_put_str(r1 + 4, c1 + 26, " [ No ]  ", (sel == 1) ? ATTR_DLG_BTN_SEL : ATTR_DLG_BTN, 9);
        fm_flush();

        {
            uint16_t key = os_getc();
            uint8_t c = (uint8_t)(key & 0xFF);
            uint8_t sc = (uint8_t)(key >> 8);

            if (c == 0) {
                if (sc == SC_LEFT || sc == SC_RIGHT || sc == SC_UP || sc == SC_DOWN) {
                    sel = 1 - sel;
                }
            } else if (c == '\t') {
                sel = 1 - sel;
            } else if (c == 'y' || c == 'Y') {
                return 1;
            } else if (c == 'n' || c == 'N' || c == 0x1B) {
                return 0;
            } else if (c == '\r' || c == '\n') {
                return (sel == 0);
            }
        }
    }
}

static int fm_input_box(const char *title, const char *prompt, char *buf, int max_len) {
    int w = 54, h = 8;
    int r1 = 8, c1 = 13, r2 = r1 + h - 1, c2 = c1 + w - 1;
    int cur_pos = str_len(buf);

    fm_hide_cursor();
    fm_draw_box(r1, c1, r2, c2, ATTR_DLG_FRAME, 1, title);
    fm_fill_rect(r1 + 1, c1 + 1, r2 - 1, c2 - 1, ' ', ATTR_DLG_BG);
    fm_put_str(r1 + 2, c1 + 4, prompt, ATTR_DLG_BG, w - 8);
    os_set_cursor_visible(1);

    while (1) {
        int i;
        char disp[46];
        int dlen = str_len(buf);
        for (i = 0; i < 44; i++) {
            disp[i] = (i < dlen) ? buf[i] : ' ';
        }
        disp[44] = '\0';

        fm_put_str(r1 + 4, c1 + 4, disp, ATTR_PANEL_SEL, 44);
        os_set_cursor(r1 + 4, c1 + 4 + cur_pos);
        fm_flush();

        {
            uint16_t key = os_getc();
            uint8_t c = (uint8_t)(key & 0xFF);
            uint8_t sc = (uint8_t)(key >> 8);

            if (c == 0) {
                if (sc == SC_LEFT && cur_pos > 0) cur_pos--;
                else if (sc == SC_RIGHT && cur_pos < str_len(buf)) cur_pos++;
                else if (sc == SC_HOME) cur_pos = 0;
                else if (sc == SC_END) cur_pos = str_len(buf);
            } else if (c == 0x1B) {
                os_set_cursor_visible(0);
                return 0; /* Cancel */
            } else if (c == '\r' || c == '\n') {
                os_set_cursor_visible(0);
                return 1; /* OK */
            } else if (c == '\b') {
                if (cur_pos > 0) {
                    int len = str_len(buf);
                    int k;
                    for (k = cur_pos - 1; k < len; k++) {
                        buf[k] = buf[k + 1];
                    }
                    cur_pos--;
                }
            } else if (c >= ' ' && c < 0x7F && str_len(buf) < max_len - 1 && cur_pos < 42) {
                int len = str_len(buf);
                int k;
                for (k = len; k >= cur_pos; k--) {
                    buf[k + 1] = buf[k];
                }
                buf[cur_pos++] = (char)c;
            }
        }
    }
}

static char fm_drive_shortcut(int idx) {
    if (idx < 9) return (char)('1' + idx);
    if (idx == 9) return '0';
    return (char)('A' + (idx - 10));
}

static void fm_drive_menu(int is_right) {
    panel_t *p = is_right ? &s_right : &s_left;
    int w = 24;
    int h;
    int r1, c1, r2, c2;
    int sel = 0;
    int i;

    fm_refresh_volumes();
    if (s_vol_count == 0) return;

    h = s_vol_count + 4;
    r1 = (25 - h) / 2;
    if (r1 < 1) r1 = 1;
    r2 = r1 + h - 1;

    c1 = is_right ? 46 : 10;
    c2 = c1 + w - 1;

    /* Pre-select current drive in menu */
    for (i = 0; i < s_vol_count; i++) {
        int vlen = str_len(s_volumes[i].name);
        if (str_nicmp(p->path, s_volumes[i].name, vlen) == 0 && p->path[vlen] == ':') {
            sel = i;
            break;
        }
    }

    fm_hide_cursor();
    fm_draw_box(r1, c1, r2, c2, ATTR_DLG_FRAME, 1, "Select Drive");

    while (1) {
        for (i = 0; i < s_vol_count; i++) {
            char line[32];
            int pos = 0;
            int k;
            uint8_t attr = (i == sel) ? ATTR_DLG_SEL : ATTR_DLG_BG;
            char sc_ch = fm_drive_shortcut(i);

            line[pos++] = (i == sel) ? (char)0x10 : ' ';
            line[pos++] = ' ';
            line[pos++] = '[';
            line[pos++] = sc_ch;
            line[pos++] = ']';
            line[pos++] = ' ';

            for (k = 0; s_volumes[i].name[k] && k < 7 && pos < 18; k++) {
                line[pos++] = s_volumes[i].name[k];
            }
            line[pos++] = ':';

            while (pos < 20) {
                line[pos++] = ' ';
            }
            line[20] = '\0';

            fm_put_str(r1 + 2 + i, c1 + 2, line, attr, 20);
        }
        fm_flush();

        {
            uint16_t key = 0;
            int got_key = 0;

            while (!got_key) {
                if (os_kbhit()) {
                    key = os_getc();
                    got_key = 1;
                } else if (s_mouse_avail) {
                    uint16_t mx, my;
                    uint16_t btn = os_mouse_get_pos(&mx, &my);
                    int mc = (int)(mx / 8);
                    int mr = (int)(my / 8);

                    if (btn & 1) {
                        if (mc >= c1 + 1 && mc <= c2 - 1 && mr >= r1 + 2 && mr < r1 + 2 + s_vol_count) {
                            int clicked = mr - (r1 + 2);
                            if (clicked == sel) {
                                str_copy(p->path, s_volumes[sel].name);
                                str_cat(p->path, ":/");
                                fm_load_dir(p);
                                while (os_mouse_get_pos(&mx, &my) & 1);
                                return;
                            } else {
                                sel = clicked;
                                while (os_mouse_get_pos(&mx, &my) & 1);
                                break;
                            }
                        } else if (mc < c1 || mc > c2 || mr < r1 || mr > r2) {
                            while (os_mouse_get_pos(&mx, &my) & 1);
                            return; /* Cancel */
                        }
                    }
                }
            }

            if (got_key) {
                uint8_t c = (uint8_t)(key & 0xFF);
                uint8_t sc = (uint8_t)(key >> 8);

                if (c == 0) {
                    if (sc == SC_UP && sel > 0) sel--;
                    else if (sc == SC_DOWN && sel < s_vol_count - 1) sel++;
                    else if (sc == SC_HOME) sel = 0;
                    else if (sc == SC_END) sel = s_vol_count - 1;
                } else if (c == 0x1B) {
                    break; /* Cancel */
                } else if (c == '\r' || c == '\n' || c == ' ') {
                    str_copy(p->path, s_volumes[sel].name);
                    str_cat(p->path, ":/");
                    fm_load_dir(p);
                    break;
                } else {
                    char typed = to_upper((char)c);
                    for (i = 0; i < s_vol_count; i++) {
                        if (typed == fm_drive_shortcut(i)) {
                            str_copy(p->path, s_volumes[i].name);
                            str_cat(p->path, ":/");
                            fm_load_dir(p);
                            return;
                        }
                    }
                }
            }
        }
    }
}

static void fm_action_drive_info(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    os_vol_info_t vi;
    char vol_name[8];
    int vlen = 0;
    int i;
    int w = 46, h = 10;
    int r1 = 7, c1 = 17, r2 = r1 + h - 1, c2 = c1 + w - 1;

    while (ap->path[vlen] && ap->path[vlen] != ':' && vlen < 7) {
        vol_name[vlen] = ap->path[vlen];
        vlen++;
    }
    vol_name[vlen] = '\0';

    fm_refresh_volumes();
    for (i = 0; i < 16; i++) {
        if (os_vol_info((uint8_t)i, (os_vol_info_t __far *)FP_HERE(&vi)) == 0) {
            if (str_icmp(vi.name, vol_name) == 0) {
                char line[40];
                char num[16];
                uint32_t tot_sec = mul_u16(vi.total_clusters, vi.sectors_per_cluster);
                uint32_t free_sec = mul_u16(vi.free_clusters, vi.sectors_per_cluster);
                uint32_t total_kb = tot_sec >> 1; /* sectors * 512 / 1024 */
                uint32_t free_kb  = free_sec >> 1;

                fm_hide_cursor();
                fm_draw_box(r1, c1, r2, c2, ATTR_DLG_FRAME, 1, "Drive Information");

                str_copy(line, "Volume Name:   ");
                str_cat(line, vi.name);
                str_cat(line, ":");
                fm_put_str(r1 + 2, c1 + 4, line, ATTR_DLG_BG, w - 8);

                str_copy(line, "File System:   ");
                str_cat(line, (vi.fat_type == 1) ? "FAT12" : "FAT16");
                if (vi.is_removable) str_cat(line, " (Floppy)");
                fm_put_str(r1 + 3, c1 + 4, line, ATTR_DLG_BG, w - 8);

                str_copy(line, "Total Space:   ");
                format_size(total_kb, num, sizeof(num));
                str_cat(line, num);
                str_cat(line, " KB");
                fm_put_str(r1 + 4, c1 + 4, line, ATTR_DLG_BG, w - 8);

                str_copy(line, "Free Space:    ");
                format_size(free_kb, num, sizeof(num));
                str_cat(line, num);
                str_cat(line, " KB");
                fm_put_str(r1 + 5, c1 + 4, line, ATTR_DLG_BG, w - 8);

                fm_put_str(r1 + 7, c1 + 18, "[ OK ]", ATTR_DLG_BTN_SEL, 6);
                fm_flush();

                while (1) {
                    uint16_t key = os_getc();
                    uint8_t c = (uint8_t)(key & 0xFF);
                    if (c == '\r' || c == '\n' || c == ' ' || c == 0x1B) break;
                }
                return;
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Main Dual-Panel Drawing
 * -------------------------------------------------------------------------- */

static void fm_draw_panel(panel_t *p, int is_left, int is_active) {
    int c1 = is_left ? LEFT_COL_START : RIGHT_COL_START;
    int c2 = is_left ? LEFT_COL_END   : RIGHT_COL_END;
    int r1 = PANEL_ROW_TOP;
    int r2 = PANEL_ROW_BOT;
    int row;
    uint8_t f_attr = is_active ? ATTR_PANEL_FRAME : 0x07;
    char title[40];

    /* Format title header e.g. " SYS:/BIN " */
    title[0] = ' ';
    str_copy_n(title + 1, p->path, 34);
    str_cat(title, " ");

    fm_draw_box(r1, c1, r2, c2, f_attr, 1, title);

    /* Column Headers on Row 2 */
    fm_put_cell(r1 + 1, c1, (char)0xC7, f_attr);
    fm_fill_rect(r1 + 1, c1 + 1, r1 + 1, c2 - 1, CH_S_HOR, f_attr);
    fm_put_cell(r1 + 1, c1 + 16, (char)0xD1, f_attr); /* ╤ */
    fm_put_cell(r1 + 1, c1 + 27, (char)0xD1, f_attr);
    fm_put_cell(r1 + 1, c2, (char)0xB6, f_attr);

    fm_put_str(r1 + 1, c1 + 2, "Name", ATTR_PANEL_TITLE, 4);
    fm_put_str(r1 + 1, c1 + 19, "Size", ATTR_PANEL_TITLE, 4);
    fm_put_str(r1 + 1, c1 + 29, "Attr", ATTR_PANEL_TITLE, 4);

    /* Render Files on Rows 3..20 (18 rows) */
    for (row = 0; row < 18; row++) {
        int fidx = p->top_index + row;
        int scr_row = r1 + 2 + row;

        fm_put_cell(scr_row, c1, CH_D_VER, f_attr);
        fm_put_cell(scr_row, c1 + 16, CH_S_VER, f_attr);
        fm_put_cell(scr_row, c1 + 27, CH_S_VER, f_attr);
        fm_put_cell(scr_row, c2, CH_D_VER, f_attr);

        if (fidx < p->file_count) {
            file_item_t *it = &p->files[fidx];
            int is_sel = (is_active && fidx == p->cursor);
            uint8_t txt_attr = ATTR_PANEL_BG;
            char sz_buf[12];
            int i;

            if (is_sel) {
                if (it->tagged) {
                    txt_attr = (s_video_type == VIDEO_TYPE_MDA) ? 0x70 : 0x3E; /* Yellow on Cyan */
                } else {
                    txt_attr = ATTR_PANEL_SEL;
                }
            } else if (it->tagged) {
                txt_attr = (s_video_type == VIDEO_TYPE_MDA) ? 0x0F : 0x1E; /* Yellow on Blue */
            } else if (it->attr & OS_ATTR_DIRECTORY) {
                txt_attr = ATTR_PANEL_DIR;
            } else if (is_binary_name(it->name)) {
                txt_attr = ATTR_PANEL_EXE;
            }

            /* Clear item cells */
            for (i = c1 + 1; i < c2; i++) {
                if (i != c1 + 16 && i != c1 + 27) {
                    fm_put_cell(scr_row, i, ' ', txt_attr);
                }
            }

            /* Tag marker */
            if (it->tagged) {
                fm_put_cell(scr_row, c1 + 1, '*', (is_sel ? (s_video_type == VIDEO_TYPE_MDA ? 0x70 : 0x3E) :
                                                            (s_video_type == VIDEO_TYPE_MDA ? 0x0F : 0x1E)));
            }

            /* Filename */
            fm_put_str(scr_row, c1 + 2, it->name, txt_attr, 13);

            /* Size / <DIR> */
            if (it->attr & OS_ATTR_DIRECTORY) {
                fm_put_str(scr_row, c1 + 19, "<DIR>", txt_attr, 5);
            } else {
                format_size(it->size, sz_buf, sizeof(sz_buf));
                {
                    int sl = str_len(sz_buf);
                    int pad = (sl < 9) ? 9 - sl : 0;
                    fm_put_str(scr_row, c1 + 17 + pad, sz_buf, txt_attr, 9);
                }
            }

            /* Attributes */
            {
                char at_str[5];
                at_str[0] = (it->attr & OS_ATTR_READ_ONLY) ? 'R' : '.';
                at_str[1] = (it->attr & OS_ATTR_HIDDEN)    ? 'H' : '.';
                at_str[2] = (it->attr & OS_ATTR_SYSTEM)    ? 'S' : '.';
                at_str[3] = (it->attr & OS_ATTR_ARCHIVE)   ? 'A' : '.';
                at_str[4] = '\0';
                fm_put_str(scr_row, c1 + 29, at_str, txt_attr, 4);
            }
        } else {
            /* Empty row */
            int i;
            for (i = c1 + 1; i < c2; i++) {
                if (i != c1 + 16 && i != c1 + 27) {
                    fm_put_cell(scr_row, i, ' ', ATTR_PANEL_BG);
                }
            }
        }
    }

    /* Panel Footer on Row 21 */
    {
        char foot[24];
        char count_str[8];
        format_size((uint32_t)p->file_count, count_str, sizeof(count_str));
        foot[0] = ' ';
        foot[1] = '\0';
        str_cat(foot, count_str);
        str_cat(foot, " item(s) ");
        fm_put_cell(r2, c1 + 16, (char)0xCF, f_attr); /* ╧ */
        fm_put_cell(r2, c1 + 27, (char)0xCF, f_attr);
        fm_put_str(r2, c1 + 18, foot, f_attr, 18);
    }
}

static void fm_draw_bars(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    file_item_t *sel_item = (ap->file_count > 0) ? &ap->files[ap->cursor] : 0;
    int i;

    /* Top Menu Bar (Row 0) */
    fm_fill_rect(0, 0, 0, 79, ' ', ATTR_STATUS_BAR);
    for (i = 0; i < 4; i++) {
        uint8_t a = (s_menu_active && s_menu_cat == i) ? ATTR_PANEL_SEL : ATTR_STATUS_BAR;
        fm_put_str(0, s_menu_headers[i].col_start, s_menu_headers[i].title, a, 10);
    }
    fm_put_str(0, 68, "[F9: Menu]", ATTR_STATUS_BAR, 11);

    /* Selected Item / Tagged Summary (Row 22) */
    fm_fill_rect(22, 0, 22, 79, ' ', ATTR_STATUS_BAR);
    {
        uint32_t t_bytes = 0;
        int t_cnt = fm_count_tagged(ap, &t_bytes);
        if (t_cnt > 0) {
            char sum[80];
            char c_str[16];
            char b_str[16];
            format_size((uint32_t)t_cnt, c_str, sizeof(c_str));
            format_size(t_bytes, b_str, sizeof(b_str));
            str_copy(sum, " ");
            str_cat(sum, c_str);
            str_cat(sum, " file(s) tagged (");
            str_cat(sum, b_str);
            str_cat(sum, " bytes)");
            fm_put_str(22, 1, sum, (s_video_type == VIDEO_TYPE_MDA) ? 0x0F : 0x1E, 78);
        } else if (sel_item) {
            char sum[80];
            char sz_str[16];
            str_copy(sum, " ");
            str_cat(sum, sel_item->name);
            if (sel_item->attr & OS_ATTR_DIRECTORY) {
                str_cat(sum, "  <DIR>");
            } else {
                str_cat(sum, "  ");
                format_size(sel_item->size, sz_str, sizeof(sz_str));
                str_cat(sum, sz_str);
                str_cat(sum, " bytes");
            }
            fm_put_str(22, 1, sum, ATTR_STATUS_BAR, 78);
        }
    }

    /* Live Interactive Shell Prompt (Row 23) */
    {
        int plen = str_len(ap->path);
        int pstart;
        if (plen > 35) plen = 35;
        fm_fill_rect(23, 0, 23, 79, ' ', 0x07);
        fm_put_str(23, 0, ap->path, 0x0F, plen);
        fm_put_cell(23, plen, '>', 0x0A);
        pstart = plen + 1;
        fm_put_str(23, pstart, s_cmd_buf, 0x0F, 79 - pstart);
    }

    /* Function Key Bar (Row 24) */
    fm_fill_rect(24, 0, 24, 79, ' ', ATTR_FN_LBL);
    {
        static const struct {
            const char *num;
            const char *lbl;
        } fn_keys[10] = {
            { " 1", "Help  " },
            { " 2", "Drive " },
            { " 3", "View  " },
            { " 4", "Edit  " },
            { " 5", "Copy  " },
            { " 6", "RenMov" },
            { " 7", "Mkdir " },
            { " 8", "Delete" },
            { " 9", "Menu  " },
            { "10", "Quit  " }
        };
        int col = 0;
        for (i = 0; i < 10; i++) {
            fm_put_str(24, col, fn_keys[i].num, ATTR_FN_NUM, 2);
            col += 2;
            fm_put_str(24, col, fn_keys[i].lbl, ATTR_FN_LBL, 6);
            col += 6;
        }
    }
}

static int fm_menu_item_count(int cat) {
    if (cat == 0 || cat == 3) return 5;
    if (cat == 1) return 12;
    if (cat == 2) return 3;
    return 0;
}

static int fm_menu_is_separator(int cat, int item) {
    if (cat == 1 && (item == 6 || item == 10)) return 1;
    return 0;
}

static void fm_draw_dropdown(void) {
    int c1, c2, r1 = 1, r2, num_items, i;
    panel_t *p = (s_menu_cat == 3) ? &s_right : &s_left;

    if (s_menu_cat == 0) {
        c1 = 1; c2 = 26; num_items = 5; r2 = 7;
    } else if (s_menu_cat == 1) {
        c1 = 8; c2 = 33; num_items = 12; r2 = 14;
    } else if (s_menu_cat == 2) {
        c1 = 16; c2 = 41; num_items = 3; r2 = 5;
    } else {
        c1 = 23; c2 = 48; num_items = 5; r2 = 7;
    }

    fm_draw_box(r1, c1, r2, c2, ATTR_DLG_FRAME, 0, 0);

    for (i = 0; i < num_items; i++) {
        int scr_r = r1 + 1 + i;
        uint8_t a = (i == s_menu_item) ? ATTR_DLG_SEL : ATTR_DLG_BG;
        char item_text[32];
        item_text[0] = '\0';

        if (s_menu_cat == 0 || s_menu_cat == 3) {
            char check = ' ';
            if (i == 1 && p->sort_mode == 0) check = (char)0xFB; /* √ */
            else if (i == 2 && p->sort_mode == 1) check = (char)0xFB;
            else if (i == 3 && p->sort_mode == 2) check = (char)0xFB;

            if (i == 0) str_copy(item_text, "  Drive...            F2");
            else if (i == 1) { str_copy(item_text, "  Sort by Name"); item_text[0] = check; }
            else if (i == 2) { str_copy(item_text, "  Sort by Size"); item_text[0] = check; }
            else if (i == 3) { str_copy(item_text, "  Sort by Extension"); item_text[0] = check; }
            else if (i == 4) str_copy(item_text, "  Re-read directory");
        } else if (s_menu_cat == 1) {
            if (i == 0) str_copy(item_text, " View                F3");
            else if (i == 1) str_copy(item_text, " Edit                F4");
            else if (i == 2) str_copy(item_text, " Copy                F5");
            else if (i == 3) str_copy(item_text, " Rename or Move      F6");
            else if (i == 4) str_copy(item_text, " Make Directory      F7");
            else if (i == 5) str_copy(item_text, " Delete              F8");
            else if (i == 6) {
                /* Separator */
                fm_put_cell(scr_r, c1, (char)0xC3, ATTR_DLG_FRAME);
                fm_fill_rect(scr_r, c1 + 1, scr_r, c2 - 1, CH_S_HOR, ATTR_DLG_FRAME);
                fm_put_cell(scr_r, c2, (char)0xB4, ATTR_DLG_FRAME);
                continue;
            }
            else if (i == 7) str_copy(item_text, " Select Group         +");
            else if (i == 8) str_copy(item_text, " Deselect Group       -");
            else if (i == 9) str_copy(item_text, " Invert Selection     *");
            else if (i == 10) {
                /* Separator */
                fm_put_cell(scr_r, c1, (char)0xC3, ATTR_DLG_FRAME);
                fm_fill_rect(scr_r, c1 + 1, scr_r, c2 - 1, CH_S_HOR, ATTR_DLG_FRAME);
                fm_put_cell(scr_r, c2, (char)0xB4, ATTR_DLG_FRAME);
                continue;
            }
            else if (i == 11) str_copy(item_text, " Quit               F10");
        } else if (s_menu_cat == 2) {
            if (i == 0) str_copy(item_text, " Select Drive...     F2");
            else if (i == 1) str_copy(item_text, " Drive Information");
            else if (i == 2) str_copy(item_text, " Rescan Volumes");
        }

        /* Pad item_text with spaces to fill width */
        {
            int tlen = str_len(item_text);
            int width = (c2 - c1) - 2;
            int pad;
            fm_put_cell(scr_r, c1 + 1, ' ', a);
            fm_put_str(scr_r, c1 + 2, item_text, a, width);
            for (pad = c1 + 2 + tlen; pad < c2; pad++) {
                fm_put_cell(scr_r, pad, ' ', a);
            }
        }
    }
}

static void fm_draw_screen(void) {
    fm_draw_bars();
    fm_draw_panel(&s_left, 1, (s_active == 0));
    fm_draw_panel(&s_right, 0, (s_active == 1));
    if (s_menu_active) {
        fm_draw_dropdown();
    }
    s_mouse_row = -1;
    s_mouse_col = -1;
    if (s_mouse_avail) {
        uint16_t mx, my;
        os_mouse_get_pos(&mx, &my);
        fm_show_cursor((int)(my / 8), (int)(mx / 8));
    }
    fm_flush();

    /* Position hardware cursor on Row 23 live prompt */
    if (!s_menu_active) {
        panel_t *ap = (s_active == 0) ? &s_left : &s_right;
        int plen = str_len(ap->path);
        if (plen > 35) plen = 35;
        os_set_cursor(23, plen + 1 + s_cmd_pos);
        os_set_cursor_visible(1);
    } else {
        os_set_cursor_visible(0);
    }
}

/* --------------------------------------------------------------------------
 * Action Handlers (F3 View, F4 Edit, F5 Copy, F6 Move, F7 Mkdir, F8 Delete)
 * -------------------------------------------------------------------------- */

static void fm_get_full_path(panel_t *p, const char *name, char *out, int max_len) {
    int len = str_len(p->path);
    str_copy_n(out, p->path, max_len);
    if (len > 0 && p->path[len - 1] != '/') {
        str_cat(out, "/");
    }
    str_cat(out, name);
}

static void fm_execute_cmd(const char *cmd_line) {
    char cmd[80];
    char args[80];
    char prog[PATH_MAX_LEN];
    int i = 0, j = 0;

    while (cmd_line[i] == ' ') i++;
    if (cmd_line[i] == '\0') return;

    while (cmd_line[i] && cmd_line[i] != ' ' && j < sizeof(cmd) - 1) {
        cmd[j++] = cmd_line[i++];
    }
    cmd[j] = '\0';

    while (cmd_line[i] == ' ') i++;

    j = 0;
    while (cmd_line[i] && j < sizeof(args) - 1) {
        args[j++] = cmd_line[i++];
    }
    args[j] = '\0';

    if (fm_is_forbidden_bin(cmd)) {
        fm_confirm_box("System File", "Cannot execute kernel or bootloader!");
        return;
    }

    {
        int has_path = 0;
        int k;
        panel_t *ap = (s_active == 0) ? &s_left : &s_right;
        for (k = 0; cmd[k]; k++) {
            if (cmd[k] == '/' || cmd[k] == ':') {
                has_path = 1;
                break;
            }
        }
        if (!has_path) {
            fm_get_full_path(ap, cmd, prog, sizeof(prog));
        } else {
            str_copy_n(prog, cmd, sizeof(prog));
        }
    }

    fm_hide_cursor();
    os_cls(ATTR_DEFAULT);
    os_set_cursor(0, 0);
    os_set_cursor_visible(1);

    {
        int rc;
        if (is_cpm_binary(prog) || is_cpm_binary(cmd)) {
            char cpm_args[80];
            str_copy_n(cpm_args, prog, sizeof(cpm_args));
            if (args[0]) {
                str_cat(cpm_args, " ");
                str_cat(cpm_args, args);
            }
            rc = os_exec("/BIN/CPM.BIN", cpm_args);
        } else {
            rc = os_exec(prog, args);
            if (rc < 0) {
                rc = os_exec(cmd, args);
            }
        }
        if (rc < 0) {
            os_puts("Bad command or file name: ");
            os_puts(cmd);
            os_puts("\n");
        }
    }

    os_puts("\n[Press any key to return to FM...]");
    os_getc();
    os_cls(ATTR_DEFAULT);
    fm_init_colors();
    fm_invalidate_cache();
    fm_load_dir(&s_left);
    fm_load_dir(&s_right);
}

static void fm_action_view(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    char full[PATH_MAX_LEN];
    if (ap->file_count == 0) return;
    if (ap->files[ap->cursor].attr & OS_ATTR_DIRECTORY) return;

    fm_hide_cursor();
    fm_get_full_path(ap, ap->files[ap->cursor].name, full, sizeof(full));

    if (str_ends_with_ic(full, ".CGA") || str_ends_with_ic(full, ".VGA") || str_ends_with_ic(full, ".PCX")) {
        os_exec("/BIN/VIEW.BIN", full);
    } else {
        os_exec("/BIN/MORE.BIN", full);
    }
    os_cls(ATTR_DEFAULT);
    fm_init_colors();
    fm_invalidate_cache();
}

static void fm_action_edit(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    char full[PATH_MAX_LEN];
    if (ap->file_count == 0) return;
    if (ap->files[ap->cursor].attr & OS_ATTR_DIRECTORY) return;

    fm_hide_cursor();
    fm_get_full_path(ap, ap->files[ap->cursor].name, full, sizeof(full));
    os_exec("/BIN/EDIT.BIN", full);
    os_cls(ATTR_DEFAULT);
    fm_init_colors();
    fm_invalidate_cache();
    fm_load_dir(ap);
}

static void fm_action_copy(void) {
    panel_t *src_p = (s_active == 0) ? &s_left : &s_right;
    panel_t *dst_p = (s_active == 0) ? &s_right : &s_left;
    uint32_t tagged_bytes = 0;
    int tagged_cnt = fm_count_tagged(src_p, &tagged_bytes);

    if (tagged_cnt > 0) {
        char dst_dir[PATH_MAX_LEN];
        char prompt[64];
        char num[8];
        int i;

        format_size((uint32_t)tagged_cnt, num, sizeof(num));
        str_copy(prompt, "Copy ");
        str_cat(prompt, num);
        str_cat(prompt, " file(s) to destination:");

        str_copy_n(dst_dir, dst_p->path, sizeof(dst_dir));
        if (fm_input_box("Copy Files", prompt, dst_dir, sizeof(dst_dir))) {
            for (i = 0; i < src_p->file_count; i++) {
                if (src_p->files[i].tagged) {
                    if (!(src_p->files[i].attr & OS_ATTR_DIRECTORY)) {
                        char src_full[PATH_MAX_LEN];
                        char dst_full[PATH_MAX_LEN];
                        int dlen;

                        fm_get_full_path(src_p, src_p->files[i].name, src_full, sizeof(src_full));
                        str_copy_n(dst_full, dst_dir, sizeof(dst_full));
                        dlen = str_len(dst_full);
                        if (dlen > 0 && dst_full[dlen - 1] != '/') {
                            str_cat(dst_full, "/");
                        }
                        str_cat(dst_full, src_p->files[i].name);

                        os_file_copy(src_full, dst_full, 0);
                    }
                    src_p->files[i].tagged = 0;
                }
            }
            fm_load_dir(src_p);
            fm_load_dir(dst_p);
        }
    } else {
        /* Single file copy */
        char src_full[PATH_MAX_LEN];
        char dst_full[PATH_MAX_LEN];

        if (src_p->file_count == 0) return;
        if (src_p->files[src_p->cursor].attr & OS_ATTR_DIRECTORY) return;

        fm_get_full_path(src_p, src_p->files[src_p->cursor].name, src_full, sizeof(src_full));
        fm_get_full_path(dst_p, src_p->files[src_p->cursor].name, dst_full, sizeof(dst_full));

        if (fm_input_box("Copy File", "Copy to destination:", dst_full, sizeof(dst_full))) {
            os_file_copy(src_full, dst_full, 0);
            fm_load_dir(src_p);
            fm_load_dir(dst_p);
        }
    }
}

static void fm_action_move(void) {
    panel_t *src_p = (s_active == 0) ? &s_left : &s_right;
    panel_t *dst_p = (s_active == 0) ? &s_right : &s_left;
    uint32_t tagged_bytes = 0;
    int tagged_cnt = fm_count_tagged(src_p, &tagged_bytes);

    if (tagged_cnt > 0) {
        char dst_dir[PATH_MAX_LEN];
        char prompt[64];
        char num[8];
        int i;

        format_size((uint32_t)tagged_cnt, num, sizeof(num));
        str_copy(prompt, "Move ");
        str_cat(prompt, num);
        str_cat(prompt, " file(s) to destination:");

        str_copy_n(dst_dir, dst_p->path, sizeof(dst_dir));
        if (fm_input_box("Move Files", prompt, dst_dir, sizeof(dst_dir))) {
            for (i = 0; i < src_p->file_count; i++) {
                if (src_p->files[i].tagged) {
                    if (str_icmp(src_p->files[i].name, "..") != 0) {
                        char src_full[PATH_MAX_LEN];
                        char dst_full[PATH_MAX_LEN];
                        int dlen;

                        fm_get_full_path(src_p, src_p->files[i].name, src_full, sizeof(src_full));
                        str_copy_n(dst_full, dst_dir, sizeof(dst_full));
                        dlen = str_len(dst_full);
                        if (dlen > 0 && dst_full[dlen - 1] != '/') {
                            str_cat(dst_full, "/");
                        }
                        str_cat(dst_full, src_p->files[i].name);

                        os_file_rename(src_full, dst_full);
                    }
                    src_p->files[i].tagged = 0;
                }
            }
            fm_load_dir(src_p);
            fm_load_dir(dst_p);
        }
    } else {
        /* Single file rename / move */
        char src_full[PATH_MAX_LEN];
        char dst_full[PATH_MAX_LEN];

        if (src_p->file_count == 0) return;
        if (str_icmp(src_p->files[src_p->cursor].name, "..") == 0) return;

        fm_get_full_path(src_p, src_p->files[src_p->cursor].name, src_full, sizeof(src_full));
        fm_get_full_path(dst_p, src_p->files[src_p->cursor].name, dst_full, sizeof(dst_full));

        if (fm_input_box("Rename / Move", "New name or destination:", dst_full, sizeof(dst_full))) {
            os_file_rename(src_full, dst_full);
            fm_load_dir(src_p);
            fm_load_dir(dst_p);
        }
    }
}

static void fm_action_mkdir(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    char new_dir[PATH_MAX_LEN];
    char full[PATH_MAX_LEN];
    new_dir[0] = '\0';

    if (fm_input_box("Create Directory", "Directory name:", new_dir, sizeof(new_dir))) {
        if (new_dir[0]) {
            fm_get_full_path(ap, new_dir, full, sizeof(full));
            os_mkdir(full);
            fm_load_dir(ap);
        }
    }
}

static void fm_action_delete(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    uint32_t tagged_bytes = 0;
    int tagged_cnt = fm_count_tagged(ap, &tagged_bytes);

    if (tagged_cnt > 0) {
        char prompt[64];
        char num[8];
        int i;

        format_size((uint32_t)tagged_cnt, num, sizeof(num));
        str_copy(prompt, "Delete ");
        str_cat(prompt, num);
        str_cat(prompt, " tagged file(s)?");

        if (fm_confirm_box("Confirm Delete", prompt)) {
            for (i = 0; i < ap->file_count; i++) {
                if (ap->files[i].tagged) {
                    if (str_icmp(ap->files[i].name, "..") != 0) {
                        char full[PATH_MAX_LEN];
                        fm_get_full_path(ap, ap->files[i].name, full, sizeof(full));
                        if (ap->files[i].attr & OS_ATTR_DIRECTORY) {
                            os_rmdir(full);
                        } else {
                            os_file_delete(full);
                        }
                    }
                    ap->files[i].tagged = 0;
                }
            }
            fm_load_dir(ap);
        }
    } else {
        /* Single file delete */
        char full[PATH_MAX_LEN];
        char prompt[64];

        if (ap->file_count == 0) return;
        if (str_icmp(ap->files[ap->cursor].name, "..") == 0) return;

        fm_get_full_path(ap, ap->files[ap->cursor].name, full, sizeof(full));

        str_copy(prompt, "Delete \"");
        str_cat(prompt, ap->files[ap->cursor].name);
        str_cat(prompt, "\"?");

        if (fm_confirm_box("Confirm Delete", prompt)) {
            if (ap->files[ap->cursor].attr & OS_ATTR_DIRECTORY) {
                os_rmdir(full);
            } else {
                os_file_delete(full);
            }
            fm_load_dir(ap);
        }
    }
}

static void fm_action_select_group(int select) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    char mask[16];
    int i;

    str_copy(mask, "*.*");
    if (fm_input_box(select ? "Select Files" : "Deselect Files", "Enter file mask:", mask, sizeof(mask))) {
        for (i = 0; i < ap->file_count; i++) {
            if (str_icmp(ap->files[i].name, "..") == 0) continue;
            if (ap->files[i].attr & OS_ATTR_DIRECTORY) continue;
            if (fm_matches_mask(mask, ap->files[i].name)) {
                ap->files[i].tagged = select;
            }
        }
    }
}

static void fm_action_invert_selection(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    int i;
    for (i = 0; i < ap->file_count; i++) {
        if (str_icmp(ap->files[i].name, "..") == 0) continue;
        if (ap->files[i].attr & OS_ATTR_DIRECTORY) continue;
        ap->files[i].tagged = !ap->files[i].tagged;
    }
}

static void fm_action_enter(void) {
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;
    if (ap->file_count == 0) return;

    if (ap->files[ap->cursor].attr & OS_ATTR_DIRECTORY) {
        fm_change_dir(ap, ap->files[ap->cursor].name);
    } else if (is_binary_name(ap->files[ap->cursor].name)) {
        char full[PATH_MAX_LEN];
        if (fm_is_forbidden_bin(ap->files[ap->cursor].name)) {
            fm_confirm_box("System File", "Cannot execute kernel or bootloader!");
            return;
        }
        fm_hide_cursor();
        fm_get_full_path(ap, ap->files[ap->cursor].name, full, sizeof(full));
        os_cls(ATTR_DEFAULT);
        if (is_cpm_binary(full)) {
            os_exec("/BIN/CPM.BIN", full);
        } else {
            os_exec(full, "");
        }
        os_puts("\n[Press any key to return to FM...]");
        os_getc();
        os_cls(ATTR_DEFAULT);
        fm_init_colors();
        fm_invalidate_cache();
        fm_load_dir(&s_left);
        fm_load_dir(&s_right);
    }
}

static void fm_menu_activate(int cat, int item) {
    s_menu_active = 0;
    if (cat == 0 || cat == 3) {
        panel_t *p = (cat == 0) ? &s_left : &s_right;
        if (item == 0) fm_drive_menu(cat == 3 ? 1 : 0);
        else if (item == 1) { p->sort_mode = 0; fm_sort_panel(p); }
        else if (item == 2) { p->sort_mode = 1; fm_sort_panel(p); }
        else if (item == 3) { p->sort_mode = 2; fm_sort_panel(p); }
        else if (item == 4) { fm_load_dir(p); }
    } else if (cat == 1) {
        if (item == 0) fm_action_view();
        else if (item == 1) fm_action_edit();
        else if (item == 2) fm_action_copy();
        else if (item == 3) fm_action_move();
        else if (item == 4) fm_action_mkdir();
        else if (item == 5) fm_action_delete();
        else if (item == 7) fm_action_select_group(1);
        else if (item == 8) fm_action_select_group(0);
        else if (item == 9) fm_action_invert_selection();
        else if (item == 11) s_running = 0;
    } else if (cat == 2) {
        if (item == 0) fm_drive_menu(s_active);
        else if (item == 1) fm_action_drive_info();
        else if (item == 2) {
            fm_refresh_volumes();
            fm_load_dir(&s_left);
            fm_load_dir(&s_right);
        }
    }
}

/* --------------------------------------------------------------------------
 * Main Event Loop & Keyboard / Mouse Dispatcher
 * -------------------------------------------------------------------------- */

static void fm_handle_key(uint16_t key) {
    uint8_t c  = (uint8_t)(key & 0xFF);
    uint8_t sc = (uint8_t)(key >> 8);
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;

    if (s_menu_active) {
        if (c == 0) {
            if (sc == SC_LEFT) {
                s_menu_cat = (s_menu_cat + 3) % 4;
                s_menu_item = 0;
            } else if (sc == SC_RIGHT) {
                s_menu_cat = (s_menu_cat + 1) % 4;
                s_menu_item = 0;
            } else if (sc == SC_UP) {
                int count = fm_menu_item_count(s_menu_cat);
                do {
                    s_menu_item = (s_menu_item + count - 1) % count;
                } while (fm_menu_is_separator(s_menu_cat, s_menu_item));
            } else if (sc == SC_DOWN) {
                int count = fm_menu_item_count(s_menu_cat);
                do {
                    s_menu_item = (s_menu_item + 1) % count;
                } while (fm_menu_is_separator(s_menu_cat, s_menu_item));
            } else if (sc == SC_F9) {
                s_menu_active = 0;
            }
        } else {
            if (c == '\r' || c == '\n') {
                fm_menu_activate(s_menu_cat, s_menu_item);
            } else if (c == 0x1B) {
                s_menu_active = 0;
            }
        }
        return;
    }

    if (c == 0) {
        switch (sc) {
        case SC_UP:
            if (ap->cursor > 0) {
                ap->cursor--;
                if (ap->cursor < ap->top_index) ap->top_index = ap->cursor;
            }
            break;
        case SC_DOWN:
            if (ap->cursor < ap->file_count - 1) {
                ap->cursor++;
                if (ap->cursor >= ap->top_index + 18) ap->top_index = ap->cursor - 17;
            }
            break;
        case SC_LEFT:
            if (s_cmd_len > 0) {
                if (s_cmd_pos > 0) s_cmd_pos--;
            }
            break;
        case SC_RIGHT:
            if (s_cmd_len > 0) {
                if (s_cmd_pos < s_cmd_len) s_cmd_pos++;
            }
            break;
        case SC_HOME:
            if (s_cmd_len > 0) {
                s_cmd_pos = 0;
            } else {
                ap->cursor = 0;
                ap->top_index = 0;
            }
            break;
        case SC_END:
            if (s_cmd_len > 0) {
                s_cmd_pos = s_cmd_len;
            } else if (ap->file_count > 0) {
                ap->cursor = ap->file_count - 1;
                if (ap->cursor >= 18) ap->top_index = ap->cursor - 17;
            }
            break;
        case SC_PGUP:
            ap->cursor -= 18;
            if (ap->cursor < 0) ap->cursor = 0;
            ap->top_index = ap->cursor;
            break;
        case SC_PGDN:
            ap->cursor += 18;
            if (ap->cursor >= ap->file_count) ap->cursor = (ap->file_count > 0) ? ap->file_count - 1 : 0;
            if (ap->cursor >= ap->top_index + 18) ap->top_index = ap->cursor - 17;
            break;
        case SC_INS:
            if (ap->file_count > 0 && str_icmp(ap->files[ap->cursor].name, "..") != 0) {
                ap->files[ap->cursor].tagged = !ap->files[ap->cursor].tagged;
                if (ap->cursor < ap->file_count - 1) {
                    ap->cursor++;
                    if (ap->cursor >= ap->top_index + 18) ap->top_index = ap->cursor - 17;
                }
            }
            break;
        case SC_DEL:
            if (s_cmd_len > 0) {
                if (s_cmd_pos < s_cmd_len) {
                    int k;
                    for (k = s_cmd_pos; k < s_cmd_len; k++) {
                        s_cmd_buf[k] = s_cmd_buf[k + 1];
                    }
                    s_cmd_len--;
                }
            } else {
                fm_action_delete();
            }
            break;
        case SC_F1:
            fm_confirm_box("hobbyOS File Manager", "Tab: Switch | F2: Drive | F3: View | F4: Edit | F5: Copy | F8: Del");
            break;
        case SC_F2:
        case SC_ALT_F1:
            fm_drive_menu(s_active);
            break;
        case SC_ALT_F2:
            fm_drive_menu(1 - s_active);
            break;
        case SC_F3:
            fm_action_view();
            break;
        case SC_F4:
            fm_action_edit();
            break;
        case SC_F5:
            fm_action_copy();
            break;
        case SC_F6:
            fm_action_move();
            break;
        case SC_F7:
            fm_action_mkdir();
            break;
        case SC_F8:
            fm_action_delete();
            break;
        case SC_F9:
            s_menu_active = 1;
            s_menu_cat = 0;
            s_menu_item = 0;
            break;
        case SC_F10:
            s_running = 0;
            break;
        default:
            break;
        }
    } else {
        if (c == '\t') {
            s_active = 1 - s_active;
        } else if (c == '\r' || c == '\n') {
            if (s_cmd_len > 0) {
                fm_execute_cmd(s_cmd_buf);
                s_cmd_buf[0] = '\0';
                s_cmd_len = 0;
                s_cmd_pos = 0;
            } else {
                fm_action_enter();
            }
        } else if (c == 0x1B) {
            if (s_cmd_len > 0) {
                s_cmd_buf[0] = '\0';
                s_cmd_len = 0;
                s_cmd_pos = 0;
            }
        } else if (c == '\b') {
            if (s_cmd_len > 0 && s_cmd_pos > 0) {
                int k;
                for (k = s_cmd_pos - 1; k < s_cmd_len; k++) {
                    s_cmd_buf[k] = s_cmd_buf[k + 1];
                }
                s_cmd_pos--;
                s_cmd_len--;
            }
        } else if (s_cmd_len == 0 && c == '+') {
            fm_action_select_group(1);
        } else if (s_cmd_len == 0 && c == '-') {
            fm_action_select_group(0);
        } else if (s_cmd_len == 0 && c == '*') {
            fm_action_invert_selection();
        } else if (c >= 32 && c <= 126) {
            if (s_cmd_len < sizeof(s_cmd_buf) - 2) {
                int k;
                for (k = s_cmd_len; k >= s_cmd_pos; k--) {
                    s_cmd_buf[k + 1] = s_cmd_buf[k];
                }
                s_cmd_buf[s_cmd_pos] = (char)c;
                s_cmd_pos++;
                s_cmd_len++;
                s_cmd_buf[s_cmd_len] = '\0';
            }
        }
    }
}

static void fm_check_mouse(void) {
    uint16_t mx, my;
    uint16_t btn = os_mouse_get_pos(&mx, &my);
    int col = (int)(mx / 8);
    int row = (int)(my / 8);
    panel_t *ap = (s_active == 0) ? &s_left : &s_right;

    if (row != s_mouse_row || col != s_mouse_col) {
        fm_hide_cursor();
        fm_show_cursor(row, col);
        fm_flush();
    }

    if (btn & 1) { /* Left click */
        if (row == 0) {
            int i;
            for (i = 0; i < 4; i++) {
                if (col >= s_menu_headers[i].col_start && col <= s_menu_headers[i].col_end) {
                    if (s_menu_active && s_menu_cat == i) {
                        s_menu_active = 0;
                    } else {
                        s_menu_active = 1;
                        s_menu_cat = i;
                        s_menu_item = 0;
                    }
                    s_needs_redraw = 1;
                    break;
                }
            }
            if (col >= 68 && col <= 78) {
                s_menu_active = !s_menu_active;
                s_menu_cat = 0;
                s_menu_item = 0;
                s_needs_redraw = 1;
            }
        } else if (s_menu_active) {
            int c1, c2, r1 = 1, r2, num_items;
            if (s_menu_cat == 0) { c1 = 1; c2 = 26; num_items = 5; r2 = 7; }
            else if (s_menu_cat == 1) { c1 = 8; c2 = 33; num_items = 12; r2 = 14; }
            else if (s_menu_cat == 2) { c1 = 16; c2 = 41; num_items = 3; r2 = 5; }
            else { c1 = 23; c2 = 48; num_items = 5; r2 = 7; }

            if (row > r1 && row < r2 && col > c1 && col < c2) {
                int item = row - (r1 + 1);
                if (!fm_menu_is_separator(s_menu_cat, item)) {
                    fm_menu_activate(s_menu_cat, item);
                    s_needs_redraw = 1;
                }
            } else if (row > 0) {
                s_menu_active = 0;
                s_needs_redraw = 1;
            }
        } else if (row == 24) {
            /* Function bar clicked */
            int fn = (col / 8) + 1;
            switch (fn) {
            case 1: fm_confirm_box("hobbyOS File Manager", "Tab: Switch | F2: Drive | F3: View | F4: Edit | F5: Copy | F8: Del"); break;
            case 2: fm_drive_menu(s_active); break;
            case 3: fm_action_view(); break;
            case 4: fm_action_edit(); break;
            case 5: fm_action_copy(); break;
            case 6: fm_action_move(); break;
            case 7: fm_action_mkdir(); break;
            case 8: fm_action_delete(); break;
            case 9:
                s_menu_active = 1;
                s_menu_cat = 0;
                s_menu_item = 0;
                break;
            case 10: s_running = 0; break;
            }
            s_needs_redraw = 1;
        } else if (row == 23) {
            /* Click on live prompt */
            int plen = str_len(ap->path);
            int pstart;
            if (plen > 35) plen = 35;
            pstart = plen + 1;
            if (col >= pstart && col <= pstart + s_cmd_len) {
                s_cmd_pos = col - pstart;
                s_needs_redraw = 1;
            }
        } else if (row >= 3 && row <= 20) {
            /* Click inside panel */
            int is_left = (col < 40);
            panel_t *p = is_left ? &s_left : &s_right;
            int clicked_idx = p->top_index + (row - 3);

            if (is_left && s_active != 0) s_active = 0;
            else if (!is_left && s_active != 1) s_active = 1;

            if (clicked_idx < p->file_count) {
                file_item_t *it = &p->files[clicked_idx];
                volatile uint16_t __far *bda_ticks = (volatile uint16_t __far *)MK_FP(0x0040, 0x006C);
                uint16_t cur_tick = *bda_ticks;
                int is_double_click = (s_last_click_panel == is_left &&
                                       s_last_click_idx == clicked_idx &&
                                       (uint16_t)(cur_tick - s_last_click_tick) < 9);

                s_last_click_tick = cur_tick;
                s_last_click_idx = clicked_idx;
                s_last_click_panel = is_left;

                if (is_double_click) {
                    if (it->attr & OS_ATTR_DIRECTORY) {
                        fm_change_dir(p, it->name);
                    } else if (is_binary_name(it->name)) {
                        s_cmd_buf[0] = '\0';
                        s_cmd_len = 0;
                        s_cmd_pos = 0;
                        fm_action_enter();
                    }
                } else {
                    p->cursor = clicked_idx;
                    if (is_binary_name(it->name)) {
                        str_copy(s_cmd_buf, it->name);
                        str_cat(s_cmd_buf, " ");
                        s_cmd_len = str_len(s_cmd_buf);
                        s_cmd_pos = s_cmd_len;
                    }
                }
            }
            s_needs_redraw = 1;
        }

        /* Wait for button release, updating cursor position while dragging */
        while (os_mouse_get_pos(&mx, &my) & 1) {
            int cur_c = (int)(mx / 8);
            int cur_r = (int)(my / 8);
            if (cur_r != s_mouse_row || cur_c != s_mouse_col) {
                fm_hide_cursor();
                fm_show_cursor(cur_r, cur_c);
                fm_flush();
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Main Entry Point
 * -------------------------------------------------------------------------- */

int app_main(void) {
    char start_cwd[PATH_MAX_LEN];
    uint16_t dummy;

    os_cls(ATTR_DEFAULT);
    fm_init_colors();
    fm_invalidate_cache();

    /* Check for INT 0x33 mouse driver */
    s_mouse_avail = (os_mouse_reset(&dummy) != 0);

    /* Enumerate mounted volumes */
    fm_refresh_volumes();

    /* Initialize left panel with CWD */
    start_cwd[0] = '\0';
    if (os_getcwd((char __far *)FP_HERE(start_cwd), sizeof(start_cwd)) != 0 || start_cwd[0] == '\0') {
        str_copy(start_cwd, "SYS:/");
    }
    str_copy(s_left.path, start_cwd);
    s_left.sort_mode = 0;
    fm_load_dir(&s_left);

    /* Initialize right panel with another drive or SYS:/ */
    if (s_vol_count >= 2) {
        str_copy(s_right.path, s_volumes[1].name);
        str_cat(s_right.path, ":/");
    } else {
        str_copy(s_right.path, "SYS:/");
    }
    s_right.sort_mode = 0;
    fm_load_dir(&s_right);

    /* Initialize screen cache to invalidate */
    fm_invalidate_cache();

    os_set_cursor_visible(0);
    os_set_break_mode(BREAK_MODE_RAW);

    /* Main event loop */
    s_needs_redraw = 1;
    while (s_running) {
        if (s_needs_redraw) {
            fm_draw_screen();
            s_needs_redraw = 0;
        }

        if (os_kbhit()) {
            uint16_t key = os_getc();
            fm_handle_key(key);
            s_needs_redraw = 1;
        } else if (s_mouse_avail) {
            fm_check_mouse();
        }
    }

    os_set_cursor_visible(1);
    os_set_break_mode(BREAK_MODE_NORMAL);
    os_cls(ATTR_DEFAULT);

    return 0;
}
