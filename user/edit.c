/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EDIT.BIN - Lean nano-inspired full-screen text editor for hobbyOS.
 *
 * Text is kept in a gap buffer inside a 64 KB block obtained from the MCB
 * allocator: offsets 0..61439 hold the text (60 KB max), and the last 4 KB
 * hold the one-line cut/paste clipboard.
 */

#include "../include/hobbyos.h"

#define MK_FP(seg, off)  ((void __far *)(((unsigned long)(seg) << 16) | (uint16_t)(off)))

#define SCREEN_COLS  80
#define TITLE_ROW    0
#define TEXT_TOP     1
#define TEXT_ROWS    22
#define STATUS_ROW   23
#define HELP_ROW     24
#define TAB_W        4

#define BUF_PARAS    4096u     /* 64 KB block from the MCB allocator */
#define TEXT_CAP     61440u    /* 60 KB maximum text size */
#define CLIP_OFF     61440u    /* 4 KB clipboard after the text area */
#define CLIP_CAP     4096u

/* PC/XT Set 1 scancodes of the navigation keys (ASCII byte is 0) */
#define SC_HOME   0x47
#define SC_UP     0x48
#define SC_PGUP   0x49
#define SC_LEFT   0x4B
#define SC_RIGHT  0x4D
#define SC_END    0x4F
#define SC_DOWN   0x50
#define SC_PGDN   0x51
#define SC_DEL    0x53

#define CTRL(c)   ((uint8_t)((c) & 0x1F))
#define KEY_ESC   0x1B

static const char help_text[] =
    " ^O Save   ^X Exit   ^K Cut Line   ^U Paste   ^W Find   Esc Cancel prompt";

static uint8_t __far *buf;
static uint16_t gap_start;   /* gap occupies physical [gap_start, gap_end) */
static uint16_t gap_end;
static uint16_t cur;         /* cursor as a logical text offset */
static uint16_t cur_line;    /* 0-based line of the cursor */
static uint16_t top;         /* logical offset of the first displayed line */
static uint16_t top_line;
static uint16_t left_col;    /* horizontal scroll (visual columns) */
static uint16_t want_col;    /* visual column kept across up/down moves */
static uint16_t clip_len;
static uint8_t  modified;
static char     filename[64];
static char     search_term[40];
static char     message[64];
static char     row_buf[SCREEN_COLS + 1];

/* ------------------------------------------------------------------ */
/* Small string helpers (freestanding, no libc)                        */
/* ------------------------------------------------------------------ */

static uint16_t str_len(const char *s) {
    uint16_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void str_copy(char *dst, const char *src, uint16_t max) {
    uint16_t i = 0;
    while (src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint8_t to_lower(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
}

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (to_lower((uint8_t)*a) != to_lower((uint8_t)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static uint16_t utoa16(uint16_t v, char *out) {
    char tmp[6];
    uint16_t n = 0;
    uint16_t i;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';
    return n;
}

static void msg_set(const char *s) {
    str_copy(message, s, sizeof(message));
}

static void msg_add(const char *s) {
    uint16_t l = str_len(message);
    str_copy(message + l, s, (uint16_t)(sizeof(message) - l));
}

static void msg_add_uint(uint16_t v) {
    char t[6];
    utoa16(v, t);
    msg_add(t);
}

/* ------------------------------------------------------------------ */
/* Gap buffer                                                          */
/* ------------------------------------------------------------------ */

static uint16_t text_len(void) {
    return gap_start + (TEXT_CAP - gap_end);
}

static uint8_t char_at(uint16_t pos) {
    return (pos < gap_start) ? buf[pos] : buf[pos + (gap_end - gap_start)];
}

static void move_gap(uint16_t pos) {
    while (gap_start > pos) {
        buf[--gap_end] = buf[--gap_start];
    }
    while (gap_start < pos) {
        buf[gap_start++] = buf[gap_end++];
    }
}

static int insert_char(uint8_t c) {
    if (gap_start == gap_end) {
        msg_set("Buffer full (60 KB max)");
        return 0;
    }
    move_gap(cur);
    buf[gap_start++] = c;
    cur++;
    if (c == '\n') {
        cur_line++;
    }
    modified = 1;
    return 1;
}

static void delete_forward(void) {
    if (cur >= text_len()) {
        return;
    }
    move_gap(cur);
    gap_end++;
    modified = 1;
}

static void delete_backward(void) {
    if (cur == 0) {
        return;
    }
    move_gap(cur);
    gap_start--;
    cur--;
    if (buf[gap_start] == '\n') {
        cur_line--;
    }
    modified = 1;
}

/* ------------------------------------------------------------------ */
/* Line / column helpers                                               */
/* ------------------------------------------------------------------ */

static uint16_t line_start(uint16_t pos) {
    while (pos > 0 && char_at(pos - 1) != '\n') {
        pos--;
    }
    return pos;
}

static uint16_t line_end(uint16_t pos) {
    uint16_t n = text_len();
    while (pos < n && char_at(pos) != '\n') {
        pos++;
    }
    return pos;
}

static uint16_t tab_advance(uint16_t v) {
    return (uint16_t)(TAB_W - (v & (TAB_W - 1)));
}

static uint16_t visual_col(uint16_t from, uint16_t to) {
    uint16_t v = 0;
    while (from < to) {
        v += (char_at(from) == '\t') ? tab_advance(v) : 1;
        from++;
    }
    return v;
}

static uint16_t pos_for_col(uint16_t ls, uint16_t col) {
    uint16_t n   = text_len();
    uint16_t v   = 0;
    uint16_t pos = ls;
    while (pos < n) {
        uint8_t  c = char_at(pos);
        uint16_t w;
        if (c == '\n') {
            break;
        }
        w = (c == '\t') ? tab_advance(v) : 1;
        if (v + w > col) {
            break;
        }
        v += w;
        pos++;
    }
    return pos;
}

static void update_want_col(void) {
    want_col = visual_col(line_start(cur), cur);
}

/* ------------------------------------------------------------------ */
/* Cursor movement                                                     */
/* ------------------------------------------------------------------ */

static void move_left(void) {
    if (cur > 0) {
        cur--;
        if (char_at(cur) == '\n') {
            cur_line--;
        }
    }
}

static void move_right(void) {
    if (cur < text_len()) {
        if (char_at(cur) == '\n') {
            cur_line++;
        }
        cur++;
    }
}

static int move_up(void) {
    uint16_t ls = line_start(cur);
    if (ls == 0) {
        return 0;
    }
    cur = pos_for_col(line_start(ls - 1), want_col);
    cur_line--;
    return 1;
}

static int move_down(void) {
    uint16_t le = line_end(cur);
    if (le >= text_len()) {
        return 0;
    }
    cur = pos_for_col(le + 1, want_col);
    cur_line++;
    return 1;
}

static void page_down(void) {
    uint16_t i;
    for (i = 0; i < TEXT_ROWS; i++) {
        if (!move_down()) {
            break;
        }
        top = line_end(top) + 1;
        top_line++;
    }
}

static void page_up(void) {
    uint16_t i;
    for (i = 0; i < TEXT_ROWS; i++) {
        if (!move_up()) {
            break;
        }
        if (top > 0) {
            top = line_start(top - 1);
            top_line--;
        }
    }
}

static void goto_pos(uint16_t p) {
    while (cur < p) {
        if (char_at(cur) == '\n') {
            cur_line++;
        }
        cur++;
    }
    while (cur > p) {
        cur--;
        if (char_at(cur) == '\n') {
            cur_line--;
        }
    }
}

static void ensure_visible(void) {
    uint16_t vc;

    if (cur_line < top_line) {
        top      = line_start(cur);
        top_line = cur_line;
    }
    while (cur_line >= top_line + TEXT_ROWS) {
        top = line_end(top) + 1;
        top_line++;
    }

    vc = visual_col(line_start(cur), cur);
    if (vc < left_col) {
        left_col = vc;
    } else if (vc >= left_col + SCREEN_COLS) {
        left_col = vc - (SCREEN_COLS - 1);
    }
}

/* ------------------------------------------------------------------ */
/* Screen drawing                                                      */
/* ------------------------------------------------------------------ */

/* Write a padded row; the bottom row stops at column 78 so the console never scrolls */
static void put_row(uint8_t row, const char *s) {
    uint8_t width = (row == HELP_ROW) ? (SCREEN_COLS - 1) : SCREEN_COLS;
    uint8_t i = 0;

    while (i < width && s[i]) {
        row_buf[i] = s[i];
        i++;
    }
    while (i < width) {
        row_buf[i++] = ' ';
    }
    row_buf[width] = '\0';
    os_set_cursor(row, 0);
    os_puts(row_buf);
}

static void draw_title(void) {
    static const char app[] = " hobbyOS EDIT";
    static const char mod[] = "Modified";
    char t[SCREEN_COLS + 1];
    const char *name = filename[0] ? filename : "New Buffer";
    uint16_t nl = str_len(name);
    uint16_t i, start;

    for (i = 0; i < SCREEN_COLS; i++) {
        t[i] = ' ';
    }
    t[SCREEN_COLS] = '\0';
    for (i = 0; app[i]; i++) {
        t[i] = app[i];
    }
    if (nl > 50) {
        nl = 50;
    }
    start = (SCREEN_COLS - nl) / 2;
    for (i = 0; i < nl; i++) {
        t[start + i] = name[i];
    }
    if (modified) {
        for (i = 0; mod[i]; i++) {
            t[71 + i] = mod[i];
        }
    }
    put_row(TITLE_ROW, t);
}

static void draw_text(void) {
    uint16_t n    = text_len();
    uint16_t pos  = top;
    uint8_t  more = 1;
    uint8_t  r;

    for (r = 0; r < TEXT_ROWS; r++) {
        uint16_t i;
        uint16_t v = 0;

        for (i = 0; i < SCREEN_COLS; i++) {
            row_buf[i] = ' ';
        }
        row_buf[SCREEN_COLS] = '\0';

        if (more) {
            while (pos < n) {
                uint8_t  c = char_at(pos);
                uint16_t w = 1;
                if (c == '\n') {
                    break;
                }
                if (c == '\t') {
                    w = tab_advance(v);
                    c = ' ';
                } else if (c < 0x20 || c == 0x7F) {
                    c = '?';
                }
                while (w--) {
                    if (v >= left_col && v - left_col < SCREEN_COLS) {
                        row_buf[v - left_col] = (char)c;
                    }
                    v++;
                }
                pos++;
            }
            if (pos < n) {
                pos++;          /* skip '\n' */
            } else {
                more = 0;       /* rows below the last line stay blank */
            }
        }

        os_set_cursor((uint8_t)(TEXT_TOP + r), 0);
        os_puts(row_buf);
    }
}

static void draw_status(uint16_t vc) {
    char s[SCREEN_COLS + 1];
    char info[24];
    uint16_t i, l;

    for (i = 0; i < SCREEN_COLS; i++) {
        s[i] = ' ';
    }
    s[SCREEN_COLS] = '\0';

    l = str_len(message);
    if (l > 56) {
        l = 56;
    }
    for (i = 0; i < l; i++) {
        s[1 + i] = message[i];
    }

    str_copy(info, "Ln ", sizeof(info));
    l = str_len(info);
    l += utoa16(cur_line + 1, info + l);
    str_copy(info + l, ", Col ", (uint16_t)(sizeof(info) - l));
    l = str_len(info);
    utoa16(vc + 1, info + l);
    l = str_len(info);
    for (i = 0; i < l; i++) {
        s[(SCREEN_COLS - 1 - l) + i] = info[i];
    }
    put_row(STATUS_ROW, s);
}

static void redraw(void) {
    uint16_t vc = visual_col(line_start(cur), cur);
    os_set_cursor_visible(0);
    draw_title();
    draw_text();
    draw_status(vc);
    os_set_cursor((uint8_t)(TEXT_TOP + (cur_line - top_line)), (uint8_t)(vc - left_col));
    os_set_cursor_visible(1);
}

/* ------------------------------------------------------------------ */
/* Status-line prompts                                                 */
/* ------------------------------------------------------------------ */

/* Edit `text` on the status line. Returns 1 on Enter, 0 on Esc / ^C. */
static int prompt(const char *label, char *text, uint16_t max) {
    uint16_t len = str_len(text);
    uint16_t lab = str_len(label);

    for (;;) {
        char     line[SCREEN_COLS + 1];
        uint16_t i;
        uint16_t k = 0;
        uint8_t  c;

        for (i = 0; i < lab && k < SCREEN_COLS - 1; i++) {
            line[k++] = label[i];
        }
        for (i = 0; i < len && k < SCREEN_COLS - 1; i++) {
            line[k++] = text[i];
        }
        line[k] = '\0';
        put_row(STATUS_ROW, line);
        os_set_cursor(STATUS_ROW, (uint8_t)k);

        c = (uint8_t)(os_getc() & 0xFF);
        if (c == '\r') {
            return 1;
        }
        if (c == KEY_ESC || c == CTRL('C')) {
            return 0;
        }
        if (c == '\b') {
            if (len > 0) {
                text[--len] = '\0';
            }
        } else if (c >= 0x20 && c != 0x7F && len + 1 < max) {
            text[len++] = (char)c;
            text[len]   = '\0';
        }
    }
}

/* Returns 'y', 'n', or 0 when cancelled with Esc / ^C */
static uint8_t ask_yn(const char *question) {
    uint16_t l = str_len(question);

    put_row(STATUS_ROW, question);
    os_set_cursor(STATUS_ROW, (uint8_t)((l < SCREEN_COLS - 1) ? l : SCREEN_COLS - 1));
    for (;;) {
        uint8_t c = to_lower((uint8_t)(os_getc() & 0xFF));
        if (c == 'y' || c == 'n') {
            return c;
        }
        if (c == KEY_ESC || c == CTRL('C')) {
            return 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int do_save(void) {
    char     name[64];
    uint16_t n;
    int      res;

    str_copy(name, filename, sizeof(name));
    if (!prompt("File Name to Write: ", name, sizeof(name)) || name[0] == '\0') {
        msg_set("Cancelled");
        return 0;
    }
    if (!str_ieq(name, filename) && os_file_size(name) >= 0) {
        if (ask_yn("File exists, overwrite? (Y/N) ") != 'y') {
            msg_set("Cancelled");
            return 0;
        }
    }

    n = text_len();
    move_gap(n);                         /* make the text contiguous at offset 0 */
    res = os_file_write(name, (const void __far *)buf, n);
    if ((uint16_t)res != n) {
        msg_set("Error writing ");
        msg_add(name);
        return 0;
    }

    str_copy(filename, name, sizeof(filename));
    modified = 0;
    msg_set("Wrote ");
    msg_add_uint(n);
    msg_add(" bytes");
    return 1;
}

/* Returns 1 if the editor should exit */
static int confirm_exit(void) {
    uint8_t answer;

    if (!modified) {
        return 1;
    }
    answer = ask_yn("Save modified buffer? (Y/N, Esc cancels) ");
    if (answer == 0) {
        msg_set("Cancelled");
        return 0;
    }
    if (answer == 'y') {
        return do_save();
    }
    return 1;
}

static void do_cut(void) {
    uint16_t ls = line_start(cur);
    uint16_t le = line_end(cur);
    uint16_t n  = text_len();
    uint16_t end = (le < n) ? (le + 1) : le;
    uint16_t i;

    if (end == ls) {
        msg_set("Nothing to cut");
        return;
    }
    if (end - ls > CLIP_CAP - 1) {
        msg_set("Line too long to cut (4 KB max)");
        return;
    }

    for (i = 0; ls + i < end; i++) {
        buf[CLIP_OFF + i] = char_at(ls + i);
    }
    clip_len = end - ls;
    if (buf[CLIP_OFF + clip_len - 1] != '\n') {
        buf[CLIP_OFF + clip_len] = '\n';   /* last line: paste always yields a full line */
        clip_len++;
    }

    move_gap(ls);
    gap_end += end - ls;
    cur = ls;
    modified = 1;
}

static void do_paste(void) {
    uint16_t i;

    if (clip_len == 0) {
        msg_set("Nothing to paste");
        return;
    }
    if (gap_end - gap_start < clip_len) {
        msg_set("Buffer full (60 KB max)");
        return;
    }
    for (i = 0; i < clip_len; i++) {
        insert_char(buf[CLIP_OFF + i]);
    }
}

static int match_at(uint16_t p, const char *t, uint16_t tl) {
    uint16_t i;
    for (i = 0; i < tl; i++) {
        if (to_lower(char_at(p + i)) != to_lower((uint8_t)t[i])) {
            return 0;
        }
    }
    return 1;
}

static void do_find(void) {
    char     term[40];
    uint16_t tl, n, last, p, i;
    uint16_t found   = 0xFFFF;
    uint8_t  wrapped = 0;

    str_copy(term, search_term, sizeof(term));
    if (!prompt("Search: ", term, sizeof(term)) || term[0] == '\0') {
        msg_set("Cancelled");
        return;
    }
    str_copy(search_term, term, sizeof(search_term));

    tl = str_len(term);
    n  = text_len();
    if (tl <= n) {
        last = n - tl;
        for (p = cur + 1; p <= last; p++) {
            if (match_at(p, term, tl)) {
                found = p;
                break;
            }
        }
        if (found == 0xFFFF) {
            for (p = 0; p <= cur && p <= last; p++) {
                if (match_at(p, term, tl)) {
                    found   = p;
                    wrapped = 1;
                    break;
                }
            }
        }
    }

    if (found == 0xFFFF) {
        msg_set("\"");
        msg_add(term);
        msg_add("\" not found");
        return;
    }

    goto_pos(found);
    if (cur_line < top_line || cur_line >= top_line + TEXT_ROWS) {
        top      = line_start(cur);
        top_line = cur_line;
        for (i = 0; i < TEXT_ROWS / 2 && top > 0; i++) {
            top = line_start(top - 1);
            top_line--;
        }
    }
    update_want_col();
    if (wrapped) {
        msg_set("Search wrapped");
    }
}

/* ------------------------------------------------------------------ */
/* File loading                                                        */
/* ------------------------------------------------------------------ */

static int load_file(uint16_t seg) {
    long     sz = os_file_size(filename);
    uint16_t n, i, j;

    if (sz < 0) {
        msg_set("New File");
        return 0;
    }
    if (sz > (long)TEXT_CAP) {
        os_puts("edit: file too large (60 KB max)\n");
        return -1;
    }

    /* Read the file straight into the tail of the buffer: the gap starts empty at offset 0 */
    n = (uint16_t)sz;
    if (n > 0 && (uint16_t)os_file_read(filename, MK_FP(seg, TEXT_CAP - n), n) != n) {
        os_puts("edit: cannot read ");
        os_puts(filename);
        os_putc('\n');
        return -1;
    }
    gap_end = TEXT_CAP - n;

    /* Strip DOS carriage returns (CR LF -> LF) */
    j = TEXT_CAP;
    for (i = TEXT_CAP; i > gap_end; ) {
        i--;
        if (buf[i] != '\r') {
            buf[--j] = buf[i];
        }
    }
    gap_end = j;

    msg_set("Read ");
    msg_add_uint(text_len());
    msg_add(" bytes");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

int app_main(void) {
    const char *args = os_get_args();
    uint16_t    seg;
    uint16_t    i = 0;
    int         running = 1;

    while (args && *args == ' ') {
        args++;
    }
    while (args && args[i] && args[i] != ' ' && i < sizeof(filename) - 1) {
        filename[i] = args[i];
        i++;
    }
    filename[i] = '\0';

    seg = os_mem_alloc(BUF_PARAS);
    if (seg == 0) {
        os_puts("edit: not enough memory (64 KB needed)\n");
        return 1;
    }
    buf       = (uint8_t __far *)MK_FP(seg, 0);
    gap_start = 0;
    gap_end   = TEXT_CAP;
    cur = cur_line = top = top_line = left_col = want_col = clip_len = 0;
    modified       = 0;
    message[0]     = '\0';
    search_term[0] = '\0';

    if (filename[0] && load_file(seg) != 0) {
        os_mem_free(seg);
        return 1;
    }

    os_cls(ATTR_DEFAULT);
    put_row(HELP_ROW, help_text);

    while (running) {
        uint16_t key;
        uint8_t  c, sc;

        ensure_visible();
        if (!os_kbhit()) {          /* skip redraws while typed-ahead keys are pending */
            redraw();
        }

        key = os_getc();
        c   = (uint8_t)(key & 0xFF);
        sc  = (uint8_t)(key >> 8);
        message[0] = '\0';

        if (c == 0) {
            switch (sc) {
            case SC_LEFT:  move_left();  update_want_col(); break;
            case SC_RIGHT: move_right(); update_want_col(); break;
            case SC_UP:    move_up();    break;
            case SC_DOWN:  move_down();  break;
            case SC_PGUP:  page_up();    break;
            case SC_PGDN:  page_down();  break;
            case SC_HOME:  cur = line_start(cur); update_want_col(); break;
            case SC_END:   cur = line_end(cur);   update_want_col(); break;
            case SC_DEL:   delete_forward();      update_want_col(); break;
            default: break;
            }
        } else if (c == '\b') {
            delete_backward();
            update_want_col();
        } else if (c == '\r') {
            insert_char('\n');
            update_want_col();
        } else if (c == '\t') {
            insert_char('\t');
            update_want_col();
        } else if (c == CTRL('O')) {
            do_save();
        } else if (c == CTRL('X')) {
            running = !confirm_exit();
        } else if (c == CTRL('K')) {
            do_cut();
            update_want_col();
        } else if (c == CTRL('U')) {
            do_paste();
            update_want_col();
        } else if (c == CTRL('W')) {
            do_find();
        } else if (c >= 0x20 && c != 0x7F) {
            insert_char(c);
            update_want_col();
        }
    }

    os_cls(ATTR_DEFAULT);
    os_mem_free(seg);
    return 0;
}
