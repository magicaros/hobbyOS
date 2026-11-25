// Copyright (C) 2026 Benoit Colson
// SPDX-License-Identifier: GPL-3.0-or-later
//
// hobbyOS Native BASIC Interpreter (GW-BASIC Compatible)
// Phase 1: Interactive REPL, Line Storage, Tokenizer, Lister & Basic Commands
// Phase 2: Variables (%, $), Expressions & Math Parser, Control Flow (LET, INPUT, IF/THEN/ELSE, GOTO, GOSUB/RETURN)
// Phase 3: Loops (FOR/NEXT, WHILE/WEND), Arrays (DIM, 1D/2D), Data (DATA/READ/RESTORE), Screen (LOCATE, SWAP)
// Phase 4: File I/O & Persistence (SAVE, LOAD, FILES, OPEN, CLOSE, PRINT#, WRITE#, INPUT#, LINE INPUT, EOF)

#include "hobbyos.h"
#include "basic_tokens.h"
#include "basic_math.h"

#define FP_HERE(ptr)        ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))
#define BASIC_SEG_PARAS     4096    /* 64 KB program segment */
#define BASIC_MAX_LINE_LEN  256
#define BASIC_PROMPT        "Ok\n"

#define BASIC_MAX_VARS      48
#define BASIC_MAX_VARNAME   16
#define BASIC_MAX_STR_LEN   64
#define BASIC_NUM_TEMP_STRS 6

#define BASIC_MAX_GOSUB     16
#define BASIC_MAX_FOR_NEST  8
#define BASIC_MAX_WHILE_NEST 8
#define BASIC_MAX_ARRAYS    16
#define BASIC_STR_ELEM_LEN  32

#define BASIC_MAX_FILES     4
#define BASIC_FILE_BUF_SIZE 256

#define BASIC_TYPE_INT      1
#define BASIC_TYPE_STR      2
#define BASIC_TYPE_UINT     3
#define BASIC_TYPE_FLOAT    4

/* Operator comparison codes */
#define OP_CMP_NONE         0
#define OP_CMP_EQ           1
#define OP_CMP_NE           2
#define OP_CMP_LT           3
#define OP_CMP_GT           4
#define OP_CMP_LE           5
#define OP_CMP_GE           6

/* Value structure for expressions */
typedef struct {
    uint8_t       type;       /* BASIC_TYPE_INT, BASIC_TYPE_STR, BASIC_TYPE_UINT, BASIC_TYPE_FLOAT */
    int16_t       ival;       /* Integer value */
    basic_float_t fval;       /* 32-bit float value */
    char          *sval;      /* Pointer to string (in DS) */
} basic_val_t;

/* Variable structure */
typedef struct {
    char          name[BASIC_MAX_VARNAME];
    uint8_t       type;
    int16_t       ival;
    basic_float_t fval;
    char          sval[BASIC_MAX_STR_LEN + 1];
    uint16_t      slen;
} basic_var_t;

/* Array descriptor structure */
typedef struct {
    char     name[BASIC_MAX_VARNAME];
    uint8_t  type;      /* BASIC_TYPE_INT or BASIC_TYPE_STR */
    uint8_t  num_dims;  /* 1 or 2 */
    uint16_t dim1;      /* e.g. 11 for 0..10 */
    uint16_t dim2;      /* e.g. 1 for 1D, or dim2 for 2D */
    uint16_t data_off;  /* Offset in s_basic_seg */
} basic_array_t;

#define BASIC_MAX_FIELDS    16
#define BASIC_MAX_REC_LEN   512

/* FIELD mapping descriptor for random-access files */
typedef struct {
    char     var_name[BASIC_MAX_VARNAME];
    uint16_t offset;
    uint16_t len;
} basic_field_t;

/* File handle structure */
typedef struct {
    int           is_open;
    uint8_t       mode;      /* 'I', 'O', 'A', 'R' */
    char          filename[32];
    uint32_t      file_pos;
    uint32_t      file_len;
    uint8_t       buf[BASIC_FILE_BUF_SIZE];
    uint16_t      buf_pos;
    uint16_t      buf_len;
    uint8_t       eof;
    /* Phase 11: Random-access file support */
    uint16_t      record_len;
    uint32_t      current_record;
    uint8_t       rec_buf[BASIC_MAX_REC_LEN];
    basic_field_t fields[BASIC_MAX_FIELDS];
    uint8_t       num_fields;
} basic_file_t;

/* Call stack frame for GOSUB / RETURN */
typedef struct {
    uint16_t line_off;
    uint16_t stmt_pos;
} basic_gosub_frame_t;

/* Loop stack frame for FOR / NEXT */
typedef struct {
    char     var_name[BASIC_MAX_VARNAME];
    int16_t  end_val;
    int16_t  step_val;
    uint16_t line_off;
    uint16_t stmt_pos;
} basic_for_frame_t;

/* Loop stack frame for WHILE / WEND */
typedef struct {
    uint16_t while_line_off;
    uint16_t while_stmt_pos;
} basic_while_frame_t;

/* --------------------------------------------------------------------------
 * Global Interpreter State
 * -------------------------------------------------------------------------- */

/* Program storage in allocated segment */
static uint16_t s_basic_seg         = 0;
static uint16_t s_prog_head         = 0;    /* Offset of first line (0 = empty) */
static uint16_t s_prog_tail         = 0;    /* End of allocated program memory */
static int      s_running           = 0;
static uint16_t s_cur_line_off      = 0;
static uint16_t s_cur_line_num      = 0;
static uint16_t s_pc_line           = 0;
static uint16_t s_pc_stmt           = 0;
static int      s_jumped            = 0;

/* Error handling state (Phase 9) */
static uint16_t s_error_goto_line   = 0;
static int16_t  s_err               = 0;
static uint16_t s_erl               = 0;
static int      s_in_error_handler  = 0;
static int      s_err_reported      = 0;
static uint16_t s_err_line_off      = 0;
static uint16_t s_err_stmt_pos      = 0;
static uint16_t s_err_next_line     = 0;
static uint16_t s_err_next_stmt     = 0;
static uint16_t s_cur_stmt_pos      = 0;
static uint16_t s_cur_next_line     = 0;
static uint16_t s_cur_next_stmt     = 0;

/* Event trapping state (Phase 10) */
typedef struct {
    uint16_t line;       /* Target line to GOSUB */
    uint8_t  state;      /* 0 = OFF, 1 = ON, 2 = STOP */
    uint8_t  pending;    /* 1 if event occurred while STOP/busy */
    uint8_t  in_handler; /* 1 if currently in handler */
} basic_event_trap_t;

static uint16_t           s_timer_interval    = 0;
static uint16_t           s_timer_line        = 0;
static uint8_t            s_timer_state       = 0;  /* 0 = OFF, 1 = ON, 2 = STOP */
static uint8_t            s_timer_pending     = 0;
static uint8_t            s_timer_in_handler  = 0;
static uint32_t           s_timer_target_tick = 0;

#define BASIC_MAX_TRAP_KEYS 15
static basic_event_trap_t s_key_trap[BASIC_MAX_TRAP_KEYS];
static char               s_fkey_strings[11][16];
static uint8_t            s_key_visible       = 0;

#define BASIC_KEY_FIFO_SIZE 64
static uint8_t            s_key_fifo[BASIC_KEY_FIFO_SIZE];
static uint8_t            s_key_fifo_head     = 0;
static uint8_t            s_key_fifo_count    = 0;
static uint8_t            s_break_pending     = 0;

/* Variable table */
static basic_var_t s_vars[BASIC_MAX_VARS];
static uint16_t    s_num_vars       = 0;

/* Array table and heap in s_basic_seg */
static basic_array_t s_arrays[BASIC_MAX_ARRAYS];
static uint16_t      s_num_arrays   = 0;
static uint16_t      s_array_heap_start = 0;
static uint16_t      s_array_heap_free  = 0;

/* Temporary string pool (in DS) */
static char     s_temp_strs[BASIC_NUM_TEMP_STRS][BASIC_MAX_STR_LEN + 1];
static uint8_t  s_temp_str_idx      = 0;

/* GOSUB stack */
static basic_gosub_frame_t s_gosub_stack[BASIC_MAX_GOSUB];
static uint16_t            s_gosub_sp       = 0;

/* FOR stack */
static basic_for_frame_t s_for_stack[BASIC_MAX_FOR_NEST];
static uint16_t          s_for_sp           = 0;

/* WHILE stack */
static basic_while_frame_t s_while_stack[BASIC_MAX_WHILE_NEST];
static uint16_t            s_while_sp       = 0;

/* DATA read cursor */
static uint16_t s_data_line_off     = 0;
static uint16_t s_data_stmt_pos     = 0;

/* File handles (#1..#4) */
static basic_file_t s_files[BASIC_MAX_FILES];

/* Random number seed */
static uint16_t s_rnd_seed          = 0x1234;

/* Active memory segment for PEEK/POKE/DEF SEG (Phase 7) */
static uint16_t s_def_seg           = 0;

/* Phase 12 State Variables */
static uint8_t  s_tron              = 0;
static uint8_t  s_can_cont          = 0;
static uint16_t s_cont_line_off     = 0;
static uint16_t s_cont_stmt_pos     = 4;
static uint8_t  s_auto_mode         = 0;
static uint16_t s_auto_line         = 10;
static uint16_t s_auto_step         = 10;
static uint8_t  s_date_month        = 1;
static uint8_t  s_date_day          = 1;
static uint16_t s_date_year         = 2026;

#ifdef __WATCOMC__
static uint8_t inb(uint16_t port);
#pragma aux inb = \
    "in al, dx" \
    parm [dx] value [al] modify [];

static void outb(uint16_t port, uint8_t val);
#pragma aux outb = \
    "out dx, al" \
    parm [dx] [al] modify [];
#else
static uint8_t inb(uint16_t port) { (void)port; return 0; }
static void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
#endif

/* Graphics and Sound state (Phase 5, 13) */
static uint8_t  s_screen_mode       = 0;    /* 0 = text 80x25, 1 = CGA 320x200, 2 = CGA 640x200, 12 = VGA 640x480, 13 = VGA 320x200 */
static uint8_t  s_text_fg           = 7;
static uint8_t  s_text_bg           = 0;
static uint8_t  s_text_attr         = 0x07;
static uint8_t  s_cga_bg            = 0;
static uint8_t  s_cga_palette       = 1;
static uint8_t  s_gfx_fg            = 15;
static int16_t  s_last_x            = 0;
static int16_t  s_last_y            = 0;
static uint8_t  s_gfx_row           = 0;
static uint8_t  s_gfx_col           = 0;
static uint8_t  s_draw_angle        = 0;    /* 0 = 0 deg, 1 = 90 deg, 2 = 180 deg, 3 = 270 deg */
static uint8_t  s_draw_scale        = 4;    /* scale / 4 (default 4 = 1:1) */
static uint8_t  s_draw_color        = 15;
static uint8_t  s_play_octave       = 4;
static uint8_t  s_play_length       = 4;
static uint8_t  s_play_tempo        = 120;
static uint8_t  s_play_style        = 0;    /* 0 = normal (7/8), 1 = legato (8/8), 2 = staccato (6/8) */

/* Forward declarations */
static int basic_eval_expr(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val);
static int basic_exec_statement(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_mid_stmt(const uint8_t __far *tok, uint16_t *pos);
static int basic_error(uint8_t code, const char *msg);
static uint16_t basic_find_line(uint16_t line_num);
static int basic_skip_to_next(const uint8_t __far *tok, uint16_t *pos, const char *var_name);
static int basic_skip_to_wend(const uint8_t __far *tok, uint16_t *pos);
static void basic_close_all_files(void);
static void basic_cmd_run(void);
static int basic_insert_line(uint16_t line_num, const uint8_t *tok_data, uint16_t tok_len);
static uint16_t basic_tokenize(const char *in, uint8_t *out, uint16_t max_out);
static int basic_exec_key(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_timer(const uint8_t __far *tok, uint16_t *pos);
static void basic_poll_events(void);
static void basic_poll_keyboard(void);
static uint16_t basic_getc(void);
static int basic_kbhit(void);
static void basic_init_fkeys(void);
static void basic_display_softkeys(void);
static void basic_clear_softkeys(void);
static void basic_gfx_print_str(const char *str, uint8_t color);
static void basic_term_putc(char c);
static void basic_term_puts(const char *s);

/* Phase 12 Forward Declarations */
static void basic_new(void);
static void basic_run_from_pc(void);
static int basic_cmd_cont(void);
static int basic_cmd_delete(uint16_t start, uint16_t end);
static int basic_cmd_renum(uint16_t new_start, uint16_t old_start, uint16_t step);
static uint16_t basic_detokenize_tokens(const uint8_t __far *tok_data, char *out_buf, uint16_t max_out);
static void basic_get_date_str(char *buf);
static void basic_get_time_str(char *buf);
static int basic_set_date_str(const char *s);
static int basic_set_time_str(const char *s);
static uint8_t bios_read_pixel(uint16_t x, uint16_t y);
static int basic_exec_bsave(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_bload(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_width(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_delete(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_auto(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_renum(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_kill(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_name(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_chdir(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_mkdir(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_rmdir(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_shell(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_date_stmt(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_time_stmt(const uint8_t __far *tok, uint16_t *pos);

/* Phase 14 Forward Declarations */
static int basic_exec_paint(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_get_gfx(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_put_gfx(const uint8_t __far *tok, uint16_t *pos);
static int basic_exec_draw(const uint8_t __far *tok, uint16_t *pos);

/* --------------------------------------------------------------------------
 * Helper String & Math Functions
 * -------------------------------------------------------------------------- */

static int basic_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int basic_is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static char basic_toupper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static uint16_t basic_strlen(const char *s) {
    uint16_t len = 0;
    if (!s) return 0;
    while (s[len] != '\0') len++;
    return len;
}

static void basic_strcpy(char *dst, const char *src) {
    uint16_t i = 0;
    while (src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void basic_strncpy(char *dst, const char *src, uint16_t n) {
    uint16_t i = 0;
    while (i < n && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    while (i < n) {
        dst[i++] = '\0';
    }
}

static void basic_strcat(char *dst, const char *src, uint16_t max_dst) {
    uint16_t dlen = basic_strlen(dst);
    uint16_t i = 0;
    while (src[i] != '\0' && dlen + 1 < max_dst) {
        dst[dlen++] = src[i++];
    }
    dst[dlen] = '\0';
}

static int basic_strcmp(const char *s1, const char *s2) {
    uint16_t i = 0;
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) return (int)(uint8_t)s1[i] - (int)(uint8_t)s2[i];
        i++;
    }
    return (int)(uint8_t)s1[i] - (int)(uint8_t)s2[i];
}

static int basic_stricmp(const char *s1, const char *s2) {
    uint16_t i = 0;
    for (;;) {
        char c1 = basic_toupper(s1[i]);
        char c2 = basic_toupper(s2[i]);
        if (c1 != c2) return (int)(uint8_t)c1 - (int)(uint8_t)c2;
        if (c1 == '\0') return 0;
        i++;
    }
}

static int basic_strnicmp(const char *s1, const char *s2, uint16_t n) {
    uint16_t i;
    for (i = 0; i < n; i++) {
        char c1 = basic_toupper(s1[i]);
        char c2 = basic_toupper(s2[i]);
        if (c1 != c2) return (int)(uint8_t)c1 - (int)(uint8_t)c2;
        if (c1 == '\0') break;
    }
    return 0;
}

static int basic_strnicmp_far(const uint8_t __far *s1, const char *s2, uint16_t n) {
    uint16_t i;
    for (i = 0; i < n; i++) {
        char c1 = basic_toupper((char)s1[i]);
        char c2 = basic_toupper(s2[i]);
        if (c1 != c2) return (int)(uint8_t)c1 - (int)(uint8_t)c2;
        if (c1 == '\0') break;
    }
    return 0;
}

static void basic_memcpy(void *dest, const void *src, uint16_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    uint16_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static void basic_memset(void *dest, uint8_t val, uint16_t n) {
    uint8_t *d = (uint8_t *)dest;
    uint16_t i;
    for (i = 0; i < n; i++) d[i] = val;
}

static int16_t basic_atoi(const char *s) {
    int16_t val = 0;
    int sign = 1;
    uint16_t i = 0;
    if (!s) return 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == '-') { sign = -1; i++; }
    else if (s[i] == '+') { i++; }
    while (basic_is_digit(s[i])) {
        val = (val * 10) + (int16_t)(s[i] - '0');
        i++;
    }
    return val * sign;
}

static void basic_gfx_print_str(const char *str, uint8_t color) {
    uint16_t max_cols = (s_screen_mode == 2 || s_screen_mode == 12) ? 80 : 40;
    uint16_t max_rows = (s_screen_mode == 12) ? 60 : 25;
    while (*str) {
        char c = *str++;
        if (c == '\r') {
            s_gfx_col = 0;
            continue;
        }
        if (c == '\n') {
            s_gfx_col = 0;
            s_gfx_row++;
            if (s_gfx_row >= max_rows) s_gfx_row = 0;
            continue;
        }
        if (c == '\b') {
            if (s_gfx_col > 0) {
                s_gfx_col--;
                os_gfx_rect((uint16_t)s_gfx_col * 8, (uint16_t)s_gfx_row * 8, 8, 8, 0, 1);
            }
            continue;
        }
        if (s_gfx_col >= max_cols) {
            s_gfx_col = 0;
            s_gfx_row++;
            if (s_gfx_row >= max_rows) s_gfx_row = 0;
        }
        {
            static char single[2];
            single[0] = c;
            single[1] = '\0';
            os_gfx_rect((uint16_t)s_gfx_col * 8, (uint16_t)s_gfx_row * 8, 8, 8, 0, 1);
            os_gfx_text((uint16_t)s_gfx_col * 8, (uint16_t)s_gfx_row * 8, (const char __far *)FP_HERE(single), color);
        }
        s_gfx_col++;
    }
}

static void basic_term_putc(char c) {
    if (s_screen_mode == 0) {
        (os_putc)(c);
    } else {
        static char s[2];
        s[0] = c;
        s[1] = '\0';
        basic_gfx_print_str(s, (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1));
    }
}

static void basic_term_puts(const char *s) {
    if (!s) return;
    if (s_screen_mode == 0) {
        (os_puts)(s);
    } else {
        basic_gfx_print_str(s, (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1));
    }
}

#define os_puts basic_term_puts
#define os_putc basic_term_putc

static void basic_print_uint(uint16_t val) {
    char buf[8];
    int p = 0;
    if (val == 0) {
        basic_term_putc('0');
        return;
    }
    while (val > 0) {
        buf[p++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (p > 0) {
        basic_term_putc(buf[--p]);
    }
}

static void basic_print_int(int16_t val) {
    if (val < 0) {
        basic_term_putc('-');
        if (val == -32768) {
            basic_term_puts("32768");
            return;
        }
        val = -val;
    }
    basic_print_uint((uint16_t)val);
}

static uint16_t basic_parse_uint(const char *s, uint16_t *out_idx) {
    uint16_t val = 0;
    uint16_t i = *out_idx;
    while (basic_is_digit(s[i])) {
        val = (val * 10) + (uint16_t)(s[i] - '0');
        i++;
    }
    *out_idx = i;
    return val;
}

static uint16_t basic_parse_uint_far(const uint8_t __far *s, uint16_t *out_idx) {
    uint16_t val = 0;
    uint16_t i = *out_idx;
    while (basic_is_digit((char)s[i])) {
        val = (val * 10) + (uint16_t)(s[i] - '0');
        i++;
    }
    *out_idx = i;
    return val;
}

static char *basic_alloc_temp_str(void) {
    char *buf = s_temp_strs[s_temp_str_idx];
    s_temp_str_idx = (uint8_t)((s_temp_str_idx + 1) % BASIC_NUM_TEMP_STRS);
    buf[0] = '\0';
    return buf;
}

static int16_t basic_rand(void) {
    s_rnd_seed = (uint16_t)((s_rnd_seed * 25173U) + 13849U);
    return (int16_t)(s_rnd_seed & 0x7FFF);
}

static int basic_wildcard_match(const char *pat, const char *str) {
    while (*pat != '\0') {
        if (*pat == '*') {
            pat++;
            if (*pat == '\0') return 1;
            while (*str != '\0') {
                if (basic_wildcard_match(pat, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pat == '?' || basic_toupper(*pat) == basic_toupper(*str)) {
            if (*str == '\0') return 0;
            pat++;
            str++;
        } else {
            return 0;
        }
    }
    return (*str == '\0');
}

static void basic_normalize_filename(char *dst, const char *src, uint16_t max_len) {
    uint16_t i = 0;
    int has_dot = 0;
    while (src[i] != '\0' && i + 1 < max_len) {
        dst[i] = basic_toupper(src[i]);
        if (dst[i] == '.') has_dot = 1;
        i++;
    }
    dst[i] = '\0';
    if (!has_dot && i + 4 < max_len) {
        dst[i++] = '.';
        dst[i++] = 'B';
        dst[i++] = 'A';
        dst[i++] = 'S';
        dst[i] = '\0';
    }
}

/* --------------------------------------------------------------------------
 * Variable Storage & Symbol Table
 * -------------------------------------------------------------------------- */

static void basic_clear_vars(void) {
    s_num_vars = 0;
}

static basic_var_t *basic_find_var(const char *name) {
    uint16_t i;
    for (i = 0; i < s_num_vars; i++) {
        if (basic_stricmp(s_vars[i].name, name) == 0) {
            return &s_vars[i];
        }
    }
    return 0;
}

static basic_var_t *basic_get_or_create_var(const char *name) {
    basic_var_t *v = basic_find_var(name);
    uint16_t len;
    if (v) return v;
    if (s_num_vars >= BASIC_MAX_VARS) {
        os_puts("Out of variable space\n");
        return 0;
    }
    v = &s_vars[s_num_vars++];
    basic_strncpy(v->name, name, BASIC_MAX_VARNAME - 1);
    v->name[BASIC_MAX_VARNAME - 1] = '\0';

    len = basic_strlen(v->name);
    if (len > 0 && v->name[len - 1] == '$') {
        v->type = BASIC_TYPE_STR;
        v->sval[0] = '\0';
        v->slen = 0;
        v->ival = 0;
        v->fval = 0;
    } else if (len > 0 && v->name[len - 1] == '%') {
        v->type = BASIC_TYPE_INT;
        v->ival = 0;
        v->fval = 0;
        v->sval[0] = '\0';
        v->slen = 0;
    } else {
        v->type = BASIC_TYPE_FLOAT;
        v->ival = 0;
        v->fval = 0;
        v->sval[0] = '\0';
        v->slen = 0;
    }
    return v;
}

static void basic_set_var_str_len(const char *name, const char *val, uint16_t len) {
    basic_var_t *var = basic_get_or_create_var(name);
    if (!var) return;
    var->type = BASIC_TYPE_STR;
    if (len > BASIC_MAX_STR_LEN) len = BASIC_MAX_STR_LEN;
    if (len > 0 && val) {
        basic_memcpy(var->sval, val, len);
    }
    var->sval[len] = '\0';
    var->slen = len;
}

static int basic_parse_var_name(const uint8_t __far *tok, uint16_t *pos, char *out_name, uint16_t max_len) {
    uint16_t p = *pos;
    uint16_t np = 0;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (!basic_is_alpha((char)tok[p])) return 0;

    while (basic_is_alpha((char)tok[p]) || basic_is_digit((char)tok[p]) || tok[p] == '.' || tok[p] == '_') {
        if (np + 2 < max_len) {
            out_name[np++] = basic_toupper((char)tok[p]);
        }
        p++;
    }
    if (tok[p] == '$' || tok[p] == '%' || tok[p] == '!' || tok[p] == '#') {
        if (np + 1 < max_len) {
            out_name[np++] = (char)tok[p];
        }
        p++;
    }
    out_name[np] = '\0';
    *pos = p;
    return (np > 0);
}

/* --------------------------------------------------------------------------
 * Array Storage & Indexing (Stored in s_basic_seg)
 * -------------------------------------------------------------------------- */

static basic_array_t *basic_find_array(const char *name) {
    uint16_t i;
    for (i = 0; i < s_num_arrays; i++) {
        if (basic_stricmp(s_arrays[i].name, name) == 0) {
            return &s_arrays[i];
        }
    }
    return 0;
}

static basic_array_t *basic_create_default_array(const char *name, uint8_t num_dims) {
    uint16_t dim1 = 11;
    uint16_t dim2 = (num_dims == 2) ? 11 : 1;
    uint16_t total_elems = (num_dims == 1) ? dim1 : (dim1 * dim2);
    uint16_t len = basic_strlen(name);
    uint8_t type;
    uint16_t elem_size;
    uint16_t bytes;
    basic_array_t *arr;
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t b;

    if (len > 0 && name[len - 1] == '$') {
        type = BASIC_TYPE_STR;
        elem_size = BASIC_STR_ELEM_LEN;
    } else if (len > 0 && name[len - 1] == '%') {
        type = BASIC_TYPE_INT;
        elem_size = 2;
    } else {
        type = BASIC_TYPE_FLOAT;
        elem_size = 4;
    }
    bytes = total_elems * elem_size;

    if (s_num_arrays >= BASIC_MAX_ARRAYS) {
        os_puts("Out of array space\n");
        return 0;
    }
    if ((uint32_t)s_array_heap_free + bytes >= 65000L) {
        os_puts("Out of memory in array alloc\n");
        return 0;
    }

    arr = &s_arrays[s_num_arrays++];
    basic_strncpy(arr->name, name, BASIC_MAX_VARNAME - 1);
    arr->name[BASIC_MAX_VARNAME - 1] = '\0';
    arr->type = type;
    arr->num_dims = num_dims;
    arr->dim1 = dim1;
    arr->dim2 = dim2;
    arr->data_off = s_array_heap_free;
    s_array_heap_free += bytes;

    for (b = 0; b < bytes; b++) {
        mem[arr->data_off + b] = 0;
    }
    return arr;
}

static int basic_read_array_elem(basic_array_t *arr, int16_t i1, int16_t i2, basic_val_t *out_val) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t linear_idx;
    uint16_t off;

    if (i1 < 0 || (uint16_t)i1 >= arr->dim1) {
        return basic_error(9, "Subscript out of range\n");
    }
    if (arr->num_dims == 2 && (i2 < 0 || (uint16_t)i2 >= arr->dim2)) {
        return basic_error(9, "Subscript out of range\n");
    }

    linear_idx = (arr->num_dims == 1) ? (uint16_t)i1 : ((uint16_t)i1 * arr->dim2 + (uint16_t)i2);

    if (arr->type == BASIC_TYPE_INT) {
        off = arr->data_off + linear_idx * 2;
        out_val->type = BASIC_TYPE_INT;
        out_val->ival = (int16_t)((uint16_t)mem[off] | ((uint16_t)mem[off + 1] << 8));
        out_val->fval = 0;
        out_val->sval = 0;
    } else if (arr->type == BASIC_TYPE_FLOAT) {
        off = arr->data_off + linear_idx * 4;
        out_val->type = BASIC_TYPE_FLOAT;
        out_val->fval = (uint32_t)mem[off] | ((uint32_t)mem[off + 1] << 8) |
                        ((uint32_t)mem[off + 2] << 16) | ((uint32_t)mem[off + 3] << 24);
        out_val->ival = 0;
        out_val->sval = 0;
    } else {
        char *str = basic_alloc_temp_str();
        uint16_t k;
        off = arr->data_off + linear_idx * BASIC_STR_ELEM_LEN;
        for (k = 0; k < BASIC_STR_ELEM_LEN - 1 && mem[off + k] != 0; k++) {
            str[k] = (char)mem[off + k];
        }
        str[k] = '\0';
        out_val->type = BASIC_TYPE_STR;
        out_val->sval = str;
        out_val->ival = 0;
        out_val->fval = 0;
    }
    return 0;
}

static int basic_write_array_elem(basic_array_t *arr, int16_t i1, int16_t i2, const basic_val_t *val) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t linear_idx;
    uint16_t off;

    if (i1 < 0 || (uint16_t)i1 >= arr->dim1) {
        return basic_error(9, "Subscript out of range\n");
    }
    if (arr->num_dims == 2 && (i2 < 0 || (uint16_t)i2 >= arr->dim2)) {
        return basic_error(9, "Subscript out of range\n");
    }

    linear_idx = (arr->num_dims == 1) ? (uint16_t)i1 : ((uint16_t)i1 * arr->dim2 + (uint16_t)i2);

    if (arr->type == BASIC_TYPE_INT) {
        if (val->type != BASIC_TYPE_INT && val->type != BASIC_TYPE_UINT) {
            return basic_error(13, "Type mismatch\n");
        }
        off = arr->data_off + linear_idx * 2;
        mem[off] = (uint8_t)(val->ival & 0xFF);
        mem[off + 1] = (uint8_t)(val->ival >> 8);
    } else if (arr->type == BASIC_TYPE_FLOAT) {
        basic_float_t fv;
        if (val->type == BASIC_TYPE_FLOAT) {
            fv = val->fval;
        } else if (val->type == BASIC_TYPE_INT) {
            fv = soft_from_int(val->ival);
        } else if (val->type == BASIC_TYPE_UINT) {
            fv = soft_from_uint((uint16_t)val->ival);
        } else {
            return basic_error(13, "Type mismatch\n");
        }
        off = arr->data_off + linear_idx * 4;
        mem[off]     = (uint8_t)(fv & 0xFF);
        mem[off + 1] = (uint8_t)((fv >> 8) & 0xFF);
        mem[off + 2] = (uint8_t)((fv >> 16) & 0xFF);
        mem[off + 3] = (uint8_t)((fv >> 24) & 0xFF);
    } else {
        uint16_t k;
        if (val->type != BASIC_TYPE_STR) {
            return basic_error(13, "Type mismatch\n");
        }
        off = arr->data_off + linear_idx * BASIC_STR_ELEM_LEN;
        for (k = 0; k < BASIC_STR_ELEM_LEN - 1 && val->sval && val->sval[k] != 0; k++) {
            mem[off + k] = (uint8_t)val->sval[k];
        }
        mem[off + k] = 0;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * File Subsystem Support (Handles #1..#4)
 * -------------------------------------------------------------------------- */

static void basic_close_all_files(void) {
    uint8_t i;
    for (i = 0; i < BASIC_MAX_FILES; i++) {
        s_files[i].is_open = 0;
        s_files[i].num_fields = 0;
    }
}

static int basic_file_getc(uint8_t fnum) {
    basic_file_t *f;
    if (fnum < 1 || fnum > BASIC_MAX_FILES) return -1;
    f = &s_files[fnum - 1];
    if (!f->is_open || f->mode != 'I') return -1;

    if (f->buf_pos >= f->buf_len) {
        uint16_t to_read;
        uint16_t rd;
        if (f->file_pos >= f->file_len) {
            f->eof = 1;
            return -1;
        }
        to_read = (uint16_t)((f->file_len - f->file_pos > BASIC_FILE_BUF_SIZE) ?
                             BASIC_FILE_BUF_SIZE : (f->file_len - f->file_pos));
        rd = os_file_read_offset(f->filename, (void __far *)FP_HERE(f->buf), to_read, f->file_pos);
        if (rd == 0) {
            f->eof = 1;
            return -1;
        }
        f->file_pos += rd;
        f->buf_pos = 0;
        f->buf_len = rd;
    }
    return (int)f->buf[f->buf_pos++];
}

static void basic_file_ungetc(uint8_t fnum) {
    basic_file_t *f;
    if (fnum < 1 || fnum > BASIC_MAX_FILES) return;
    f = &s_files[fnum - 1];
    if (!f->is_open || f->mode != 'I') return;
    if (f->buf_pos > 0) {
        f->buf_pos--;
        f->eof = 0;
    }
}

static int basic_file_write_data(uint8_t fnum, const char *data, uint16_t len) {
    basic_file_t *f;
    if (fnum < 1 || fnum > BASIC_MAX_FILES) return -1;
    f = &s_files[fnum - 1];
    if (!f->is_open || (f->mode != 'O' && f->mode != 'A')) {
        os_puts("Bad file mode\n");
        return -1;
    }
    os_file_append(f->filename, (const void __far *)FP_HERE(data), len);
    f->file_pos += len;
    f->file_len += len;
    return 0;
}

/* --------------------------------------------------------------------------
 * Tokenizer
 * -------------------------------------------------------------------------- */

static uint16_t basic_tokenize(const char *in, uint8_t *out, uint16_t max_out) {
    uint16_t ip = 0;
    uint16_t op = 0;

    while (in[ip] != '\0' && op + 2 < max_out) {
        char c = in[ip];

        if (c == ' ' || c == '\t') {
            out[op++] = (uint8_t)c;
            ip++;
            continue;
        }

        if (c == '"') {
            out[op++] = (uint8_t)c;
            ip++;
            while (in[ip] != '\0' && op + 2 < max_out) {
                out[op++] = (uint8_t)in[ip];
                if (in[ip] == '"') {
                    ip++;
                    break;
                }
                ip++;
            }
            continue;
        }

        if (c == '\'') {
            out[op++] = TOK_SQUOTE;
            ip++;
            while (in[ip] != '\0' && op + 1 < max_out) {
                out[op++] = (uint8_t)in[ip++];
            }
            break;
        }

        if (c == '?') {
            out[op++] = TOK_PRINT;
            ip++;
            continue;
        }

        {
            uint16_t k;
            uint16_t tok;
            int matched = 0;

            for (k = 0; k < BASIC_NUM_KEYWORDS; k++) {
                uint8_t klen = g_basic_keywords[k].len;
                const char *kw = g_basic_keywords[k].kw;

                if (basic_strnicmp(&in[ip], kw, klen) == 0) {
                    char first_kw_char = kw[0];
                    char last_kw_char = kw[klen - 1];
                    char next_in_char = in[ip + klen];

                    if (basic_is_alpha(first_kw_char) || basic_is_digit(first_kw_char)) {
                        if (ip > 0 && (basic_is_alpha(in[ip - 1]) || basic_is_digit(in[ip - 1]) || in[ip - 1] == '.' || in[ip - 1] == '_')) {
                            continue;
                        }
                    }

                    if (basic_is_alpha(last_kw_char) || basic_is_digit(last_kw_char)) {
                        if (basic_is_alpha(next_in_char) || basic_is_digit(next_in_char) || next_in_char == '.' || next_in_char == '_' ||
                            next_in_char == '$' || next_in_char == '%' || next_in_char == '!' || next_in_char == '#') {
                            if (next_in_char == '$' && basic_strnicmp(&in[ip], "INPUT$", 6) == 0) {
                                /* INPUT$ is a built-in function, tokenized as TOK_INPUT followed by '$' */
                            } else {
                                continue;
                            }
                        }
                    }

                    tok = g_basic_keywords[k].tok;
                    if (tok > 0xFF) {
                        out[op++] = (uint8_t)(tok >> 8);
                        out[op++] = (uint8_t)(tok & 0xFF);
                    } else {
                        out[op++] = (uint8_t)tok;
                    }

                    ip += klen;
                    matched = 1;

                    if (tok == TOK_REM) {
                        while (in[ip] != '\0' && op + 1 < max_out) {
                            out[op++] = (uint8_t)in[ip++];
                        }
                    }
                    break;
                }
            }

            if (matched) continue;
        }

        out[op++] = (uint8_t)basic_toupper(c);
        ip++;
    }

    out[op] = 0x00;
    return op;
}

/* --------------------------------------------------------------------------
 * Lister & De-tokenizer
 * -------------------------------------------------------------------------- */

static uint16_t basic_detokenize_tokens(const uint8_t __far *tok_data, char *out_buf, uint16_t max_out) {
    uint16_t p = 0;
    uint16_t op = 0;

    while (tok_data[p] != 0x00 && op + 2 < max_out) {
        uint8_t b = tok_data[p++];

        if (b == 0xFD || b == 0xFE || b == 0xFF) {
            uint8_t b2 = tok_data[p++];
            uint16_t tok = ((uint16_t)b << 8) | b2;
            const char *name = basic_token_to_name(tok);
            if (name) {
                while (*name && op + 1 < max_out) out_buf[op++] = *name++;
                if (tok_data[p] >= 'A' && tok_data[p] <= 'Z' && op + 1 < max_out) {
                    out_buf[op++] = ' ';
                }
            }
        } else if (b >= 0x81) {
            const char *name = basic_token_to_name((uint16_t)b);
            if (name) {
                while (*name && op + 1 < max_out) out_buf[op++] = *name++;
                if (b != TOK_SQUOTE && tok_data[p] >= 'A' && tok_data[p] <= 'Z' && op + 1 < max_out) {
                    out_buf[op++] = ' ';
                }
            }
        } else {
            out_buf[op++] = (char)b;
        }
    }
    out_buf[op] = '\0';
    return op;
}

static uint16_t basic_detokenize_line(uint16_t line_num, const uint8_t __far *tok_data, char *out_buf, uint16_t max_out) {
    uint16_t op = 0;
    char num_buf[8];
    int np = 0;
    uint16_t v = line_num;

    if (v == 0) {
        if (op + 1 < max_out) out_buf[op++] = '0';
    } else {
        while (v > 0) {
            num_buf[np++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (np > 0 && op + 1 < max_out) {
            out_buf[op++] = num_buf[--np];
        }
    }
    if (op + 1 < max_out) out_buf[op++] = ' ';

    op += basic_detokenize_tokens(tok_data, &out_buf[op], max_out - op);
    return op;
}

static void basic_list_line(uint16_t line_num, const uint8_t __far *tok_data) {
    char buf[BASIC_MAX_LINE_LEN];
    basic_detokenize_line(line_num, tok_data, buf, sizeof(buf));
    basic_term_puts(buf);
    basic_term_putc('\n');
}

/* --------------------------------------------------------------------------
 * Phase 12 Helpers: BIOS RTC, Pixels, Date, Time & Program Management
 * -------------------------------------------------------------------------- */

#ifdef __WATCOMC__
static uint8_t bios_read_pixel(uint16_t x, uint16_t y);
#pragma aux bios_read_pixel = \
    "mov ah, 0x0D" \
    "xor bh, bh" \
    "int 0x10" \
    parm [cx] [dx] value [al] modify [ax bx];

static void bios_set_timer_ticks(uint16_t hi, uint16_t lo);
#pragma aux bios_set_timer_ticks = \
    "mov ah, 0x01" \
    "int 0x1a" \
    parm [cx] [dx] modify [ax];
#else
static uint8_t bios_read_pixel(uint16_t x, uint16_t y) { (void)x; (void)y; return 0; }
static void bios_set_timer_ticks(uint16_t hi, uint16_t lo) { (void)hi; (void)lo; }
#endif

static void basic_get_date_str(char *buf) {
    uint8_t m = s_date_month;
    uint8_t d = s_date_day;
    uint16_t yr = s_date_year;

    buf[0] = (char)('0' + (m / 10));
    buf[1] = (char)('0' + (m % 10));
    buf[2] = '-';
    buf[3] = (char)('0' + (d / 10));
    buf[4] = (char)('0' + (d % 10));
    buf[5] = '-';
    buf[6] = (char)('0' + ((yr / 1000) % 10));
    buf[7] = (char)('0' + ((yr / 100) % 10));
    buf[8] = (char)('0' + ((yr / 10) % 10));
    buf[9] = (char)('0' + (yr % 10));
    buf[10] = '\0';
}

static void basic_get_time_str(char *buf) {
    uint8_t h, m, s;
    uint32_t rem = *((const volatile uint32_t __far *)0x0040006CUL);

    h = 0;
    while (rem >= 65543UL) {
        rem -= 65543UL;
        h++;
    }
    h %= 24;
    {
        uint16_t hour_ticks = (uint16_t)rem;
        m = (uint8_t)(hour_ticks / 1092U);
        if (m > 59) m = 59;
        {
            uint16_t min_ticks = hour_ticks % 1092U;
            s = (uint8_t)(((uint16_t)min_ticks * 10U) / 182U);
            if (s > 59) s = 59;
        }
    }

    buf[0] = (char)('0' + (h / 10));
    buf[1] = (char)('0' + (h % 10));
    buf[2] = ':';
    buf[3] = (char)('0' + (m / 10));
    buf[4] = (char)('0' + (m % 10));
    buf[5] = ':';
    buf[6] = (char)('0' + (s / 10));
    buf[7] = (char)('0' + (s % 10));
    buf[8] = '\0';
}

static int basic_set_date_str(const char *s) {
    uint8_t m = 0, d = 0;
    uint16_t yr = 0;
    uint16_t idx = 0;

    while (s[idx] == ' ' || s[idx] == '\t') idx++;
    if (!basic_is_digit(s[idx])) return basic_error(5, "Illegal function call\n");
    m = (uint8_t)basic_parse_uint(s, &idx);

    if (s[idx] != '-' && s[idx] != '/') return basic_error(5, "Illegal function call\n");
    idx++;

    if (!basic_is_digit(s[idx])) return basic_error(5, "Illegal function call\n");
    d = (uint8_t)basic_parse_uint(s, &idx);

    if (s[idx] != '-' && s[idx] != '/') return basic_error(5, "Illegal function call\n");
    idx++;

    if (!basic_is_digit(s[idx])) return basic_error(5, "Illegal function call\n");
    yr = basic_parse_uint(s, &idx);
    if (yr < 100) {
        yr += (yr < 80) ? 2000 : 1900;
    }

    if (m < 1 || m > 12 || d < 1 || d > 31 || yr < 1980 || yr > 2099) {
        return basic_error(5, "Illegal function call\n");
    }

    s_date_month = m;
    s_date_day = d;
    s_date_year = yr;
    return 0;
}

static int basic_set_time_str(const char *s) {
    uint8_t h = 0, m = 0, sec = 0;
    uint16_t idx = 0;

    while (s[idx] == ' ' || s[idx] == '\t') idx++;
    if (!basic_is_digit(s[idx])) return basic_error(5, "Illegal function call\n");
    h = (uint8_t)basic_parse_uint(s, &idx);

    if (s[idx] != ':') return basic_error(5, "Illegal function call\n");
    idx++;

    if (!basic_is_digit(s[idx])) return basic_error(5, "Illegal function call\n");
    m = (uint8_t)basic_parse_uint(s, &idx);

    if (s[idx] == ':') {
        idx++;
        if (basic_is_digit(s[idx])) {
            sec = (uint8_t)basic_parse_uint(s, &idx);
        }
    }

    if (h > 23 || m > 59 || sec > 59) {
        return basic_error(5, "Illegal function call\n");
    }

    {
        uint32_t h_ticks = ((uint32_t)h << 16) + (uint32_t)((uint16_t)h * 7U);
        uint16_t m_ticks = (uint16_t)m * 1092U;
        uint16_t s_ticks = ((uint16_t)sec * 182U) / 10U;
        uint32_t ticks = h_ticks + (uint32_t)m_ticks + (uint32_t)s_ticks;
        bios_set_timer_ticks((uint16_t)(ticks >> 16), (uint16_t)(ticks & 0xFFFFU));
        *((uint32_t __far *)0x0040006CUL) = ticks;
        *((uint8_t __far *)0x00400070UL) = 0;
    }
    return 0;
}

static int basic_cmd_delete(uint16_t start, uint16_t end) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t cur = s_prog_head;
    uint16_t prev = 0;
    int deleted = 0;

    if (start > end) {
        return basic_error(5, "Illegal function call\n");
    }

    while (cur != 0) {
        uint16_t next = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
        uint16_t num  = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);

        if (num >= start && num <= end) {
            if (prev == 0) {
                s_prog_head = next;
            } else {
                mem[prev] = (uint8_t)(next & 0xFF);
                mem[prev + 1] = (uint8_t)(next >> 8);
            }
            deleted++;
            cur = next;
        } else {
            prev = cur;
            cur = next;
        }
    }

    s_can_cont = 0;
    if (deleted == 0 && start == end) {
        return basic_error(5, "Illegal function call\n");
    }
    return 0;
}

typedef struct {
    uint16_t old_num;
    uint16_t new_num;
} basic_renum_t;

static int basic_cmd_renum(uint16_t new_start, uint16_t old_start, uint16_t step) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t temp_seg;
    basic_renum_t __far *map;
    uint8_t __far *text_buf;
    uint16_t cur;
    uint16_t count = 0;
    uint16_t cur_new = new_start;
    uint16_t text_len = 0;
    uint16_t i;

    if (s_prog_head == 0) return 0;
    if (step == 0) step = 10;

    temp_seg = os_mem_alloc(1024);
    if (temp_seg == 0) {
        return basic_error(7, "Out of memory\n");
    }

    map = (basic_renum_t __far *)MK_FP(temp_seg, 0);
    text_buf = (uint8_t __far *)MK_FP(temp_seg, 2048);

    cur = s_prog_head;
    while (cur != 0 && count < 500) {
        uint16_t next = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
        uint16_t num  = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);

        map[count].old_num = num;
        if (num >= old_start) {
            if ((uint32_t)cur_new > 65529UL) {
                os_mem_free(temp_seg);
                return basic_error(5, "Illegal function call\n");
            }
            map[count].new_num = cur_new;
            cur_new += step;
        } else {
            map[count].new_num = num;
        }
        count++;
        cur = next;
    }

    for (i = 1; i < count; i++) {
        if (map[i].new_num <= map[i - 1].new_num) {
            os_mem_free(temp_seg);
            return basic_error(5, "Illegal function call\n");
        }
    }

    for (i = 0; i < count; i++) {
        char line_str[BASIC_MAX_LINE_LEN];
        char updated_str[BASIC_MAX_LINE_LEN];
        uint16_t lp = 0, up = 0;
        uint16_t line_off = basic_find_line(map[i].old_num);
        if (!line_off) continue;

        basic_detokenize_tokens(&mem[line_off + 4], line_str, sizeof(line_str));

        while (line_str[lp] != '\0' && up + 12 < sizeof(updated_str)) {
            if (line_str[lp] == '"') {
                updated_str[up++] = line_str[lp++];
                while (line_str[lp] != '\0' && line_str[lp] != '"' && up + 2 < sizeof(updated_str)) {
                    updated_str[up++] = line_str[lp++];
                }
                if (line_str[lp] == '"') updated_str[up++] = line_str[lp++];
                continue;
            }

            if (line_str[lp] == '\'' || basic_strnicmp(&line_str[lp], "REM", 3) == 0) {
                while (line_str[lp] != '\0' && up + 1 < sizeof(updated_str)) {
                    updated_str[up++] = line_str[lp++];
                }
                break;
            }

            {
                int kw_len = 0;
                int is_branch = 0;
                if (basic_strnicmp(&line_str[lp], "GOTO", 4) == 0) { kw_len = 4; is_branch = 1; }
                else if (basic_strnicmp(&line_str[lp], "GOSUB", 5) == 0) { kw_len = 5; is_branch = 1; }
                else if (basic_strnicmp(&line_str[lp], "THEN", 4) == 0) { kw_len = 4; is_branch = 1; }
                else if (basic_strnicmp(&line_str[lp], "ELSE", 4) == 0) { kw_len = 4; is_branch = 1; }
                else if (basic_strnicmp(&line_str[lp], "RESTORE", 7) == 0) { kw_len = 7; is_branch = 1; }
                else if (basic_strnicmp(&line_str[lp], "RUN", 3) == 0) { kw_len = 3; is_branch = 1; }
                else if (basic_strnicmp(&line_str[lp], "RESUME", 6) == 0) { kw_len = 6; is_branch = 1; }
                else if (basic_strnicmp(&line_str[lp], "RETURN", 6) == 0) { kw_len = 6; is_branch = 1; }

                if (is_branch) {
                    int k;
                    for (k = 0; k < kw_len; k++) {
                        updated_str[up++] = line_str[lp++];
                    }
                    for (;;) {
                        while (line_str[lp] == ' ' || line_str[lp] == '\t') {
                            updated_str[up++] = line_str[lp++];
                        }
                        if (basic_is_digit(line_str[lp])) {
                            uint16_t old_target = basic_parse_uint(line_str, &lp);
                            uint16_t new_target = old_target;
                            int found = 0;
                            int m;
                            for (m = 0; m < count; m++) {
                                if (map[m].old_num == old_target) {
                                    new_target = map[m].new_num;
                                    found = 1;
                                    break;
                                }
                            }
                            if (!found) {
                                os_puts("Undefined line number ");
                                basic_print_uint(old_target);
                                os_puts(" in ");
                                basic_print_uint(map[i].old_num);
                                os_putc('\n');
                            }
                            {
                                char nbuf[8];
                                int np = 0;
                                uint16_t v = new_target;
                                char tmp[8];
                                int tp = 0;
                                if (v == 0) nbuf[np++] = '0';
                                else {
                                    while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                                    while (tp > 0) nbuf[np++] = tmp[--tp];
                                }
                                for (m = 0; m < np; m++) updated_str[up++] = nbuf[m];
                            }
                            while (line_str[lp] == ' ' || line_str[lp] == '\t') {
                                updated_str[up++] = line_str[lp++];
                            }
                            if (line_str[lp] == ',') {
                                updated_str[up++] = line_str[lp++];
                                continue;
                            }
                        }
                        break;
                    }
                    continue;
                }
            }

            updated_str[up++] = line_str[lp++];
        }
        updated_str[up] = '\0';

        {
            char nbuf[8];
            int np = 0;
            uint16_t v = map[i].new_num;
            char tmp[8];
            int tp = 0;
            int m;
            if (v == 0) nbuf[np++] = '0';
            else {
                while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                while (tp > 0) nbuf[np++] = tmp[--tp];
            }
            for (m = 0; m < np && text_len + 1 < 14000; m++) {
                text_buf[text_len++] = (uint8_t)nbuf[m];
            }
            if (text_len + 1 < 14000) text_buf[text_len++] = ' ';
            for (m = 0; m < up && text_len + 1 < 14000; m++) {
                text_buf[text_len++] = (uint8_t)updated_str[m];
            }
            if (text_len + 1 < 14000) text_buf[text_len++] = '\n';
        }
    }

    basic_new();

    {
        uint16_t tp = 0;
        char line_in[BASIC_MAX_LINE_LEN];
        uint8_t tok_tmp[BASIC_MAX_LINE_LEN];

        while (tp < text_len) {
            uint16_t lp = 0;
            uint16_t sp = 0;
            while (tp < text_len && text_buf[tp] != '\r' && text_buf[tp] != '\n') {
                if (lp + 1 < sizeof(line_in)) line_in[lp++] = (char)text_buf[tp];
                tp++;
            }
            line_in[lp] = '\0';
            while (tp < text_len && (text_buf[tp] == '\r' || text_buf[tp] == '\n')) tp++;

            while (line_in[sp] == ' ' || line_in[sp] == '\t') sp++;
            if (basic_is_digit(line_in[sp])) {
                uint16_t num = basic_parse_uint(line_in, &sp);
                uint16_t tlen;
                while (line_in[sp] == ' ' || line_in[sp] == '\t') sp++;
                tlen = basic_tokenize(&line_in[sp], tok_tmp, sizeof(tok_tmp));
                basic_insert_line(num, tok_tmp, tlen);
            }
        }
    }

    os_mem_free(temp_seg);
    s_can_cont = 0;
    return 0;
}

/* --------------------------------------------------------------------------
 * Phase 10: Event Trapping & Function Key Helpers
 * -------------------------------------------------------------------------- */

static void basic_init_fkeys(void) {
    int i, j;
    static const char * const s_defs[11] = {
        "",
        "LIST ",
        "RUN\r",
        "LOAD\"",
        "SAVE\"",
        "CONT\r",
        ",\"LPT1:\"",
        "TRON\r",
        "TROFF\r",
        "KEY ",
        "SCREEN 0,0,0\r"
    };
    for (i = 1; i <= 10; i++) {
        for (j = 0; j < 15 && s_defs[i][j] != '\0'; j++) {
            s_fkey_strings[i][j] = s_defs[i][j];
        }
        s_fkey_strings[i][j] = '\0';
    }
    s_key_visible = 1;
}

static void basic_display_softkeys(void) {
    uint16_t cur;
    uint8_t row, col;
    int k;
    uint8_t bda_cols;
    int max_k;

    if (s_screen_mode != 0) return;
    bda_cols = *((const uint8_t __far *)0x0040004AUL);
    if (bda_cols == 0) bda_cols = 80;
    max_k = (bda_cols <= 40) ? 5 : 10;

    cur = os_get_cursor();
    row = (uint8_t)(cur >> 8);
    col = (uint8_t)(cur & 0xFF);

    for (k = 1; k <= max_k; k++) {
        char num_c = (k == 10) ? '0' : (char)('0' + k);
        uint8_t base = (uint8_t)((k - 1) * 8);
        int l, m;

        /* Inverse video for key number (0x70) */
        os_console_write_cell(24, base, (0x70 << 8) | (uint8_t)num_c);

        /* Normal video for up to 6 label characters (0x07) */
        m = 0;
        for (l = 0; l < 6 && s_fkey_strings[k][m] != '\0'; m++) {
            char c = s_fkey_strings[k][m];
            if (c == '\r' || c == '\n') continue;
            os_console_write_cell(24, (uint8_t)(base + 1 + l), (0x07 << 8) | (uint8_t)c);
            l++;
        }
        while (l < 6) {
            os_console_write_cell(24, (uint8_t)(base + 1 + l), (0x07 << 8) | ' ');
            l++;
        }
        /* Trailing space between keys (Light Blue / Cyan background: 0x30) */
        os_console_write_cell(24, (uint8_t)(base + 7), (0x30 << 8) | ' ');
    }

    {
        uint8_t col_idx;
        for (col_idx = (uint8_t)(max_k * 8); col_idx < bda_cols; col_idx++) {
            os_console_write_cell(24, col_idx, (0x30 << 8) | ' ');
        }
    }

    os_set_cursor(row, col);
}

static void basic_clear_softkeys(void) {
    uint16_t cur;
    uint8_t row, col;
    uint8_t i;
    uint8_t bda_cols;
    uint8_t attr;

    if (s_screen_mode != 0) return;
    bda_cols = *((const uint8_t __far *)0x0040004AUL);
    if (bda_cols == 0) bda_cols = 80;

    cur = os_get_cursor();
    row = (uint8_t)(cur >> 8);
    col = (uint8_t)(cur & 0xFF);

    attr = s_text_attr ? s_text_attr : 0x07;
    for (i = 0; i < bda_cols; i++) {
        os_console_write_cell(24, i, ((uint16_t)attr << 8) | ' ');
    }

    os_set_cursor(row, col);
}

static uint8_t scan_to_trap_key(uint8_t sc) {
    if (sc >= 0x3B && sc <= 0x44) return (uint8_t)(sc - 0x3B + 1); /* F1..F10 -> 1..10 */
    if (sc == 0x48) return 11; /* Up */
    if (sc == 0x4B) return 12; /* Left */
    if (sc == 0x4D) return 13; /* Right */
    if (sc == 0x50) return 14; /* Down */
    return 0;
}

static void basic_process_key(uint16_t key) {
    uint8_t sc = (uint8_t)(key >> 8);
    uint8_t ascii = (uint8_t)(key & 0xFF);
    uint8_t tk = scan_to_trap_key(sc);

    if (tk >= 1 && tk <= 14 && s_key_trap[tk].line != 0 && s_key_trap[tk].state != 0) {
        if (s_key_trap[tk].state == 1) { /* ON */
            s_key_trap[tk].pending = 1;
        } else if (s_key_trap[tk].state == 2) { /* STOP */
            s_key_trap[tk].pending = 1;
        }
        return;
    }

    if (tk >= 1 && tk <= 10 && s_fkey_strings[tk][0] != '\0') {
        const char *p = s_fkey_strings[tk];
        while (*p && s_key_fifo_count < BASIC_KEY_FIFO_SIZE) {
            s_key_fifo[(s_key_fifo_head + s_key_fifo_count) % BASIC_KEY_FIFO_SIZE] = (uint8_t)*p++;
            s_key_fifo_count++;
        }
        return;
    }

    if (ascii != 0 && s_key_fifo_count < BASIC_KEY_FIFO_SIZE) {
        s_key_fifo[(s_key_fifo_head + s_key_fifo_count) % BASIC_KEY_FIFO_SIZE] = ascii;
        s_key_fifo_count++;
    }
}

static void basic_poll_keyboard(void) {
    while (os_kbhit()) {
        uint16_t key = os_getc();
        if ((key & 0xFF) == 0x03) {
            s_break_pending = 1;
            break;
        }
        basic_process_key(key);
    }
}

static int basic_kbhit(void) {
    basic_poll_keyboard();
    return (s_key_fifo_count > 0) || s_break_pending;
}

static uint16_t basic_getc(void) {
    while (!basic_kbhit()) {
        if (s_running) basic_poll_events();
    }
    if (s_key_fifo_count > 0) {
        uint8_t ch = s_key_fifo[s_key_fifo_head];
        s_key_fifo_head = (s_key_fifo_head + 1) % BASIC_KEY_FIFO_SIZE;
        s_key_fifo_count--;
        return (uint16_t)ch;
    }
    if (s_break_pending) {
        s_break_pending = 0;
        return 0x03;
    }
    return 0;
}

static void basic_poll_events(void) {
    uint32_t now;

    if (!s_running || s_in_error_handler || s_jumped) return;

    now = *((const uint32_t __far *)MK_FP(0x0040, 0x006C));

    /* 1. Timer event checking */
    if (s_timer_interval > 0) {
        int triggered = 0;
        if (now >= s_timer_target_tick) {
            triggered = 1;
        } else if (s_timer_target_tick > 1573040UL && now < 2000UL) {
            triggered = 1;
        }

        if (triggered) {
            uint32_t dt = mul16(s_timer_interval, 18) + (uint32_t)(s_timer_interval / 5);
            s_timer_target_tick = now + dt;

            if (s_timer_state == 1) { /* ON */
                if (!s_timer_in_handler) {
                    s_timer_pending = 1;
                }
            } else if (s_timer_state == 2) { /* STOP */
                s_timer_pending = 1;
            }
        }

        if (s_timer_state == 1 && s_timer_pending && !s_timer_in_handler) {
            uint16_t target_off = basic_find_line(s_timer_line);
            if (target_off != 0 && s_gosub_sp < BASIC_MAX_GOSUB) {
                s_timer_pending = 0;
                s_timer_in_handler = 1;
                s_gosub_stack[s_gosub_sp].line_off = s_cur_line_off;
                s_gosub_stack[s_gosub_sp].stmt_pos = s_pc_stmt;
                s_gosub_sp++;
                s_pc_line = target_off;
                s_pc_stmt = 4;
                s_jumped = 1;
                return;
            }
        }
    }

    /* 2. Keyboard event checking */
    basic_poll_keyboard();

    {
        int k;
        for (k = 1; k <= 14; k++) {
            if (s_key_trap[k].state == 1 && s_key_trap[k].pending && !s_key_trap[k].in_handler) {
                uint16_t target_off = basic_find_line(s_key_trap[k].line);
                if (target_off != 0 && s_gosub_sp < BASIC_MAX_GOSUB) {
                    s_key_trap[k].pending = 0;
                    s_key_trap[k].in_handler = 1;
                    s_gosub_stack[s_gosub_sp].line_off = s_cur_line_off;
                    s_gosub_stack[s_gosub_sp].stmt_pos = s_pc_stmt;
                    s_gosub_sp++;
                    s_pc_line = target_off;
                    s_pc_stmt = 4;
                    s_jumped = 1;
                    return;
                }
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * Program Memory Storage (Linked list of tokenized lines)
 * -------------------------------------------------------------------------- */

static void basic_new(void) {
    s_prog_head = 0;
    s_prog_tail = 4;
    basic_clear_vars();
    basic_close_all_files();
    s_gosub_sp = 0;
    s_for_sp = 0;
    s_while_sp = 0;
    s_num_arrays = 0;
    s_array_heap_start = 16;
    s_array_heap_free  = 16;
    s_data_line_off = 0;
    s_data_stmt_pos = 0;
    s_def_seg = (uint16_t)get_ds();
    s_error_goto_line = 0;
    s_err = 0;
    s_erl = 0;
    s_in_error_handler = 0;
    s_err_reported = 0;
    s_timer_state = 0;
    s_timer_pending = 0;
    s_timer_in_handler = 0;
    s_timer_interval = 0;
    s_timer_line = 0;
    {
        int k;
        for (k = 1; k <= 14; k++) {
            s_key_trap[k].line = 0;
            s_key_trap[k].state = 0;
            s_key_trap[k].pending = 0;
            s_key_trap[k].in_handler = 0;
        }
    }
    s_key_fifo_head = 0;
    s_key_fifo_count = 0;
    s_break_pending = 0;
    s_can_cont = 0;
    s_tron = 0;
    s_auto_mode = 0;
    s_draw_angle = 0;
    s_draw_scale = 4;
    s_draw_color = (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1);
    s_cont_line_off = 0;
    s_cont_stmt_pos = 4;
    basic_init_fkeys();
    if (s_basic_seg) {
        uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
        mem[0] = 0; mem[1] = 0; mem[2] = 0; mem[3] = 0;
    }
}

static uint16_t basic_get_free_mem(void) {
    uint16_t used;
    if (s_num_arrays > 0) {
        used = s_array_heap_free;
    } else if (s_prog_head != 0) {
        used = (s_prog_tail + 15) & ~15;
    } else {
        used = 0;
    }
    if (used < 61440U) {
        return 61440U - used;
    }
    return 0;
}

static uint16_t basic_find_line(uint16_t line_num) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t cur = s_prog_head;
    while (cur != 0) {
        uint16_t next = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
        uint16_t num  = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);
        if (num == line_num) return cur;
        if (num > line_num) break;
        cur = next;
    }
    return 0;
}

static void basic_print_error_name(uint8_t code) {
    switch (code) {
        case 1:  basic_term_puts("NEXT without FOR"); break;
        case 2:  basic_term_puts("Syntax error"); break;
        case 3:  basic_term_puts("RETURN without GOSUB"); break;
        case 4:  basic_term_puts("Out of DATA"); break;
        case 5:  basic_term_puts("Illegal function call"); break;
        case 6:  basic_term_puts("Overflow"); break;
        case 7:  basic_term_puts("Out of memory"); break;
        case 8:  basic_term_puts("Undefined line number"); break;
        case 9:  basic_term_puts("Subscript out of range"); break;
        case 10: basic_term_puts("Duplicate Definition"); break;
        case 11: basic_term_puts("Division by zero"); break;
        case 13: basic_term_puts("Type mismatch"); break;
        case 14: basic_term_puts("Out of string space"); break;
        case 20: basic_term_puts("RESUME without error"); break;
        case 30: basic_term_puts("WEND without WHILE"); break;
        case 52: basic_term_puts("Bad file number"); break;
        case 53: basic_term_puts("File not found"); break;
        case 54: basic_term_puts("Bad file mode"); break;
        case 55: basic_term_puts("File already open"); break;
        default: basic_term_puts("Unprintable error"); break;
    }
}

static void basic_puts_no_nl(const char *s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n' && *(s + 1) == '\0') break;
        basic_term_putc(*s++);
    }
}

static int basic_error(uint8_t code, const char *msg) {
    s_err = (int16_t)code;
    s_erl = s_cur_line_num;

    if (s_error_goto_line != 0 && !s_in_error_handler && s_cur_line_num != 0) {
        uint16_t target_off = basic_find_line(s_error_goto_line);
        if (!target_off) {
            basic_term_puts("Undefined line number in ON ERROR GOTO\n");
            s_running = 0;
            return -1;
        }
        s_in_error_handler = 1;
        s_err_line_off  = s_cur_line_off;
        s_err_stmt_pos  = s_cur_stmt_pos;
        s_err_next_line = s_cur_next_line;
        s_err_next_stmt = s_cur_next_stmt;

        s_pc_line = target_off;
        s_pc_stmt = 4;
        s_jumped = 1;
        return -1;
    }

    if (msg) {
        if (s_cur_line_num != 0) {
            basic_puts_no_nl(msg);
        } else {
            basic_term_puts(msg);
            if (msg[basic_strlen(msg) - 1] != '\n') basic_term_putc('\n');
        }
    } else {
        basic_print_error_name(code);
        if (s_cur_line_num == 0) basic_term_putc('\n');
    }

    s_running = 0;
    return -1;
}

static void basic_delete_line(uint16_t line_num) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t prev = 0;
    uint16_t cur = s_prog_head;

    s_can_cont = 0;

    while (cur != 0) {
        uint16_t next = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
        uint16_t num  = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);

        if (num == line_num) {
            if (prev == 0) {
                s_prog_head = next;
            } else {
                mem[prev] = (uint8_t)(next & 0xFF);
                mem[prev + 1] = (uint8_t)(next >> 8);
            }
            return;
        }

        prev = cur;
        cur = next;
    }
}

static int basic_insert_line(uint16_t line_num, const uint8_t *tok_data, uint16_t tok_len) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t prev = 0;
    uint16_t cur = s_prog_head;
    uint16_t new_off = s_prog_tail;
    uint16_t needed = 4 + tok_len + 1;
    uint16_t i;

    basic_delete_line(line_num);

    if ((uint32_t)s_prog_tail + needed >= 65000L) {
        os_puts("Out of memory\n");
        return -1;
    }

    mem[new_off + 2] = (uint8_t)(line_num & 0xFF);
    mem[new_off + 3] = (uint8_t)(line_num >> 8);
    for (i = 0; i <= tok_len; i++) {
        mem[new_off + 4 + i] = tok_data[i];
    }
    s_prog_tail += needed;

    prev = 0;
    cur = s_prog_head;
    while (cur != 0) {
        uint16_t num = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);
        if (num > line_num) break;
        prev = cur;
        cur = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
    }

    mem[new_off] = (uint8_t)(cur & 0xFF);
    mem[new_off + 1] = (uint8_t)(cur >> 8);

    if (prev == 0) {
        s_prog_head = new_off;
    } else {
        mem[prev] = (uint8_t)(new_off & 0xFF);
        mem[prev + 1] = (uint8_t)(new_off >> 8);
    }

    return 0;
}

static void basic_cmd_list(uint16_t start_line, uint16_t end_line) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t cur = s_prog_head;

    while (cur != 0) {
        uint16_t next = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
        uint16_t num  = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);

        if (num >= start_line && num <= end_line) {
            basic_list_line(num, &mem[cur + 4]);
        }

        if (os_kbhit() && (os_getc() & 0xFF) == 0x03) {
            os_puts("^C\n");
            break;
        }

        cur = next;
    }
}

static void basic_cmd_files(const char *pattern) {
    os_dir_entry_t entry;
    uint16_t idx = 0;
    int col = 0;

    for (;;) {
        int res = os_dir_entry(idx++, (os_dir_entry_t __far *)FP_HERE(&entry));
        if (res < 0) break;
        if (entry.name[0] == '\0') break;

        if (pattern && pattern[0] != '\0') {
            if (!basic_wildcard_match(pattern, entry.name)) continue;
        }

        os_puts(entry.name);
        {
            uint16_t len = basic_strlen(entry.name);
            while (len < 15) {
                os_putc(' ');
                len++;
            }
        }
        col++;
        if (col >= 4) {
            os_putc('\n');
            col = 0;
        }
    }
    if (col != 0) os_putc('\n');
}

/* --------------------------------------------------------------------------
 * Expression Evaluator (Recursive Descent Precedence Climber)
 * -------------------------------------------------------------------------- */

static int basic_match_cmp_op(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    uint8_t c1, c2;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    c1 = tok[p];

    if (c1 != TOK_EQ && c1 != TOK_LT && c1 != TOK_GT && c1 != '=' && c1 != '<' && c1 != '>') {
        return OP_CMP_NONE;
    }
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    c2 = tok[p];

    if ((c1 == TOK_LT || c1 == '<') && (c2 == TOK_EQ || c2 == '=')) { *pos = p + 1; return OP_CMP_LE; }
    if ((c1 == TOK_EQ || c1 == '=') && (c2 == TOK_LT || c2 == '<')) { *pos = p + 1; return OP_CMP_LE; }
    if ((c1 == TOK_GT || c1 == '>') && (c2 == TOK_EQ || c2 == '=')) { *pos = p + 1; return OP_CMP_GE; }
    if ((c1 == TOK_EQ || c1 == '=') && (c2 == TOK_GT || c2 == '>')) { *pos = p + 1; return OP_CMP_GE; }
    if ((c1 == TOK_LT || c1 == '<') && (c2 == TOK_GT || c2 == '>')) { *pos = p + 1; return OP_CMP_NE; }
    if ((c1 == TOK_GT || c1 == '>') && (c2 == TOK_LT || c2 == '<')) { *pos = p + 1; return OP_CMP_NE; }
    if (c1 == TOK_EQ || c1 == '=') { *pos = p; return OP_CMP_EQ; }
    if (c1 == TOK_LT || c1 == '<') { *pos = p; return OP_CMP_LT; }
    if (c1 == TOK_GT || c1 == '>') { *pos = p; return OP_CMP_GT; }
    return OP_CMP_NONE;
}

static basic_float_t val_to_float(const basic_val_t *v) {
    if (v->type == BASIC_TYPE_FLOAT) return v->fval;
    if (v->type == BASIC_TYPE_UINT)  return soft_from_uint((uint16_t)v->ival);
    return soft_from_int(v->ival);
}

static int16_t val_to_int(const basic_val_t *v) {
    if (v->type == BASIC_TYPE_FLOAT) return soft_to_int(v->fval);
    return v->ival;
}

static int eval_primary(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    uint16_t p = *pos;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    /* Parenthesized subexpression */
    if (tok[p] == '(') {
        p++;
        if (basic_eval_expr(tok, &p, out_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ')') {
            os_puts("Syntax error (missing ')')\n");
            return -1;
        }
        p++;
        *pos = p;
        return 0;
    }

    /* String literal */
    if (tok[p] == '"') {
        char *str = basic_alloc_temp_str();
        uint16_t sp = 0;
        p++;
        while (tok[p] != 0 && tok[p] != '"') {
            if (sp < BASIC_MAX_STR_LEN) {
                str[sp++] = (char)tok[p];
            }
            p++;
        }
        if (tok[p] == '"') p++;
        str[sp] = '\0';
        out_val->type = BASIC_TYPE_STR;
        out_val->sval = str;
        out_val->ival = 0;
        *pos = p;
        return 0;
    }

    /* Hex &H... or Octal &O... */
    if (tok[p] == '&') {
        int16_t val = 0;
        char mode;
        p++;
        mode = basic_toupper((char)tok[p]);
        if (mode == 'H') {
            p++;
            while ((tok[p] >= '0' && tok[p] <= '9') ||
                   (tok[p] >= 'A' && tok[p] <= 'F') ||
                   (tok[p] >= 'a' && tok[p] <= 'f')) {
                char c = basic_toupper((char)tok[p++]);
                uint8_t d = (c >= 'A') ? (uint8_t)(c - 'A' + 10) : (uint8_t)(c - '0');
                val = (val << 4) | (int16_t)d;
            }
        } else {
            if (mode == 'O') p++;
            while (tok[p] >= '0' && tok[p] <= '7') {
                val = (val << 3) | (int16_t)(tok[p++] - '0');
            }
        }
        out_val->type = BASIC_TYPE_INT;
        out_val->ival = val;
        out_val->sval = 0;
        *pos = p;
        return 0;
    }

    /* Decimal or Floating Point number literal */
    if (basic_is_digit((char)tok[p]) || (tok[p] == '.' && basic_is_digit((char)tok[p + 1]))) {
        uint16_t scan = p;
        int has_fp = 0;
        while (basic_is_digit((char)tok[scan])) scan++;
        if (tok[scan] == '.') {
            has_fp = 1;
            scan++;
            while (basic_is_digit((char)tok[scan])) scan++;
        }
        if (tok[scan] == 'E' || tok[scan] == 'e' || tok[scan] == 'D' || tok[scan] == 'd') {
            has_fp = 1;
        }

        if (has_fp) {
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_parse_float_far(tok, &p);
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        } else {
            uint32_t val = 0;
            while (basic_is_digit((char)tok[p])) {
                val = (val << 3) + (val << 1) + (uint32_t)(tok[p++] - '0');
            }
            if (val > 32767) {
                out_val->type = BASIC_TYPE_FLOAT;
                out_val->fval = soft_from_uint((uint16_t)val);
                out_val->ival = 0;
            } else {
                out_val->type = BASIC_TYPE_INT;
                out_val->ival = (int16_t)val;
                out_val->fval = 0;
            }
            out_val->sval = 0;
            *pos = p;
            return 0;
        }
    }

    /* Built-in variable/function: ERR (token 0xD5) */
    if (tok[p] == TOK_ERR) {
        p++;
        out_val->type = BASIC_TYPE_INT;
        out_val->ival = s_err;
        out_val->fval = 0;
        out_val->sval = 0;
        *pos = p;
        return 0;
    }

    /* Built-in variable/function: ERL (token 0xD4) */
    if (tok[p] == TOK_ERL) {
        p++;
        out_val->type = BASIC_TYPE_INT;
        out_val->ival = (int16_t)s_erl;
        out_val->fval = 0;
        out_val->sval = 0;
        *pos = p;
        return 0;
    }

    /* Built-in variable/function: CSRLIN (token 0xDB) */
    if (tok[p] == TOK_CSRLIN) {
        uint16_t cur = os_get_cursor();
        p++;
        out_val->type = BASIC_TYPE_INT;
        if (s_screen_mode == 0) {
            out_val->ival = (int16_t)((cur >> 8) + 1);
        } else {
            out_val->ival = (int16_t)(s_gfx_row + 1);
        }
        out_val->fval = 0;
        out_val->sval = 0;
        *pos = p;
        return 0;
    }

    /* Built-in function: POINT(x, y) (token 0xDC) */
    if (tok[p] == TOK_POINT) {
        basic_val_t x_val, y_val;
        int16_t x, y;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') {
            os_puts("Syntax error in POINT (expected '(')\n");
            return -1;
        }
        p++;
        if (basic_eval_expr(tok, &p, &x_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') p++;
        if (basic_eval_expr(tok, &p, &y_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;

        x = val_to_int(&x_val);
        y = val_to_int(&y_val);

        out_val->type = BASIC_TYPE_INT;
        out_val->fval = 0;
        out_val->sval = 0;

        if (s_screen_mode == 0) {
            out_val->ival = -1;
        } else {
            int16_t max_x = (s_screen_mode == 2 || s_screen_mode == 12) ? 640 : 320;
            int16_t max_y = (s_screen_mode == 12) ? 480 : 200;
            if (x < 0 || x >= max_x || y < 0 || y >= max_y) {
                out_val->ival = -1;
            } else {
                out_val->ival = (int16_t)bios_read_pixel((uint16_t)x, (uint16_t)y);
            }
        }
        *pos = p;
        return 0;
    }

    /* Built-in function: INKEY$ (token 0xDE) */
    if (tok[p] == TOK_INKEY) {
        char *str = basic_alloc_temp_str();
        p++;
        basic_poll_keyboard();
        if (s_key_fifo_count > 0) {
            uint8_t ch = s_key_fifo[s_key_fifo_head];
            s_key_fifo_head = (s_key_fifo_head + 1) % BASIC_KEY_FIFO_SIZE;
            s_key_fifo_count--;
            str[0] = (char)ch;
            str[1] = '\0';
        } else {
            str[0] = '\0';
        }
        out_val->type = BASIC_TYPE_STR;
        out_val->sval = str;
        out_val->ival = 0;
        *pos = p;
        return 0;
    }

    /* Built-in function: INPUT$(n [, [#]f]) */
    if (tok[p] == TOK_INPUT && tok[p + 1] == '$') {
        basic_val_t n_val;
        int16_t n;
        uint8_t fnum = 0;
        char *str;
        int idx = 0;

        p += 2; /* skip TOK_INPUT and '$' */
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') {
            os_puts("Syntax error in INPUT$ (expected '(')\n");
            return -1;
        }
        p++;

        if (basic_eval_expr(tok, &p, &n_val) != 0) return -1;
        n = val_to_int(&n_val);

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            basic_val_t f_val;
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '#') p++;
            if (basic_eval_expr(tok, &p, &f_val) != 0) return -1;
            fnum = (uint8_t)val_to_int(&f_val);
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;

        if (n < 0 || n > BASIC_MAX_STR_LEN) {
            os_puts("Illegal function call in INPUT$\n");
            return -1;
        }

        str = basic_alloc_temp_str();
        if (fnum == 0) {
            while (idx < n) {
                uint16_t key = basic_getc();
                str[idx++] = (char)(key & 0xFF);
            }
        } else {
            while (idx < n) {
                int ch = basic_file_getc(fnum);
                if (ch < 0) break;
                str[idx++] = (char)ch;
            }
        }
        str[idx] = '\0';

        out_val->type = BASIC_TYPE_STR;
        out_val->sval = str;
        out_val->ival = idx;
        *pos = p;
        return 0;
    }

    /* Built-in function: STRING$(n, char_code_or_str) */
    if (tok[p] == TOK_STRING) {
        basic_val_t n_arg, c_arg;
        char *str = basic_alloc_temp_str();
        int16_t n;
        char ch = ' ';
        int i;
        p++; /* skip TOK_STRING */
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') {
            os_puts("Syntax error in STRING$ (expected '(')\n");
            return -1;
        }
        p++;
        if (basic_eval_expr(tok, &p, &n_arg) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            os_puts("Syntax error in STRING$ (expected ',')\n");
            return -1;
        }
        p++;
        if (basic_eval_expr(tok, &p, &c_arg) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;

        n = val_to_int(&n_arg);
        if (n < 0) n = 0;
        if (n > BASIC_MAX_STR_LEN) n = BASIC_MAX_STR_LEN;

        if (c_arg.type == BASIC_TYPE_STR) {
            ch = (c_arg.sval && c_arg.sval[0]) ? c_arg.sval[0] : ' ';
        } else {
            ch = (char)(val_to_int(&c_arg) & 0xFF);
        }

        for (i = 0; i < n; i++) str[i] = ch;
        str[n] = '\0';
        out_val->type = BASIC_TYPE_STR;
        out_val->sval = str;
        out_val->ival = 0;
        out_val->fval = 0;
        *pos = p;
        return 0;
    }

    /* Built-in function: INSTR([start,] str1, str2) */
    if (tok[p] == TOK_INSTR) {
        basic_val_t a1, a2, a3;
        int16_t start = 1;
        const char *s1 = "";
        const char *s2 = "";
        uint16_t l1, l2;
        int res = 0;
        uint16_t i, j;

        p++; /* skip TOK_INSTR */
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') {
            os_puts("Syntax error in INSTR (expected '(')\n");
            return -1;
        }
        p++;

        if (basic_eval_expr(tok, &p, &a1) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            os_puts("Syntax error in INSTR (expected ',')\n");
            return -1;
        }
        p++;

        if (a1.type == BASIC_TYPE_STR) {
            /* INSTR(str1, str2) */
            s1 = a1.sval ? a1.sval : "";
            if (basic_eval_expr(tok, &p, &a2) != 0) return -1;
            s2 = a2.sval ? a2.sval : "";
        } else {
            /* INSTR(start, str1, str2) */
            start = val_to_int(&a1);
            if (basic_eval_expr(tok, &p, &a2) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] != ',') {
                os_puts("Syntax error in INSTR (expected ',')\n");
                return -1;
            }
            p++;
            s1 = a2.sval ? a2.sval : "";
            if (basic_eval_expr(tok, &p, &a3) != 0) return -1;
            s2 = a3.sval ? a3.sval : "";
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;

        l1 = basic_strlen(s1);
        l2 = basic_strlen(s2);

        if (l2 == 0) {
            res = (start > 0 && (uint16_t)start <= l1 + 1) ? start : 1;
        } else if (start <= 0 || (uint16_t)start > l1 || l2 > l1) {
            res = 0;
        } else {
            uint16_t s_idx = (uint16_t)(start - 1);
            for (i = s_idx; i + l2 <= l1; i++) {
                int match = 1;
                for (j = 0; j < l2; j++) {
                    if (s1[i + j] != s2[j]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    res = (int)(i + 1);
                    break;
                }
            }
        }

        out_val->type = BASIC_TYPE_INT;
        out_val->ival = (int16_t)res;
        out_val->fval = 0;
        out_val->sval = 0;
        *pos = p;
        return 0;
    }

    /* Built-in 2-byte functions with 0xFD prefix (Phase 11: Binary conversion) */
    if (tok[p] == 0xFD) {
        uint8_t fn = tok[p + 1];
        p += 2;

        /* CVI(S$) - 2-byte string to 16-bit signed integer */
        if (fn == (uint8_t)(TOK_CVI & 0xFF)) {
            basic_val_t arg;
            int16_t iv;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            if (arg.type != BASIC_TYPE_STR || !arg.sval) {
                return basic_error(13, "Type mismatch\n");
            }
            iv = (int16_t)((uint8_t)arg.sval[0] | ((uint16_t)(uint8_t)arg.sval[1] << 8));
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = iv;
            out_val->fval = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* CVS(S$) - 4-byte string to 32-bit single-precision float */
        if (fn == (uint8_t)(TOK_CVS & 0xFF)) {
            basic_val_t arg;
            basic_float_t fv;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            if (arg.type != BASIC_TYPE_STR || !arg.sval) {
                return basic_error(13, "Type mismatch\n");
            }
            basic_memcpy(&fv, arg.sval, 4);
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = fv;
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* CVD(S$) - 8-byte string to double / float */
        if (fn == (uint8_t)(TOK_CVD & 0xFF)) {
            basic_val_t arg;
            basic_float_t fv;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            if (arg.type != BASIC_TYPE_STR || !arg.sval) {
                return basic_error(13, "Type mismatch\n");
            }
            basic_memcpy(&fv, arg.sval, 4);
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = fv;
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* MKI$(X%) - 16-bit integer to 2-byte string */
        if (fn == (uint8_t)(TOK_MKI & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            int16_t iv;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            iv = val_to_int(&arg);
            str[0] = (char)(iv & 0xFF);
            str[1] = (char)((iv >> 8) & 0xFF);
            str[2] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 2;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* MKS$(X!) - 32-bit float to 4-byte string */
        if (fn == (uint8_t)(TOK_MKS & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            basic_float_t fv;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            fv = (arg.type == BASIC_TYPE_FLOAT) ? arg.fval : soft_from_int(val_to_int(&arg));
            basic_memcpy(str, &fv, 4);
            str[4] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 4;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* MKD$(X#) - 8-byte double to 8-byte string */
        if (fn == (uint8_t)(TOK_MKD & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            basic_float_t fv;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            fv = (arg.type == BASIC_TYPE_FLOAT) ? arg.fval : soft_from_int(val_to_int(&arg));
            basic_memset(str, 0, 8);
            basic_memcpy(str, &fv, 4);
            str[8] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 8;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* Not a recognized 0xFD function */
        p -= 2;
    }

    /* Built-in 2-byte functions with 0xFE prefix (Phase 7) */
    if (tok[p] == 0xFE) {
        uint8_t fn = tok[p + 1];
        p += 2;

        /* TIMER (0xFE94) */
        if (fn == (uint8_t)(TOK_TIMER & 0xFF)) {
            uint32_t ticks = *((const uint32_t __far *)0x0040006CUL);
            basic_float_t f_ticks = soft_from_uint32(ticks);
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') {
                p++;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] == ')') p++;
            }
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_mul(f_ticks, 0x3D60FEB3UL); /* * 0.05492549 */
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* DATE$ (0xFE8D) */
        if (fn == (uint8_t)(TOK_DATE & 0xFF)) {
            char *str = basic_alloc_temp_str();
            basic_get_date_str(str);
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* TIME$ (0xFE8E) */
        if (fn == (uint8_t)(TOK_TIME & 0xFF)) {
            char *str = basic_alloc_temp_str();
            basic_get_time_str(str);
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* Not a recognized 0xFE function */
        p -= 2;
    }

    /* Built-in 2-byte functions with 0xFF prefix */
    if (tok[p] == 0xFF) {
        uint8_t fn = tok[p + 1];
        p += 2;

        /* RND [(expr)] */
        if (fn == (uint8_t)(TOK_RND & 0xFF)) {
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') {
                basic_val_t dummy;
                p++;
                if (basic_eval_expr(tok, &p, &dummy) != 0) return -1;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] == ')') p++;
                if (dummy.type == BASIC_TYPE_FLOAT && (dummy.fval & 0x80000000UL)) {
                    s_rnd_seed = (uint16_t)soft_to_int(dummy.fval);
                } else if (dummy.type == BASIC_TYPE_INT && dummy.ival < 0) {
                    s_rnd_seed = (uint16_t)dummy.ival;
                }
            }
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_div(soft_from_uint((uint16_t)basic_rand()), soft_from_uint(32768U));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') {
            os_puts("Syntax error (expected '(')\n");
            return -1;
        }
        p++;

        /* PEEK(addr) */
        if (fn == (uint8_t)(TOK_PEEK & 0xFF)) {
            basic_val_t arg;
            uint16_t addr;
            uint8_t val;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            addr = (uint16_t)val_to_int(&arg);
            val = *((const uint8_t __far *)MK_FP(s_def_seg, addr));
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = (int16_t)val;
            out_val->fval = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* INP(port) */
        if (fn == (uint8_t)(TOK_INP & 0xFF)) {
            basic_val_t arg;
            uint16_t port;
            uint8_t val;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            port = (uint16_t)val_to_int(&arg);
            val = inb(port);
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = (int16_t)val;
            out_val->fval = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* POS(dummy) */
        if (fn == (uint8_t)(TOK_POS & 0xFF)) {
            basic_val_t dummy;
            uint16_t cur;
            if (basic_eval_expr(tok, &p, &dummy) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            cur = os_get_cursor();
            out_val->type = BASIC_TYPE_INT;
            if (s_screen_mode == 0) {
                out_val->ival = (int16_t)((cur & 0xFF) + 1);
            } else {
                out_val->ival = (int16_t)(s_gfx_col + 1);
            }
            out_val->fval = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* EOF(filenum) */
        if (fn == (uint8_t)(TOK_EOF & 0xFF)) {
            basic_val_t arg;
            uint8_t fnum;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            fnum = (uint8_t)arg.ival;
            out_val->type = BASIC_TYPE_INT;
            if (fnum >= 1 && fnum <= BASIC_MAX_FILES && s_files[fnum - 1].is_open) {
                basic_file_t *f = &s_files[fnum - 1];
                if (f->mode == 'R') {
                    out_val->ival = f->eof ? -1 : 0;
                } else {
                    int is_eof = f->eof || (f->buf_pos >= f->buf_len && f->file_pos >= f->file_len);
                    out_val->ival = is_eof ? -1 : 0;
                }
            } else {
                out_val->ival = -1;
            }
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* LOC(filenum) - Phase 11 */
        if (fn == (uint8_t)(TOK_LOC & 0xFF)) {
            basic_val_t arg;
            uint8_t fnum;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            fnum = (uint8_t)arg.ival;
            out_val->type = BASIC_TYPE_INT;
            if (fnum >= 1 && fnum <= BASIC_MAX_FILES && s_files[fnum - 1].is_open) {
                basic_file_t *f = &s_files[fnum - 1];
                if (f->mode == 'R') {
                    out_val->ival = (int16_t)f->current_record;
                } else {
                    out_val->ival = (int16_t)(f->file_pos >> 7);
                }
            } else {
                out_val->ival = 0;
            }
            out_val->sval = 0;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* LOF(filenum) - Phase 11 */
        if (fn == (uint8_t)(TOK_LOF & 0xFF)) {
            basic_val_t arg;
            uint8_t fnum;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '(') p++;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            fnum = (uint8_t)arg.ival;
            if (fnum >= 1 && fnum <= BASIC_MAX_FILES && s_files[fnum - 1].is_open) {
                basic_file_t *f = &s_files[fnum - 1];
                long sz = os_file_size(f->filename);
                if (sz < 0) sz = (long)f->file_len;
                if (sz <= 32767L) {
                    out_val->type = BASIC_TYPE_INT;
                    out_val->ival = (int16_t)sz;
                } else {
                    out_val->type = BASIC_TYPE_FLOAT;
                    out_val->fval = soft_from_int((int32_t)sz);
                }
            } else {
                out_val->type = BASIC_TYPE_INT;
                out_val->ival = 0;
            }
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* SPACE$(N) */
        if (fn == (uint8_t)(TOK_SPACE & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            int16_t n;
            int i;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            n = val_to_int(&arg);
            if (n < 0) n = 0;
            if (n > BASIC_MAX_STR_LEN) n = BASIC_MAX_STR_LEN;
            for (i = 0; i < n; i++) str[i] = ' ';
            str[n] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* HEX$(N) */
        if (fn == (uint8_t)(TOK_HEX & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            uint16_t v;
            int sp = 0;
            char tmp[5];
            int tp = 0;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            v = (uint16_t)val_to_int(&arg);
            if (v == 0) {
                str[sp++] = '0';
            } else {
                while (v > 0) {
                    uint8_t d = (uint8_t)(v & 0x0F);
                    tmp[tp++] = (d < 10) ? (char)('0' + d) : (char)('A' + d - 10);
                    v >>= 4;
                }
                while (tp > 0) str[sp++] = tmp[--tp];
            }
            str[sp] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* OCT$(N) */
        if (fn == (uint8_t)(TOK_OCT & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            uint16_t v;
            int sp = 0;
            char tmp[7];
            int tp = 0;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            v = (uint16_t)val_to_int(&arg);
            if (v == 0) {
                str[sp++] = '0';
            } else {
                while (v > 0) {
                    uint8_t d = (uint8_t)(v & 0x07);
                    tmp[tp++] = (char)('0' + d);
                    v >>= 3;
                }
                while (tp > 0) str[sp++] = tmp[--tp];
            }
            str[sp] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* CHR$(X) */
        if (fn == (uint8_t)(TOK_CHR & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            str[0] = (char)(arg.ival & 0xFF);
            str[1] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            *pos = p;
            return 0;
        }

        /* ASC(S$) */
        if (fn == (uint8_t)(TOK_ASC & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = (arg.sval && arg.sval[0]) ? (uint8_t)arg.sval[0] : 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* LEN(S$) */
        if (fn == (uint8_t)(TOK_LEN & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = basic_strlen(arg.sval);
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* VAL(S$) */
        if (fn == (uint8_t)(TOK_VAL & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            if (arg.sval) {
                uint16_t sp = 0;
                int has_dot = 0;
                while (arg.sval[sp] == ' ' || arg.sval[sp] == '\t') sp++;
                while (arg.sval[sp] != '\0') {
                    if (arg.sval[sp] == '.' || arg.sval[sp] == 'E' || arg.sval[sp] == 'e') has_dot = 1;
                    sp++;
                }
                sp = 0;
                if (has_dot) {
                    out_val->type = BASIC_TYPE_FLOAT;
                    out_val->fval = basic_parse_float_str(arg.sval, &sp);
                    out_val->ival = 0;
                } else {
                    out_val->type = BASIC_TYPE_INT;
                    out_val->ival = basic_atoi(arg.sval);
                    out_val->fval = 0;
                }
            } else {
                out_val->type = BASIC_TYPE_INT;
                out_val->ival = 0;
                out_val->fval = 0;
            }
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* STR$(X) */
        if (fn == (uint8_t)(TOK_STR & 0xFF)) {
            basic_val_t arg;
            char *str = basic_alloc_temp_str();
            int16_t v;
            uint16_t sp = 0;
            char buf[8];
            int bp = 0;

            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;

            if (arg.type == BASIC_TYPE_FLOAT) {
                basic_format_float(arg.fval, str, BASIC_MAX_STR_LEN);
            } else if (arg.type == BASIC_TYPE_UINT) {
                uint16_t uv = (uint16_t)arg.ival;
                str[sp++] = ' ';
                if (uv == 0) {
                    str[sp++] = '0';
                } else {
                    while (uv > 0) {
                        buf[bp++] = (char)('0' + (uv % 10));
                        uv /= 10;
                    }
                    while (bp > 0) {
                        str[sp++] = buf[--bp];
                    }
                }
                str[sp] = '\0';
            } else {
                v = arg.ival;
                if (v >= 0) {
                    str[sp++] = ' ';
                } else {
                    str[sp++] = '-';
                    v = -v;
                }
                if (v == 0) {
                    str[sp++] = '0';
                } else {
                    while (v > 0) {
                        buf[bp++] = (char)('0' + (v % 10));
                        v /= 10;
                    }
                    while (bp > 0) {
                        str[sp++] = buf[--bp];
                    }
                }
                str[sp] = '\0';
            }
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            out_val->fval = 0;
            *pos = p;
            return 0;
        }

        /* ABS(X) */
        if (fn == (uint8_t)(TOK_ABS & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            if (arg.type == BASIC_TYPE_FLOAT) {
                out_val->type = BASIC_TYPE_FLOAT;
                out_val->fval = arg.fval & 0x7FFFFFFFUL;
                out_val->ival = 0;
            } else {
                out_val->type = BASIC_TYPE_INT;
                out_val->ival = (arg.ival < 0) ? -arg.ival : arg.ival;
                out_val->fval = 0;
            }
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* SGN(X) */
        if (fn == (uint8_t)(TOK_SGN & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_INT;
            if (arg.type == BASIC_TYPE_FLOAT) {
                out_val->ival = (arg.fval == 0 || (arg.fval & 0x7FFFFFFFUL) == 0) ? 0 :
                                ((arg.fval & 0x80000000UL) ? -1 : 1);
            } else {
                out_val->ival = (arg.ival > 0) ? 1 : ((arg.ival < 0) ? -1 : 0);
            }
            out_val->fval = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* SQR(X) */
        if (fn == (uint8_t)(TOK_SQR & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_sqr(val_to_float(&arg));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* SIN(X) */
        if (fn == (uint8_t)(TOK_SIN & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_sin(val_to_float(&arg));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* COS(X) */
        if (fn == (uint8_t)(TOK_COS & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_cos(val_to_float(&arg));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* TAN(X) */
        if (fn == (uint8_t)(TOK_TAN & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_tan(val_to_float(&arg));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* ATN(X) */
        if (fn == (uint8_t)(TOK_ATN & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_atn(val_to_float(&arg));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* LOG(X) */
        if (fn == (uint8_t)(TOK_LOG & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_log(val_to_float(&arg));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* EXP(X) */
        if (fn == (uint8_t)(TOK_EXP & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_exp(val_to_float(&arg));
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* INT(X) */
        if (fn == (uint8_t)(TOK_INT & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            if (arg.type == BASIC_TYPE_FLOAT) {
                int16_t iv = soft_to_int(arg.fval);
                basic_float_t f_iv = soft_from_int(iv);
                if (basic_float_cmp(arg.fval, f_iv) < 0) iv--;
                out_val->type = BASIC_TYPE_INT;
                out_val->ival = iv;
                out_val->fval = 0;
            } else {
                out_val->type = BASIC_TYPE_INT;
                out_val->ival = arg.ival;
                out_val->fval = 0;
            }
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* FIX(X) */
        if (fn == (uint8_t)(TOK_FIX & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = val_to_int(&arg);
            out_val->fval = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* CINT(X) */
        if (fn == (uint8_t)(TOK_CINT & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = val_to_int(&arg);
            out_val->fval = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* CSNG(X) */
        if (fn == (uint8_t)(TOK_CSNG & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = val_to_float(&arg);
            out_val->ival = 0;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* FRE(X) or FRE(X$) */
        if (fn == (uint8_t)(TOK_FRE & 0xFF)) {
            basic_val_t arg;
            uint16_t free_bytes;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;

            free_bytes = basic_get_free_mem();

            out_val->type = BASIC_TYPE_UINT;
            out_val->ival = (int16_t)free_bytes;
            out_val->sval = 0;
            *pos = p;
            return 0;
        }

        /* LEFT$(S$, N) */
        if (fn == (uint8_t)(TOK_LEFT & 0xFF)) {
            basic_val_t s_arg, n_arg;
            char *str = basic_alloc_temp_str();
            uint16_t n, i;
            if (basic_eval_expr(tok, &p, &s_arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') p++;
            if (basic_eval_expr(tok, &p, &n_arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;

            n = (n_arg.ival > 0) ? (uint16_t)n_arg.ival : 0;
            for (i = 0; i < n && s_arg.sval && s_arg.sval[i] != '\0'; i++) {
                str[i] = s_arg.sval[i];
            }
            str[i] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            *pos = p;
            return 0;
        }

        /* RIGHT$(S$, N) */
        if (fn == (uint8_t)(TOK_RIGHT & 0xFF)) {
            basic_val_t s_arg, n_arg;
            char *str = basic_alloc_temp_str();
            uint16_t slen, n, start, i;
            if (basic_eval_expr(tok, &p, &s_arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') p++;
            if (basic_eval_expr(tok, &p, &n_arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;

            slen = basic_strlen(s_arg.sval);
            n = (n_arg.ival > 0) ? (uint16_t)n_arg.ival : 0;
            start = (n >= slen) ? 0 : (slen - n);
            for (i = 0; start + i < slen; i++) {
                str[i] = s_arg.sval[start + i];
            }
            str[i] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            *pos = p;
            return 0;
        }

        /* MID$(S$, P [, N]) */
        if (fn == (uint8_t)(TOK_MID & 0xFF)) {
            basic_val_t s_arg, p_arg, n_arg;
            char *str = basic_alloc_temp_str();
            uint16_t slen, start_p, n, i;
            if (basic_eval_expr(tok, &p, &s_arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') p++;
            if (basic_eval_expr(tok, &p, &p_arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') {
                p++;
                if (basic_eval_expr(tok, &p, &n_arg) != 0) return -1;
                n = (n_arg.ival > 0) ? (uint16_t)n_arg.ival : 0;
            } else {
                n = 255;
            }
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;

            slen = basic_strlen(s_arg.sval);
            start_p = (p_arg.ival > 0) ? (uint16_t)(p_arg.ival - 1) : 0;
            for (i = 0; i < n && start_p + i < slen; i++) {
                str[i] = s_arg.sval[start_p + i];
            }
            str[i] = '\0';
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            *pos = p;
            return 0;
        }
    }

    /* Variable or Array lookup */
    if (basic_is_alpha((char)tok[p])) {
        char var_name[BASIC_MAX_VARNAME];
        if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
            os_puts("Syntax error\n");
            return -1;
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == '(') {
            basic_val_t i1_val, i2_val;
            basic_array_t *arr;
            uint8_t num_dims = 1;
            p++;
            if (basic_eval_expr(tok, &p, &i1_val) != 0) return -1;
            i2_val.ival = 0;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') {
                p++;
                num_dims = 2;
                if (basic_eval_expr(tok, &p, &i2_val) != 0) return -1;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
            }
            if (tok[p] != ')') {
                os_puts("Syntax error in array subscript (expected ')')\n");
                return -1;
            }
            p++;

            arr = basic_find_array(var_name);
            if (!arr) {
                arr = basic_create_default_array(var_name, num_dims);
                if (!arr) return -1;
            }
            if (basic_read_array_elem(arr, i1_val.ival, i2_val.ival, out_val) != 0) {
                return -1;
            }
            *pos = p;
            return 0;
        }

        {
            basic_var_t *var = basic_find_var(var_name);
            if (var) {
                out_val->type = var->type;
                out_val->ival = (var->type == BASIC_TYPE_STR) ? (int16_t)var->slen : var->ival;
                out_val->fval = var->fval;
                out_val->sval = var->sval;
            } else {
                uint16_t len = basic_strlen(var_name);
                if (len > 0 && var_name[len - 1] == '$') {
                    char *str = basic_alloc_temp_str();
                    out_val->type = BASIC_TYPE_STR;
                    out_val->sval = str;
                    out_val->ival = 0;
                    out_val->fval = 0;
                } else if (len > 0 && var_name[len - 1] == '%') {
                    out_val->type = BASIC_TYPE_INT;
                    out_val->ival = 0;
                    out_val->fval = 0;
                    out_val->sval = 0;
                } else {
                    out_val->type = BASIC_TYPE_FLOAT;
                    out_val->ival = 0;
                    out_val->fval = 0;
                    out_val->sval = 0;
                }
            }
            *pos = p;
            return 0;
        }
    }

    os_puts("Syntax error in expression\n");
    return -1;
}

static int eval_unary(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    uint16_t p = *pos;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_MINUS || tok[p] == '-') {
        p++;
        *pos = p;
        if (eval_unary(tok, pos, out_val) != 0) return -1;
        if (out_val->type == BASIC_TYPE_FLOAT) {
            out_val->fval ^= 0x80000000UL;
        } else {
            out_val->ival = -out_val->ival;
        }
        return 0;
    }
    if (tok[p] == TOK_PLUS || tok[p] == '+') {
        p++;
        *pos = p;
        return eval_unary(tok, pos, out_val);
    }
    return eval_primary(tok, pos, out_val);
}

static int eval_power(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_unary(tok, pos, out_val) != 0) return -1;

    for (;;) {
        uint16_t p = *pos;
        basic_val_t right;
        basic_float_t lf, rf;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != TOK_CARET && tok[p] != '^') break;

        p++;
        *pos = p;
        if (eval_unary(tok, pos, &right) != 0) return -1;

        lf = val_to_float(out_val);
        rf = val_to_float(&right);
        out_val->type = BASIC_TYPE_FLOAT;
        out_val->fval = basic_float_pow(lf, rf);
        out_val->ival = 0;
    }
    return 0;
}

static int eval_mul(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_power(tok, pos, out_val) != 0) return -1;

    for (;;) {
        uint16_t p = *pos;
        uint8_t op;
        basic_val_t right;
        basic_float_t lf, rf;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        op = tok[p];

        if (op != TOK_TIMES && op != TOK_DIV && op != TOK_INTDIV && op != TOK_MOD &&
            op != '*' && op != '/' && op != '\\') {
            break;
        }

        p++;
        *pos = p;
        if (eval_power(tok, pos, &right) != 0) return -1;

        if (op == TOK_DIV || op == '/') {
            lf = val_to_float(out_val);
            rf = val_to_float(&right);
            if (rf == 0) {
                return basic_error(11, "Division by zero\n");
            }
            out_val->type = BASIC_TYPE_FLOAT;
            out_val->fval = basic_float_div(lf, rf);
            out_val->ival = 0;
        } else if (op == TOK_TIMES || op == '*') {
            if (out_val->type == BASIC_TYPE_FLOAT || right.type == BASIC_TYPE_FLOAT) {
                lf = val_to_float(out_val);
                rf = val_to_float(&right);
                out_val->type = BASIC_TYPE_FLOAT;
                out_val->fval = basic_float_mul(lf, rf);
                out_val->ival = 0;
            } else {
                out_val->ival = out_val->ival * right.ival;
            }
        } else if (op == TOK_INTDIV || op == '\\') {
            int16_t ri = val_to_int(&right);
            if (ri == 0) {
                return basic_error(11, "Division by zero\n");
            }
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = val_to_int(out_val) / ri;
            out_val->fval = 0;
        } else if (op == TOK_MOD) {
            int16_t ri = val_to_int(&right);
            if (ri == 0) {
                return basic_error(11, "Division by zero\n");
            }
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = val_to_int(out_val) % ri;
            out_val->fval = 0;
        }
    }
    return 0;
}

static int eval_add(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_mul(tok, pos, out_val) != 0) return -1;

    for (;;) {
        uint16_t p = *pos;
        uint8_t op;
        basic_val_t right;
        basic_float_t lf, rf;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        op = tok[p];

        if (op != TOK_PLUS && op != TOK_MINUS && op != '+' && op != '-') {
            break;
        }

        p++;
        *pos = p;
        if (eval_mul(tok, pos, &right) != 0) return -1;

        if (op == TOK_PLUS || op == '+') {
            if (out_val->type == BASIC_TYPE_STR && right.type == BASIC_TYPE_STR) {
                char *new_str = basic_alloc_temp_str();
                basic_strcpy(new_str, out_val->sval);
                basic_strcat(new_str, right.sval, BASIC_MAX_STR_LEN);
                out_val->sval = new_str;
            } else if (out_val->type == BASIC_TYPE_FLOAT || right.type == BASIC_TYPE_FLOAT) {
                lf = val_to_float(out_val);
                rf = val_to_float(&right);
                out_val->type = BASIC_TYPE_FLOAT;
                out_val->fval = basic_float_add(lf, rf);
                out_val->ival = 0;
            } else if (out_val->type == BASIC_TYPE_UINT || right.type == BASIC_TYPE_UINT) {
                out_val->type = BASIC_TYPE_UINT;
                out_val->ival = out_val->ival + right.ival;
            } else {
                out_val->ival = out_val->ival + right.ival;
            }
        } else {
            if (out_val->type == BASIC_TYPE_FLOAT || right.type == BASIC_TYPE_FLOAT) {
                lf = val_to_float(out_val);
                rf = val_to_float(&right);
                out_val->type = BASIC_TYPE_FLOAT;
                out_val->fval = basic_float_sub(lf, rf);
                out_val->ival = 0;
            } else {
                out_val->ival = out_val->ival - right.ival;
            }
        }
    }
    return 0;
}

static int eval_rel(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_add(tok, pos, out_val) != 0) return -1;

    for (;;) {
        int cmp_op = basic_match_cmp_op(tok, pos);
        basic_val_t right;
        int cond = 0;

        if (cmp_op == OP_CMP_NONE) break;

        if (eval_add(tok, pos, &right) != 0) return -1;

        if (out_val->type == BASIC_TYPE_STR && right.type == BASIC_TYPE_STR) {
            int cmp = basic_strcmp(out_val->sval, right.sval);
            switch (cmp_op) {
                case OP_CMP_EQ: cond = (cmp == 0); break;
                case OP_CMP_NE: cond = (cmp != 0); break;
                case OP_CMP_LT: cond = (cmp < 0); break;
                case OP_CMP_GT: cond = (cmp > 0); break;
                case OP_CMP_LE: cond = (cmp <= 0); break;
                case OP_CMP_GE: cond = (cmp >= 0); break;
            }
        } else if (out_val->type == BASIC_TYPE_FLOAT || right.type == BASIC_TYPE_FLOAT) {
            int cmp = basic_float_cmp(val_to_float(out_val), val_to_float(&right));
            switch (cmp_op) {
                case OP_CMP_EQ: cond = (cmp == 0); break;
                case OP_CMP_NE: cond = (cmp != 0); break;
                case OP_CMP_LT: cond = (cmp < 0); break;
                case OP_CMP_GT: cond = (cmp > 0); break;
                case OP_CMP_LE: cond = (cmp <= 0); break;
                case OP_CMP_GE: cond = (cmp >= 0); break;
            }
        } else if (out_val->type == BASIC_TYPE_UINT || right.type == BASIC_TYPE_UINT) {
            uint16_t u1 = (uint16_t)out_val->ival;
            uint16_t u2 = (uint16_t)right.ival;
            switch (cmp_op) {
                case OP_CMP_EQ: cond = (u1 == u2); break;
                case OP_CMP_NE: cond = (u1 != u2); break;
                case OP_CMP_LT: cond = (u1 < u2); break;
                case OP_CMP_GT: cond = (u1 > u2); break;
                case OP_CMP_LE: cond = (u1 <= u2); break;
                case OP_CMP_GE: cond = (u1 >= u2); break;
            }
        } else {
            switch (cmp_op) {
                case OP_CMP_EQ: cond = (out_val->ival == right.ival); break;
                case OP_CMP_NE: cond = (out_val->ival != right.ival); break;
                case OP_CMP_LT: cond = (out_val->ival < right.ival); break;
                case OP_CMP_GT: cond = (out_val->ival > right.ival); break;
                case OP_CMP_LE: cond = (out_val->ival <= right.ival); break;
                case OP_CMP_GE: cond = (out_val->ival >= right.ival); break;
            }
        }

        out_val->type = BASIC_TYPE_INT;
        out_val->ival = cond ? -1 : 0;
        out_val->fval = 0;
        out_val->sval = 0;
    }
    return 0;
}

static int eval_not(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    uint16_t p = *pos;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_NOT) {
        p++;
        *pos = p;
        if (eval_not(tok, pos, out_val) != 0) return -1;
        out_val->ival = ~val_to_int(out_val);
        out_val->type = BASIC_TYPE_INT;
        out_val->fval = 0;
        out_val->sval = 0;
        return 0;
    }
    return eval_rel(tok, pos, out_val);
}

static int eval_and(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_not(tok, pos, out_val) != 0) return -1;

    for (;;) {
        uint16_t p = *pos;
        basic_val_t right;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != TOK_AND) break;

        p++;
        *pos = p;
        if (eval_not(tok, pos, &right) != 0) return -1;
        out_val->ival = val_to_int(out_val) & val_to_int(&right);
        out_val->type = BASIC_TYPE_INT;
        out_val->fval = 0;
        out_val->sval = 0;
    }
    return 0;
}

static int eval_or(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_and(tok, pos, out_val) != 0) return -1;

    for (;;) {
        uint16_t p = *pos;
        uint8_t op;
        basic_val_t right;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        op = tok[p];
        if (op != TOK_OR && op != TOK_XOR) break;

        p++;
        *pos = p;
        if (eval_and(tok, pos, &right) != 0) return -1;

        if (op == TOK_OR) {
            out_val->ival = val_to_int(out_val) | val_to_int(&right);
        } else {
            out_val->ival = val_to_int(out_val) ^ val_to_int(&right);
        }
        out_val->type = BASIC_TYPE_INT;
        out_val->fval = 0;
        out_val->sval = 0;
    }
    return 0;
}

static int basic_eval_expr(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    return eval_or(tok, pos, out_val);
}

/* --------------------------------------------------------------------------
 * Persistence & File Commands (SAVE, LOAD, FILES, OPEN, CLOSE)
 * -------------------------------------------------------------------------- */

static int basic_exec_save(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t fname_val;
    char fname[32];
    int ascii_mode = 0;

    p++; /* skip TOK_SAVE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &fname_val) != 0) return -1;
    if (fname_val.type != BASIC_TYPE_STR || !fname_val.sval) {
        os_puts("Type mismatch in SAVE filename\n");
        return -1;
    }
    basic_normalize_filename(fname, fname_val.sval, sizeof(fname));

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == 'A' || tok[p] == 'a') {
            ascii_mode = 1;
            p++;
        } else if (tok[p] == 'P' || tok[p] == 'p') {
            p++;
        } else {
            os_puts("Syntax error in SAVE option\n");
            return -1;
        }
    }

    if (ascii_mode) {
        uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
        uint16_t cur = s_prog_head;
        char line_buf[BASIC_MAX_LINE_LEN + 4];

        os_file_write(fname, (const void __far *)FP_HERE(""), 0);

        while (cur != 0) {
            uint16_t next = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
            uint16_t num  = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);
            uint16_t len  = basic_detokenize_line(num, &mem[cur + 4], line_buf, sizeof(line_buf) - 2);

            line_buf[len++] = '\r';
            line_buf[len++] = '\n';
            line_buf[len]   = '\0';

            os_file_append(fname, (const void __far *)FP_HERE(line_buf), len);
            cur = next;
        }
    } else {
        uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
        uint16_t cur = s_prog_head;
        uint16_t out_off = (s_prog_tail + 15) & ~15;
        uint16_t p_out = out_off;

        if ((uint32_t)out_off + 2048 >= 65000L) {
            os_puts("Out of memory in SAVE\n");
            return -1;
        }

        mem[p_out++] = 0xFF; /* Binary GW-BASIC magic header */

        while (cur != 0) {
            uint16_t next = (uint16_t)mem[cur] | ((uint16_t)mem[cur + 1] << 8);
            uint16_t num  = (uint16_t)mem[cur + 2] | ((uint16_t)mem[cur + 3] << 8);
            uint16_t tok_p = cur + 4;

            mem[p_out++] = 0x01; /* non-zero link placeholder */
            mem[p_out++] = 0x00;
            mem[p_out++] = (uint8_t)(num & 0xFF);
            mem[p_out++] = (uint8_t)(num >> 8);

            while (mem[tok_p] != 0x00 && p_out < 65000U) {
                mem[p_out++] = mem[tok_p++];
            }
            mem[p_out++] = 0x00;
            cur = next;
        }

        mem[p_out++] = 0x00;
        mem[p_out++] = 0x00;

        os_file_write(fname, (const void __far *)MK_FP(s_basic_seg, out_off), p_out - out_off);
    }

    *pos = p;
    return 0;
}

static int basic_exec_load(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t fname_val;
    char fname[32];
    int run_after = 0;
    long sz;
    uint8_t __far *mem;
    uint16_t load_seg_off = 0x8000;
    uint16_t bytes_read;

    p++; /* skip TOK_LOAD */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &fname_val) != 0) return -1;
    if (fname_val.type != BASIC_TYPE_STR || !fname_val.sval) {
        os_puts("Type mismatch in LOAD filename\n");
        return -1;
    }
    basic_normalize_filename(fname, fname_val.sval, sizeof(fname));

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == 'R' || tok[p] == 'r') {
            run_after = 1;
            p++;
        } else if (tok[p] == 'A' || tok[p] == 'a') {
            /* Optional ASCII specifier (ASCII/binary is autodetected) */
            p++;
        } else {
            os_puts("Syntax error in LOAD option\n");
            return -1;
        }
    }

    sz = os_file_size(fname);
    if (sz <= 0 || sz > 32000L) {
        os_puts("File not found\n");
        return -1;
    }

    mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    bytes_read = os_file_read(fname, (void __far *)MK_FP(s_basic_seg, load_seg_off), (uint16_t)sz);
    if (bytes_read == 0) {
        os_puts("Read error\n");
        return -1;
    }

    basic_new();

    if (mem[load_seg_off] == 0xFF) {
        /* Binary GW-BASIC file */
        uint16_t fp = load_seg_off + 1;
        while (fp + 4 < load_seg_off + bytes_read) {
            uint16_t next_ptr = (uint16_t)mem[fp] | ((uint16_t)mem[fp + 1] << 8);
            uint16_t num;
            uint16_t tok_start;
            uint16_t tok_len = 0;

            if (next_ptr == 0) break;

            num = (uint16_t)mem[fp + 2] | ((uint16_t)mem[fp + 3] << 8);
            tok_start = fp + 4;
            while (mem[tok_start + tok_len] != 0x00 && tok_start + tok_len < load_seg_off + bytes_read) {
                tok_len++;
            }

            {
                uint8_t tok_tmp[BASIC_MAX_LINE_LEN];
                uint16_t k;
                for (k = 0; k < tok_len && k < sizeof(tok_tmp) - 1; k++) {
                    tok_tmp[k] = mem[tok_start + k];
                }
                tok_tmp[k] = 0x00;
                basic_insert_line(num, tok_tmp, k);
            }
            fp = tok_start + tok_len + 1;
        }
    } else {
        /* ASCII text file */
        uint16_t fp = load_seg_off;
        char line_buf[BASIC_MAX_LINE_LEN];
        uint8_t tok_tmp[BASIC_MAX_LINE_LEN];

        while (fp < load_seg_off + bytes_read) {
            uint16_t lp = 0;
            uint16_t sp = 0;

            while (fp < load_seg_off + bytes_read && mem[fp] != '\r' && mem[fp] != '\n') {
                if (lp + 1 < sizeof(line_buf)) line_buf[lp++] = (char)mem[fp];
                fp++;
            }
            line_buf[lp] = '\0';
            while (fp < load_seg_off + bytes_read && (mem[fp] == '\r' || mem[fp] == '\n')) fp++;

            while (line_buf[sp] == ' ' || line_buf[sp] == '\t') sp++;
            if (basic_is_digit(line_buf[sp])) {
                uint16_t num = basic_parse_uint(line_buf, &sp);
                uint16_t tlen;
                while (line_buf[sp] == ' ' || line_buf[sp] == '\t') sp++;
                tlen = basic_tokenize(&line_buf[sp], tok_tmp, sizeof(tok_tmp));
                basic_insert_line(num, tok_tmp, tlen);
            }
        }
    }

    if (run_after) {
        basic_cmd_run();
    }

    *pos = p;
    return 0;
}

static int basic_exec_open(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t arg1;
    uint8_t mode = 'I';
    char fname[32];
    uint8_t fnum = 1;
    uint16_t reclen = 128;
    long sz;

    p++; /* skip TOK_OPEN */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &arg1) != 0) return -1;
    if (arg1.type != BASIC_TYPE_STR || !arg1.sval) {
        os_puts("Type mismatch in OPEN\n");
        return -1;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == ',') {
        /* Syntax 1: OPEN "mode", #n, "filename" [, reclen] */
        basic_val_t fnum_val, fname_val;
        mode = (uint8_t)basic_toupper(arg1.sval[0]);
        p++; /* skip ',' */
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == '#') p++;

        if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
        fnum = (uint8_t)fnum_val.ival;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            os_puts("Syntax error in OPEN (expected ',')\n");
            return -1;
        }
        p++;

        if (basic_eval_expr(tok, &p, &fname_val) != 0) return -1;
        if (fname_val.type != BASIC_TYPE_STR || !fname_val.sval) {
            os_puts("Type mismatch in filename\n");
            return -1;
        }
        basic_strncpy(fname, fname_val.sval, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            basic_val_t rlen_val;
            p++;
            if (basic_eval_expr(tok, &p, &rlen_val) != 0) return -1;
            reclen = (uint16_t)val_to_int(&rlen_val);
            if (reclen == 0) reclen = 128;
            if (reclen > BASIC_MAX_REC_LEN) reclen = BASIC_MAX_REC_LEN;
        }
    } else {
        /* Syntax 2: OPEN "filename" [FOR mode] AS #n [LEN = reclen] */
        basic_val_t fnum_val;
        basic_strncpy(fname, arg1.sval, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';

        if (tok[p] == TOK_FOR) {
            p++; /* skip TOK_FOR */
            while (tok[p] == ' ' || tok[p] == '\t') p++;

            if (basic_strnicmp_far(&tok[p], "OUTPUT", 6) == 0) {
                mode = 'O';
                p += 6;
            } else if (tok[p] == TOK_INPUT || basic_strnicmp_far(&tok[p], "INPUT", 5) == 0) {
                mode = 'I';
                p = (tok[p] == TOK_INPUT) ? (p + 1) : (p + 5);
            } else if (basic_strnicmp_far(&tok[p], "APPEND", 6) == 0) {
                mode = 'A';
                p += 6;
            } else if (basic_strnicmp_far(&tok[p], "RANDOM", 6) == 0) {
                mode = 'R';
                p += 6;
            } else {
                os_puts("Bad file mode in OPEN\n");
                return -1;
            }
        } else {
            /* FOR omitted -> defaults to RANDOM mode in GW-BASIC */
            mode = 'R';
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (basic_strnicmp_far(&tok[p], "AS", 2) == 0) {
            p += 2;
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == '#') p++;

        if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
        fnum = (uint8_t)fnum_val.ival;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if ((tok[p] == 0xFF && tok[p + 1] == (uint8_t)(TOK_LEN & 0xFF)) ||
            basic_strnicmp_far(&tok[p], "LEN", 3) == 0) {
            basic_val_t rlen_val;
            if (tok[p] == 0xFF) p += 2;
            else p += 3;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '=' || tok[p] == TOK_EQ) p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (basic_eval_expr(tok, &p, &rlen_val) != 0) return -1;
            reclen = (uint16_t)val_to_int(&rlen_val);
            if (reclen == 0) reclen = 128;
            if (reclen > BASIC_MAX_REC_LEN) reclen = BASIC_MAX_REC_LEN;
        }
    }

    if (fnum < 1 || fnum > BASIC_MAX_FILES) {
        os_puts("Bad file number\n");
        return -1;
    }

    {
        basic_file_t *f = &s_files[fnum - 1];
        if (f->is_open) {
            os_puts("File already open\n");
            return -1;
        }

        f->mode = mode;
        basic_strncpy(f->filename, fname, sizeof(f->filename) - 1);
        f->filename[sizeof(f->filename) - 1] = '\0';
        f->buf_pos = 0;
        f->buf_len = 0;
        f->eof = 0;

        if (mode == 'O') {
            os_file_write(f->filename, (const void __far *)FP_HERE(""), 0);
            f->file_pos = 0;
            f->file_len = 0;
        } else if (mode == 'A') {
            sz = os_file_size(f->filename);
            f->file_pos = (sz > 0) ? (uint32_t)sz : 0;
            f->file_len = f->file_pos;
        } else if (mode == 'R') {
            sz = os_file_size(f->filename);
            if (sz < 0) {
                os_file_write(f->filename, (const void __far *)FP_HERE(""), 0);
                sz = 0;
            }
            f->file_pos = 0;
            f->file_len = (uint32_t)sz;
            f->record_len = reclen;
            f->current_record = 0;
            f->num_fields = 0;
            basic_memset(f->rec_buf, ' ', reclen);
        } else {
            sz = os_file_size(f->filename);
            if (sz < 0) {
                os_puts("File not found\n");
                return -1;
            }
            f->file_pos = 0;
            f->file_len = (uint32_t)sz;
        }
        f->is_open = 1;
    }

    *pos = p;
    return 0;
}

static int basic_exec_close(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_CLOSE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == 0x00 || tok[p] == ':') {
        basic_close_all_files();
        *pos = p;
        return 0;
    }

    for (;;) {
        basic_val_t fnum_val;
        uint8_t fnum;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == '#') p++;
        if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
        fnum = (uint8_t)fnum_val.ival;
        if (fnum >= 1 && fnum <= BASIC_MAX_FILES) {
            s_files[fnum - 1].is_open = 0;
            s_files[fnum - 1].num_fields = 0;
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            continue;
        }
        break;
    }

    *pos = p;
    return 0;
}

/* --------------------------------------------------------------------------
 * Phase 11: Random-Access Statements (FIELD, LSET, RSET, PUT, GET)
 * -------------------------------------------------------------------------- */

static int basic_exec_field(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t fnum_val;
    uint8_t fnum;
    uint16_t cur_offset = 0;
    basic_file_t *f;

    p += 2; /* skip TOK_FIELD (0xFE, 0x82) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == '#') p++;

    if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
    fnum = (uint8_t)fnum_val.ival;

    if (fnum < 1 || fnum > BASIC_MAX_FILES || !s_files[fnum - 1].is_open || s_files[fnum - 1].mode != 'R') {
        return basic_error(52, "Bad file number\n");
    }
    f = &s_files[fnum - 1];

    for (;;) {
        basic_val_t w_val;
        uint16_t width;
        char var_name[BASIC_MAX_VARNAME];
        basic_var_t *var;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            return basic_error(2, "Syntax error in FIELD\n");
        }
        p++; /* skip ',' */

        if (basic_eval_expr(tok, &p, &w_val) != 0) return -1;
        width = (uint16_t)val_to_int(&w_val);

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (basic_strnicmp_far(&tok[p], "AS", 2) != 0) {
            return basic_error(2, "Syntax error in FIELD (expected AS)\n");
        }
        p += 2;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
            return basic_error(2, "Syntax error in FIELD variable name\n");
        }

        var = basic_get_or_create_var(var_name);
        if (!var || var->type != BASIC_TYPE_STR) {
            return basic_error(13, "Type mismatch in FIELD\n");
        }

        if (f->num_fields < BASIC_MAX_FIELDS) {
            basic_field_t *fld = &f->fields[f->num_fields++];
            basic_strncpy(fld->var_name, var_name, sizeof(fld->var_name) - 1);
            fld->var_name[sizeof(fld->var_name) - 1] = '\0';
            fld->offset = cur_offset;
            fld->len = width;
        }

        if (cur_offset < f->record_len) {
            uint16_t copylen = width;
            if (cur_offset + copylen > f->record_len) {
                copylen = f->record_len - cur_offset;
            }
            if (copylen > BASIC_MAX_STR_LEN) copylen = BASIC_MAX_STR_LEN;
            basic_memcpy(var->sval, &f->rec_buf[cur_offset], copylen);
            var->sval[copylen] = '\0';
            var->slen = copylen;
        }

        cur_offset += width;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            continue;
        }
        break;
    }

    *pos = p;
    return 0;
}

static int basic_exec_lset_rset(const uint8_t __far *tok, uint16_t *pos, int is_rset) {
    uint16_t p = *pos;
    char var_name[BASIC_MAX_VARNAME];
    basic_val_t val;
    basic_var_t *var;
    basic_file_t *f_found = 0;
    basic_field_t *fld_found = 0;
    uint8_t fi, fli;
    uint16_t target_len;
    uint16_t src_len;
    char formatted[BASIC_MAX_REC_LEN + 1];

    p += 2; /* skip TOK_LSET (0xFE, 0x85) or TOK_RSET (0xFE, 0x86) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
        return basic_error(2, "Syntax error in variable name\n");
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != TOK_EQ && tok[p] != '=') {
        return basic_error(2, "Syntax error (expected '=')\n");
    }
    p++;

    if (basic_eval_expr(tok, &p, &val) != 0) return -1;
    if (val.type != BASIC_TYPE_STR) {
        return basic_error(13, "Type mismatch\n");
    }

    var = basic_get_or_create_var(var_name);
    if (!var || var->type != BASIC_TYPE_STR) {
        return basic_error(13, "Type mismatch\n");
    }

    for (fi = 0; fi < BASIC_MAX_FILES; fi++) {
        if (s_files[fi].is_open && s_files[fi].mode == 'R') {
            for (fli = 0; fli < s_files[fi].num_fields; fli++) {
                if (basic_stricmp(s_files[fi].fields[fli].var_name, var_name) == 0) {
                    f_found = &s_files[fi];
                    fld_found = &s_files[fi].fields[fli];
                    break;
                }
            }
            if (f_found) break;
        }
    }

    if (fld_found) {
        target_len = fld_found->len;
    } else {
        target_len = var->slen ? var->slen : basic_strlen(var->sval);
    }
    if (target_len > BASIC_MAX_REC_LEN) target_len = BASIC_MAX_REC_LEN;

    src_len = (val.ival > 0) ? (uint16_t)val.ival : (val.sval ? basic_strlen(val.sval) : 0);

    if (!is_rset) {
        /* LSET: left-justified, padded with spaces on right */
        uint16_t cpy = (src_len < target_len) ? src_len : target_len;
        if (cpy > 0 && val.sval) {
            basic_memcpy(formatted, val.sval, cpy);
        }
        if (target_len > cpy) {
            basic_memset(&formatted[cpy], ' ', target_len - cpy);
        }
    } else {
        /* RSET: right-justified, padded with spaces on left */
        if (src_len >= target_len) {
            if (val.sval) {
                basic_memcpy(formatted, val.sval, target_len);
            }
        } else {
            uint16_t pad = target_len - src_len;
            basic_memset(formatted, ' ', pad);
            if (src_len > 0 && val.sval) {
                basic_memcpy(&formatted[pad], val.sval, src_len);
            }
        }
    }
    formatted[target_len] = '\0';

    if (f_found && fld_found) {
        if (fld_found->offset < f_found->record_len) {
            uint16_t wr = target_len;
            if (fld_found->offset + wr > f_found->record_len) {
                wr = f_found->record_len - fld_found->offset;
            }
            basic_memcpy(&f_found->rec_buf[fld_found->offset], formatted, wr);
        }
    }

    {
        uint16_t v_copy = target_len;
        if (v_copy > BASIC_MAX_STR_LEN) v_copy = BASIC_MAX_STR_LEN;
        basic_memcpy(var->sval, formatted, v_copy);
        var->sval[v_copy] = '\0';
        var->slen = v_copy;
    }

    *pos = p;
    return 0;
}

static int basic_exec_put(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t fnum_val;
    uint8_t fnum;
    uint32_t recnum;
    basic_file_t *f;
    uint32_t file_offset;

    p += 2; /* skip TOK_PUT (0xFE, 0x88) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == '#') p++;

    if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
    fnum = (uint8_t)fnum_val.ival;

    if (fnum < 1 || fnum > BASIC_MAX_FILES || !s_files[fnum - 1].is_open || s_files[fnum - 1].mode != 'R') {
        return basic_error(52, "Bad file number\n");
    }
    f = &s_files[fnum - 1];

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        basic_val_t r_val;
        p++;
        if (basic_eval_expr(tok, &p, &r_val) != 0) return -1;
        recnum = (uint32_t)val_to_int(&r_val);
        if (recnum < 1) {
            return basic_error(63, "Bad record number\n");
        }
    } else {
        recnum = f->current_record + 1;
        if (recnum == 0) recnum = 1;
    }

    file_offset = mul16((uint16_t)(recnum - 1), f->record_len);
    os_file_write_offset(f->filename, (const void __far *)FP_HERE(f->rec_buf), f->record_len, file_offset);

    f->current_record = recnum;
    {
        long sz = os_file_size(f->filename);
        if (sz >= 0) f->file_len = (uint32_t)sz;
    }

    *pos = p;
    return 0;
}

static int basic_exec_get(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t fnum_val;
    uint8_t fnum;
    uint32_t recnum;
    basic_file_t *f;
    uint32_t file_offset;
    uint16_t rd;
    uint8_t fli;

    p += 2; /* skip TOK_GET (0xFE, 0x89) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == '#') p++;

    if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
    fnum = (uint8_t)fnum_val.ival;

    if (fnum < 1 || fnum > BASIC_MAX_FILES || !s_files[fnum - 1].is_open || s_files[fnum - 1].mode != 'R') {
        return basic_error(52, "Bad file number\n");
    }
    f = &s_files[fnum - 1];

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        basic_val_t r_val;
        p++;
        if (basic_eval_expr(tok, &p, &r_val) != 0) return -1;
        recnum = (uint32_t)val_to_int(&r_val);
        if (recnum < 1) {
            return basic_error(63, "Bad record number\n");
        }
    } else {
        recnum = f->current_record + 1;
        if (recnum == 0) recnum = 1;
    }

    file_offset = mul16((uint16_t)(recnum - 1), f->record_len);
    basic_memset(f->rec_buf, ' ', f->record_len);
    rd = os_file_read_offset(f->filename, (void __far *)FP_HERE(f->rec_buf), f->record_len, file_offset);

    f->current_record = recnum;
    if (rd < f->record_len) {
        f->eof = 1;
    } else {
        f->eof = 0;
    }

    /* Synchronize all FIELD variables bound to this channel */
    for (fli = 0; fli < f->num_fields; fli++) {
        basic_field_t *fld = &f->fields[fli];
        basic_var_t *var = basic_get_or_create_var(fld->var_name);
        if (var && var->type == BASIC_TYPE_STR) {
            uint16_t cpy = fld->len;
            if (fld->offset < f->record_len) {
                if (fld->offset + cpy > f->record_len) {
                    cpy = f->record_len - fld->offset;
                }
                if (cpy > BASIC_MAX_STR_LEN) cpy = BASIC_MAX_STR_LEN;
                basic_memcpy(var->sval, &f->rec_buf[fld->offset], cpy);
                var->sval[cpy] = '\0';
                var->slen = cpy;
            }
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_write_file(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t fnum_val;
    uint8_t fnum;

    p++; /* skip TOK_WRITE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] != '#') {
        os_puts("Syntax error in WRITE (expected '#')\n");
        return -1;
    }
    p++;

    if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
    fnum = (uint8_t)fnum_val.ival;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in WRITE (expected ',')\n");
        return -1;
    }
    p++;

    {
        int first = 1;
        char num_buf[16];

        while (tok[p] != 0x00 && tok[p] != ':') {
            basic_val_t val;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == 0x00 || tok[p] == ':') break;

            if (!first) {
                basic_file_write_data(fnum, ",", 1);
            }
            first = 0;

            if (basic_eval_expr(tok, &p, &val) != 0) return -1;

            if (val.type == BASIC_TYPE_STR) {
                basic_file_write_data(fnum, "\"", 1);
                if (val.sval) {
                    basic_file_write_data(fnum, val.sval, basic_strlen(val.sval));
                }
                basic_file_write_data(fnum, "\"", 1);
            } else if (val.type == BASIC_TYPE_FLOAT) {
                char fbuf[24];
                uint16_t flen = basic_format_float(val.fval, fbuf, sizeof(fbuf));
                if (fbuf[0] == ' ') {
                    basic_file_write_data(fnum, fbuf + 1, flen - 1);
                } else {
                    basic_file_write_data(fnum, fbuf, flen);
                }
            } else if (val.type == BASIC_TYPE_UINT) {
                uint16_t uv = (uint16_t)val.ival;
                int np = 0;
                if (uv == 0) {
                    num_buf[np++] = '0';
                } else {
                    char tmp[8];
                    int tp = 0;
                    while (uv > 0) {
                        tmp[tp++] = (char)('0' + (uv % 10));
                        uv /= 10;
                    }
                    while (tp > 0) num_buf[np++] = tmp[--tp];
                }
                basic_file_write_data(fnum, num_buf, (uint16_t)np);
            } else {
                int16_t v = val.ival;
                int np = 0;
                int sign = 0;
                if (v < 0) { sign = 1; v = -v; }
                if (v == 0) {
                    num_buf[np++] = '0';
                } else {
                    char tmp[8];
                    int tp = 0;
                    while (v > 0) {
                        tmp[tp++] = (char)('0' + (v % 10));
                        v /= 10;
                    }
                    if (sign) num_buf[np++] = '-';
                    while (tp > 0) num_buf[np++] = tmp[--tp];
                }
                basic_file_write_data(fnum, num_buf, (uint16_t)np);
            }

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') {
                p++;
                continue;
            }
            break;
        }
        basic_file_write_data(fnum, "\r\n", 2);
    }

    *pos = p;
    return 0;
}

static uint16_t basic_read_line(char *buf, uint16_t max_len);

static int basic_exec_line_input(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_INPUT */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == '#') {
        /* LINE INPUT #n, var$ */
        basic_val_t fnum_val;
        uint8_t fnum;
        char var_name[BASIC_MAX_VARNAME];
        basic_var_t *var;
        char line_buf[BASIC_MAX_STR_LEN + 1];
        uint16_t lp = 0;

        p++;
        if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
        fnum = (uint8_t)fnum_val.ival;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            os_puts("Syntax error in LINE INPUT (expected ',')\n");
            return -1;
        }
        p++;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
            os_puts("Syntax error in variable name\n");
            return -1;
        }

        var = basic_get_or_create_var(var_name);
        if (!var || var->type != BASIC_TYPE_STR) {
            os_puts("Type mismatch in LINE INPUT\n");
            return -1;
        }

        for (;;) {
            int ch = basic_file_getc(fnum);
            if (ch < 0) break;
            if (ch == '\r') continue;
            if (ch == '\n') break;
            if (lp < BASIC_MAX_STR_LEN) {
                line_buf[lp++] = (char)ch;
            }
        }
        line_buf[lp] = '\0';
        basic_strcpy(var->sval, line_buf);
    } else {
        /* Console LINE INPUT [prompt;] var$ */
        char prompt_buf[80];
        char in_buf[BASIC_MAX_LINE_LEN];
        char var_name[BASIC_MAX_VARNAME];
        basic_var_t *var;
        int has_prompt = 0;

        if (tok[p] == '"') {
            uint16_t pp = 0;
            p++;
            while (tok[p] != 0 && tok[p] != '"') {
                if (pp + 1 < sizeof(prompt_buf)) prompt_buf[pp++] = (char)tok[p];
                p++;
            }
            if (tok[p] == '"') p++;
            prompt_buf[pp] = '\0';
            has_prompt = 1;

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ';' || tok[p] == ',') p++;
        }

        if (has_prompt) os_puts(prompt_buf);

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
            os_puts("Syntax error in LINE INPUT variable\n");
            return -1;
        }

        var = basic_get_or_create_var(var_name);
        if (!var || var->type != BASIC_TYPE_STR) {
            os_puts("Type mismatch in LINE INPUT\n");
            return -1;
        }

        basic_read_line(in_buf, sizeof(in_buf));
        basic_strncpy(var->sval, in_buf, sizeof(var->sval) - 1);
        var->sval[sizeof(var->sval) - 1] = '\0';
    }

    *pos = p;
    return 0;
}

/* --------------------------------------------------------------------------
 * Statement Execution (Phase 1, Phase 2, Phase 3 & Phase 4)
 * -------------------------------------------------------------------------- */

static int basic_skip_to_else(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    int nesting = 0;

    while (tok[p] != 0x00) {
        if (tok[p] == '"') {
            p++;
            while (tok[p] != 0x00 && tok[p] != '"') p++;
            if (tok[p] == '"') p++;
            continue;
        }
        if (tok[p] == TOK_IF) {
            nesting++;
            p++;
            continue;
        }
        if (tok[p] == TOK_ELSE) {
            if (nesting > 0) {
                nesting--;
                p++;
                continue;
            }
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (basic_is_digit((char)tok[p])) {
                uint16_t target_line = basic_parse_uint_far(tok, &p);
                uint16_t target_off = basic_find_line(target_line);
                if (target_off == 0) {
                    return basic_error(8, "Undefined line number\n");
                }
                s_pc_line = target_off;
                s_pc_stmt = 4;
                s_jumped = 1;
                *pos = p;
                return 0;
            } else {
                *pos = p;
                return 0;
            }
        }
        p++;
    }

    *pos = p;
    return 0;
}

static int basic_exec_let(const uint8_t __far *tok, uint16_t *pos) {
    char var_name[BASIC_MAX_VARNAME];
    basic_val_t val;
    uint16_t p = *pos;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == 0xFF && tok[p + 1] == (uint8_t)(TOK_MID & 0xFF)) {
        *pos = p;
        return basic_exec_mid_stmt(tok, pos);
    }
    if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
        os_puts("Syntax error in variable name\n");
        return -1;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == '(') {
        basic_val_t i1_val, i2_val;
        basic_array_t *arr;
        uint8_t num_dims = 1;
        p++;
        if (basic_eval_expr(tok, &p, &i1_val) != 0) return -1;
        i2_val.ival = 0;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            num_dims = 2;
            if (basic_eval_expr(tok, &p, &i2_val) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
        }
        if (tok[p] != ')') {
            os_puts("Syntax error in array subscript (expected ')')\n");
            return -1;
        }
        p++;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != TOK_EQ && tok[p] != '=') {
            os_puts("Syntax error (expected '=')\n");
            return -1;
        }
        p++;

        if (basic_eval_expr(tok, &p, &val) != 0) return -1;

        arr = basic_find_array(var_name);
        if (!arr) {
            arr = basic_create_default_array(var_name, num_dims);
            if (!arr) return -1;
        }
        if (basic_write_array_elem(arr, i1_val.ival, i2_val.ival, &val) != 0) {
            return -1;
        }
        *pos = p;
        return 0;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != TOK_EQ && tok[p] != '=') {
        os_puts("Syntax error (expected '=')\n");
        return -1;
    }
    p++;

    if (basic_eval_expr(tok, &p, &val) != 0) {
        return -1;
    }

    {
        basic_var_t *var = basic_get_or_create_var(var_name);
        if (!var) return -1;

        if (var->type == BASIC_TYPE_STR) {
            uint16_t slen;
            if (val.type != BASIC_TYPE_STR) {
                return basic_error(13, "Type mismatch\n");
            }
            slen = (val.ival > 0) ? (uint16_t)val.ival : (val.sval ? basic_strlen(val.sval) : 0);
            if (slen > BASIC_MAX_STR_LEN) slen = BASIC_MAX_STR_LEN;
            if (val.sval && slen > 0) {
                basic_memcpy(var->sval, val.sval, slen);
            }
            var->sval[slen] = '\0';
            var->slen = slen;
        } else if (var->type == BASIC_TYPE_INT) {
            if (val.type == BASIC_TYPE_STR) {
                return basic_error(13, "Type mismatch\n");
            }
            var->ival = (val.type == BASIC_TYPE_FLOAT) ? soft_to_int(val.fval) : val.ival;
            var->fval = 0;
        } else {
            if (val.type == BASIC_TYPE_STR) {
                return basic_error(13, "Type mismatch\n");
            }
            var->type = val.type;
            var->ival = val.ival;
            var->fval = val.fval;
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_dim(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_DIM */

    for (;;) {
        char var_name[BASIC_MAX_VARNAME];
        basic_val_t d1_val, d2_val;
        uint16_t dim1, dim2;
        uint8_t num_dims;
        uint8_t type;
        uint16_t total_elems, elem_size, bytes;
        basic_array_t *arr;
        uint16_t len, i;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
            os_puts("Syntax error in DIM\n");
            return -1;
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') {
            os_puts("Syntax error in DIM (expected '(')\n");
            return -1;
        }
        p++;

        if (basic_eval_expr(tok, &p, &d1_val) != 0) return -1;
        dim1 = (uint16_t)(val_to_int(&d1_val) + 1);
        num_dims = 1;
        dim2 = 1;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            if (basic_eval_expr(tok, &p, &d2_val) != 0) return -1;
            dim2 = (uint16_t)(val_to_int(&d2_val) + 1);
            num_dims = 2;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
        }

        if (tok[p] != ')') {
            os_puts("Syntax error in DIM (expected ')')\n");
            return -1;
        }
        p++;

        for (i = 0; i < s_num_arrays; i++) {
            if (basic_stricmp(s_arrays[i].name, var_name) == 0) {
                os_puts("Duplicate definition\n");
                return -1;
            }
        }

        if (s_num_arrays >= BASIC_MAX_ARRAYS) {
            os_puts("Out of array space\n");
            return -1;
        }

        len = basic_strlen(var_name);
        if (len > 0 && var_name[len - 1] == '$') {
            type = BASIC_TYPE_STR;
            elem_size = BASIC_STR_ELEM_LEN;
        } else if (len > 0 && var_name[len - 1] == '%') {
            type = BASIC_TYPE_INT;
            elem_size = 2;
        } else {
            type = BASIC_TYPE_FLOAT;
            elem_size = 4;
        }

        total_elems = (num_dims == 1) ? dim1 : (dim1 * dim2);
        bytes = total_elems * elem_size;

        if ((uint32_t)s_array_heap_free + bytes >= 65000L) {
            os_puts("Out of memory in DIM\n");
            return -1;
        }

        arr = &s_arrays[s_num_arrays++];
        basic_strncpy(arr->name, var_name, BASIC_MAX_VARNAME - 1);
        arr->name[BASIC_MAX_VARNAME - 1] = '\0';
        arr->type = type;
        arr->num_dims = num_dims;
        arr->dim1 = dim1;
        arr->dim2 = dim2;
        arr->data_off = s_array_heap_free;
        s_array_heap_free += bytes;

        {
            uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
            uint16_t b;
            for (b = 0; b < bytes; b++) {
                mem[arr->data_off + b] = 0;
            }
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            continue;
        }
        break;
    }

    *pos = p;
    return 0;
}

static int basic_skip_to_next(const uint8_t __far *tok, uint16_t *pos, const char *var_name) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t cur_line = s_cur_line_off;
    uint16_t p = *pos;
    int nesting = 0;

    (void)var_name;

    for (;;) {
        while (mem[cur_line + p] != 0x00) {
            uint8_t b = mem[cur_line + p];
            if (b == '"') {
                p++;
                while (mem[cur_line + p] != 0x00 && mem[cur_line + p] != '"') p++;
                if (mem[cur_line + p] == '"') p++;
                continue;
            }
            if (b == TOK_FOR) {
                nesting++;
                p++;
                continue;
            }
            if (b == TOK_NEXT) {
                if (nesting > 0) {
                    nesting--;
                    p++;
                    continue;
                }
                p++;
                while (mem[cur_line + p] == ' ' || mem[cur_line + p] == '\t') p++;
                if (basic_is_alpha((char)mem[cur_line + p])) {
                    char dummy[BASIC_MAX_VARNAME];
                    basic_parse_var_name(&mem[cur_line], &p, dummy, sizeof(dummy));
                }
                s_pc_line = cur_line;
                s_pc_stmt = p;
                s_jumped = 1;
                *pos = p;
                return 0;
            }
            p++;
        }

        cur_line = (uint16_t)mem[cur_line] | ((uint16_t)mem[cur_line + 1] << 8);
        if (cur_line == 0) {
            os_puts("FOR without NEXT\n");
            return -1;
        }
        p = 4;
    }
}

static int basic_exec_for(const uint8_t __far *tok, uint16_t *pos) {
    char var_name[BASIC_MAX_VARNAME];
    basic_val_t start_val, end_val, step_val;
    basic_var_t *var;
    int16_t step = 1;
    uint16_t p = *pos;
    int slot = -1;
    uint16_t i;

    p++; /* skip TOK_FOR */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
        os_puts("Syntax error in FOR variable\n");
        return -1;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != TOK_EQ && tok[p] != '=') {
        os_puts("Syntax error in FOR (expected '=')\n");
        return -1;
    }
    p++;

    if (basic_eval_expr(tok, &p, &start_val) != 0) return -1;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != TOK_TO) {
        os_puts("Syntax error in FOR (expected TO)\n");
        return -1;
    }
    p++;

    if (basic_eval_expr(tok, &p, &end_val) != 0) return -1;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == TOK_STEP) {
        p++;
        if (basic_eval_expr(tok, &p, &step_val) != 0) return -1;
        step = (step_val.type == BASIC_TYPE_FLOAT) ? soft_to_int(step_val.fval) : step_val.ival;
    }

    var = basic_get_or_create_var(var_name);
    if (!var) return -1;
    var->ival = (start_val.type == BASIC_TYPE_FLOAT) ? soft_to_int(start_val.fval) : start_val.ival;
    if (var->type == BASIC_TYPE_FLOAT) {
        var->fval = (start_val.type == BASIC_TYPE_FLOAT) ? start_val.fval : soft_from_int(start_val.ival);
    }

    for (i = 0; i < s_for_sp; i++) {
        if (basic_stricmp(s_for_stack[i].var_name, var_name) == 0) {
            slot = (int)i;
            break;
        }
    }
    if (slot == -1) {
        if (s_for_sp >= BASIC_MAX_FOR_NEST) {
            os_puts("FOR stack overflow\n");
            return -1;
        }
        slot = (int)s_for_sp++;
    }

    basic_strcpy(s_for_stack[slot].var_name, var_name);
    s_for_stack[slot].end_val  = (end_val.type == BASIC_TYPE_FLOAT) ? soft_to_int(end_val.fval) : end_val.ival;
    s_for_stack[slot].step_val = step;
    s_for_stack[slot].line_off = s_cur_line_off;
    s_for_stack[slot].stmt_pos = p;

    if ((step > 0 && var->ival > s_for_stack[slot].end_val) || (step < 0 && var->ival < s_for_stack[slot].end_val)) {
        return basic_skip_to_next(tok, &p, var_name);
    }

    *pos = p;
    return 0;
}

static int basic_exec_next(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_NEXT */

    for (;;) {
        char var_name[BASIC_MAX_VARNAME];
        int has_var = 0;
        int slot = -1;
        basic_var_t *var;
        int done = 0;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (basic_is_alpha((char)tok[p])) {
            basic_parse_var_name(tok, &p, var_name, sizeof(var_name));
            has_var = 1;
        }

        if (s_for_sp == 0) {
            return basic_error(1, "NEXT without FOR\n");
        }

        if (has_var) {
            int i;
            for (i = (int)s_for_sp - 1; i >= 0; i--) {
                if (basic_stricmp(s_for_stack[i].var_name, var_name) == 0) {
                    slot = i;
                    break;
                }
            }
            if (slot == -1) {
                return basic_error(1, "NEXT without FOR\n");
            }
        } else {
            slot = (int)s_for_sp - 1;
        }

        var = basic_find_var(s_for_stack[slot].var_name);
        if (!var) return -1;

        var->ival += s_for_stack[slot].step_val;
        if (var->type == BASIC_TYPE_FLOAT) {
            var->fval = soft_from_int(var->ival);
        }

        if (s_for_stack[slot].step_val > 0) {
            if (var->ival > s_for_stack[slot].end_val) done = 1;
        } else {
            if (var->ival < s_for_stack[slot].end_val) done = 1;
        }

        if (!done) {
            s_pc_line = s_for_stack[slot].line_off;
            s_pc_stmt = s_for_stack[slot].stmt_pos;
            s_jumped = 1;
            *pos = p;
            return 0;
        }

        s_for_sp = (uint16_t)slot;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            continue;
        }
        break;
    }

    *pos = p;
    return 0;
}

static int basic_skip_to_wend(const uint8_t __far *tok, uint16_t *pos) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t cur_line = s_cur_line_off;
    uint16_t p = *pos;
    int nesting = 0;

    for (;;) {
        while (mem[cur_line + p] != 0x00) {
            uint8_t b = mem[cur_line + p];
            if (b == '"') {
                p++;
                while (mem[cur_line + p] != 0x00 && mem[cur_line + p] != '"') p++;
                if (mem[cur_line + p] == '"') p++;
                continue;
            }
            if (b == TOK_WHILE) {
                nesting++;
                p++;
                continue;
            }
            if (b == TOK_WEND) {
                if (nesting > 0) {
                    nesting--;
                    p++;
                    continue;
                }
                p++;
                s_pc_line = cur_line;
                s_pc_stmt = p;
                s_jumped = 1;
                *pos = p;
                return 0;
            }
            p++;
        }

        cur_line = (uint16_t)mem[cur_line] | ((uint16_t)mem[cur_line + 1] << 8);
        if (cur_line == 0) {
            os_puts("WHILE without WEND\n");
            return -1;
        }
        p = 4;
    }
}

static int basic_exec_while(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t while_stmt_pos = *pos;
    uint16_t p = *pos;
    uint16_t expr_line = s_cur_line_off;
    basic_val_t cond;

    p++; /* skip TOK_WHILE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &cond) != 0) return -1;

    if (cond.ival != 0) {
        if (s_while_sp == 0 || s_while_stack[s_while_sp - 1].while_line_off != expr_line ||
            s_while_stack[s_while_sp - 1].while_stmt_pos != while_stmt_pos) {
            if (s_while_sp >= BASIC_MAX_WHILE_NEST) {
                os_puts("WHILE stack overflow\n");
                return -1;
            }
            s_while_stack[s_while_sp].while_line_off = expr_line;
            s_while_stack[s_while_sp].while_stmt_pos = while_stmt_pos;
            s_while_sp++;
        }
        *pos = p;
        return 0;
    } else {
        if (s_while_sp > 0 && s_while_stack[s_while_sp - 1].while_line_off == expr_line &&
            s_while_stack[s_while_sp - 1].while_stmt_pos == while_stmt_pos) {
            s_while_sp--;
        }
        return basic_skip_to_wend(tok, &p);
    }
}

static int basic_exec_wend(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_WEND */

    if (s_while_sp == 0) {
        return basic_error(30, "WEND without WHILE\n");
    }

    s_pc_line = s_while_stack[s_while_sp - 1].while_line_off;
    s_pc_stmt = s_while_stack[s_while_sp - 1].while_stmt_pos;
    s_jumped = 1;
    *pos = p;
    return 0;
}

static int basic_read_next_data_item(char *out_buf, uint16_t max_len) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);

    for (;;) {
        if (s_data_line_off == 0) {
            return basic_error(4, "Out of DATA\n");
        }

        if (s_data_stmt_pos == 0) {
            uint16_t p = 4;
            int found = 0;
            while (mem[s_data_line_off + p] != 0x00) {
                if (mem[s_data_line_off + p] == TOK_DATA) {
                    s_data_stmt_pos = p + 1;
                    found = 1;
                    break;
                }
                p++;
            }
            if (!found) {
                s_data_line_off = (uint16_t)mem[s_data_line_off] | ((uint16_t)mem[s_data_line_off + 1] << 8);
                s_data_stmt_pos = 0;
                continue;
            }
        }

        {
            uint16_t p = s_data_stmt_pos;
            uint16_t bp = 0;

            while (mem[s_data_line_off + p] == ' ' || mem[s_data_line_off + p] == '\t') p++;

            if (mem[s_data_line_off + p] == 0x00 || mem[s_data_line_off + p] == ':') {
                s_data_line_off = (uint16_t)mem[s_data_line_off] | ((uint16_t)mem[s_data_line_off + 1] << 8);
                s_data_stmt_pos = 0;
                continue;
            }

            if (mem[s_data_line_off + p] == '"') {
                p++;
                while (mem[s_data_line_off + p] != 0x00 && mem[s_data_line_off + p] != '"') {
                    if (bp + 1 < max_len) out_buf[bp++] = (char)mem[s_data_line_off + p];
                    p++;
                }
                if (mem[s_data_line_off + p] == '"') p++;
            } else {
                while (mem[s_data_line_off + p] != 0x00 &&
                       mem[s_data_line_off + p] != ':' &&
                       mem[s_data_line_off + p] != ',') {
                    if (bp + 1 < max_len) out_buf[bp++] = (char)mem[s_data_line_off + p];
                    p++;
                }
                while (bp > 0 && (out_buf[bp - 1] == ' ' || out_buf[bp - 1] == '\t')) bp--;
            }
            out_buf[bp] = '\0';

            while (mem[s_data_line_off + p] == ' ' || mem[s_data_line_off + p] == '\t') p++;
            if (mem[s_data_line_off + p] == ',') {
                p++;
            }
            s_data_stmt_pos = p;
            return 0;
        }
    }
}

static int basic_exec_read(const uint8_t __far *tok, uint16_t *pos) {
    char data_str[BASIC_MAX_STR_LEN + 1];
    uint16_t p = *pos;
    p++; /* skip TOK_READ */

    for (;;) {
        char var_name[BASIC_MAX_VARNAME];
        basic_var_t *var;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
            os_puts("Syntax error in READ\n");
            return -1;
        }

        if (basic_read_next_data_item(data_str, sizeof(data_str)) != 0) {
            return -1;
        }

        var = basic_get_or_create_var(var_name);
        if (!var) return -1;

        if (var->type == BASIC_TYPE_STR) {
            basic_strncpy(var->sval, data_str, sizeof(var->sval) - 1);
            var->sval[sizeof(var->sval) - 1] = '\0';
        } else if (var->type == BASIC_TYPE_FLOAT) {
            var->fval = basic_atof(data_str);
        } else {
            var->ival = basic_atoi(data_str);
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            continue;
        }
        break;
    }

    *pos = p;
    return 0;
}

static int basic_exec_restore(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_RESTORE */

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_is_digit((char)tok[p])) {
        uint16_t target_line = basic_parse_uint_far(tok, &p);
        uint16_t off = basic_find_line(target_line);
        if (off == 0) {
            os_puts("Undefined line number ");
            basic_print_uint(target_line);
            os_putc('\n');
            return -1;
        }
        s_data_line_off = off;
    } else {
        s_data_line_off = s_prog_head;
    }
    s_data_stmt_pos = 0;
    *pos = p;
    return 0;
}

static int basic_exec_locate(const uint8_t __far *tok, uint16_t *pos) {
    basic_val_t r_val, c_val;
    uint8_t row, col;
    uint16_t p = *pos;
    p++; /* skip TOK_LOCATE */

    if (basic_eval_expr(tok, &p, &r_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        if (basic_eval_expr(tok, &p, &c_val) != 0) return -1;
    } else {
        c_val.ival = 1;
    }

    row = (r_val.ival > 0) ? (uint8_t)(r_val.ival - 1) : 0;
    col = (c_val.ival > 0) ? (uint8_t)(c_val.ival - 1) : 0;
    if (s_screen_mode == 0) {
        uint8_t max_r = s_key_visible ? 23 : 24;
        if (row > max_r) row = max_r;
        if (col > 79) col = 79;
        os_set_cursor(row, col);
    } else {
        uint8_t max_r = (s_screen_mode == 12) ? 59 : 24;
        uint8_t max_c = (s_screen_mode == 2 || s_screen_mode == 12) ? 79 : 39;
        if (row > max_r) row = max_r;
        if (col > max_c) col = max_c;
    }
    s_gfx_row = row;
    s_gfx_col = col;

    *pos = p;
    return 0;
}

static int basic_exec_swap(const uint8_t __far *tok, uint16_t *pos) {
    char v1_name[BASIC_MAX_VARNAME];
    char v2_name[BASIC_MAX_VARNAME];
    basic_var_t *v1, *v2;
    uint16_t p = *pos;
    p++; /* skip TOK_SWAP */

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (!basic_parse_var_name(tok, &p, v1_name, sizeof(v1_name))) {
        os_puts("Syntax error in SWAP\n");
        return -1;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in SWAP (expected ',')\n");
        return -1;
    }
    p++;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (!basic_parse_var_name(tok, &p, v2_name, sizeof(v2_name))) {
        os_puts("Syntax error in SWAP\n");
        return -1;
    }

    v1 = basic_get_or_create_var(v1_name);
    v2 = basic_get_or_create_var(v2_name);
    if (!v1 || !v2) return -1;

    if (v1->type != v2->type) {
        os_puts("Type mismatch in SWAP\n");
        return -1;
    }

    if (v1->type == BASIC_TYPE_STR) {
        char tmp_str[BASIC_MAX_STR_LEN + 1];
        basic_strcpy(tmp_str, v1->sval);
        basic_strcpy(v1->sval, v2->sval);
        basic_strcpy(v2->sval, tmp_str);
    } else {
        int16_t tmp_i = v1->ival;
        basic_float_t tmp_f = v1->fval;
        uint8_t tmp_t = v1->type;
        v1->ival = v2->ival;
        v1->fval = v2->fval;
        v1->type = v2->type;
        v2->ival = tmp_i;
        v2->fval = tmp_f;
        v2->type = tmp_t;
    }

    *pos = p;
    return 0;
}

static int basic_exec_goto(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    uint16_t target_line;
    uint16_t target_off;

    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (!basic_is_digit((char)tok[p])) {
        os_puts("Syntax error (line number expected)\n");
        return -1;
    }
    target_line = basic_parse_uint_far(tok, &p);

    target_off = basic_find_line(target_line);
    if (target_off == 0) {
        return basic_error(8, "Undefined line number\n");
    }

    s_pc_line = target_off;
    s_pc_stmt = 4;
    s_jumped = 1;
    *pos = p;
    return 0;
}

static int basic_exec_gosub(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    uint16_t target_line;
    uint16_t target_off;

    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (!basic_is_digit((char)tok[p])) {
        os_puts("Syntax error (line number expected)\n");
        return -1;
    }
    target_line = basic_parse_uint_far(tok, &p);

    target_off = basic_find_line(target_line);
    if (target_off == 0) {
        return basic_error(8, "Undefined line number\n");
    }

    if (s_gosub_sp >= BASIC_MAX_GOSUB) {
        return basic_error(7, "Out of memory\n");
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ':') p++;

    s_gosub_stack[s_gosub_sp].line_off = s_cur_line_off;
    s_gosub_stack[s_gosub_sp].stmt_pos = p;
    s_gosub_sp++;

    s_pc_line = target_off;
    s_pc_stmt = 4;
    s_jumped = 1;
    *pos = p;
    return 0;
}

static int basic_exec_return(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++;

    if (s_gosub_sp == 0) {
        return basic_error(3, "RETURN without GOSUB\n");
    }

    s_gosub_sp--;
    s_pc_line = s_gosub_stack[s_gosub_sp].line_off;
    s_pc_stmt = s_gosub_stack[s_gosub_sp].stmt_pos;
    s_jumped = 1;

    /* Clear event in_handler flags */
    if (s_timer_in_handler) {
        s_timer_in_handler = 0;
    }
    {
        int k;
        for (k = 1; k <= 14; k++) {
            if (s_key_trap[k].in_handler) {
                s_key_trap[k].in_handler = 0;
            }
        }
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != TOK_ELSE) {
        /* Optional RETURN <linenum> */
        basic_val_t ret_line_val;
        if (basic_eval_expr(tok, &p, &ret_line_val) == 0) {
            uint16_t ret_line = (uint16_t)val_to_int(&ret_line_val);
            if (ret_line != 0) {
                uint16_t ret_off = basic_find_line(ret_line);
                if (ret_off) {
                    s_pc_line = ret_off;
                    s_pc_stmt = 4;
                }
            }
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_if(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t cond;

    p++;
    if (basic_eval_expr(tok, &p, &cond) != 0) {
        return -1;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_GOTO) {
        if (cond.ival != 0) {
            int ret;
            *pos = p;
            ret = basic_exec_goto(tok, pos);
            return ret;
        } else {
            int ret = basic_skip_to_else(tok, &p);
            *pos = p;
            return ret;
        }
    } else if (tok[p] == TOK_THEN) {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;

        if (cond.ival != 0) {
            if (basic_is_digit((char)tok[p])) {
                uint16_t target_line = basic_parse_uint_far(tok, &p);
                uint16_t target_off = basic_find_line(target_line);
                if (target_off == 0) {
                    return basic_error(8, "Undefined line number\n");
                }
                s_pc_line = target_off;
                s_pc_stmt = 4;
                s_jumped = 1;
                *pos = p;
                return 0;
            } else {
                *pos = p;
                return 0;
            }
        } else {
            int ret = basic_skip_to_else(tok, &p);
            *pos = p;
            return ret;
        }
    } else {
        os_puts("Syntax error in IF (expected THEN or GOTO)\n");
        return -1;
    }
}

static int basic_exec_input(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    /* File INPUT: INPUT #filenum, var1 [, var2 ...] */
    if (tok[p] == '#') {
        basic_val_t fnum_val;
        uint8_t fnum;
        p++;
        if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
        fnum = (uint8_t)fnum_val.ival;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            os_puts("Syntax error in INPUT (expected ',')\n");
            return -1;
        }
        p++;

        for (;;) {
            char var_name[BASIC_MAX_VARNAME];
            basic_var_t *var;
            char val_str[BASIC_MAX_STR_LEN + 1];
            uint16_t vp = 0;
            int ch;

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
                os_puts("Syntax error in INPUT variable\n");
                return -1;
            }

            var = basic_get_or_create_var(var_name);
            if (!var) return -1;

            /* Read next item from file */
            do {
                ch = basic_file_getc(fnum);
            } while (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n');

            if (ch < 0) {
                os_puts("Input past end of file\n");
                return -1;
            }

            if (ch == '"') {
                for (;;) {
                    ch = basic_file_getc(fnum);
                    if (ch < 0 || ch == '"' || ch == '\n') break;
                    if (vp < BASIC_MAX_STR_LEN && ch != '\r') val_str[vp++] = (char)ch;
                }
                /* Consume trailing spaces and the delimiter comma/newline */
                do {
                    ch = basic_file_getc(fnum);
                } while (ch == ' ' || ch == '\t');
                if (ch == '\r') {
                    int next_ch = basic_file_getc(fnum);
                    if (next_ch != '\n' && next_ch >= 0) basic_file_ungetc(fnum);
                } else if (ch != ',') {
                    if (ch >= 0) basic_file_ungetc(fnum);
                }
            } else {
                while (ch >= 0 && ch != ',' && ch != '\r' && ch != '\n') {
                    if (vp < BASIC_MAX_STR_LEN) val_str[vp++] = (char)ch;
                    ch = basic_file_getc(fnum);
                }
                while (vp > 0 && (val_str[vp - 1] == ' ' || val_str[vp - 1] == '\t')) vp--;
                if (ch == '\r') {
                    int next_ch = basic_file_getc(fnum);
                    if (next_ch != '\n' && next_ch >= 0) basic_file_ungetc(fnum);
                }
            }
            val_str[vp] = '\0';

            if (var->type == BASIC_TYPE_STR) {
                basic_strcpy(var->sval, val_str);
            } else if (var->type == BASIC_TYPE_FLOAT) {
                var->fval = basic_atof(val_str);
            } else {
                var->ival = basic_atoi(val_str);
            }

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') {
                p++;
                continue;
            }
            break;
        }

        *pos = p;
        return 0;
    }

    /* Console INPUT [prompt;] var1 [, var2 ...] */
    {
        char prompt_buf[80];
        char in_buf[BASIC_MAX_LINE_LEN];
        int has_prompt = 0;
        int print_qmark = 1;

        if (tok[p] == '"') {
            uint16_t pp = 0;
            p++;
            while (tok[p] != 0 && tok[p] != '"') {
                if (pp + 1 < sizeof(prompt_buf)) {
                    prompt_buf[pp++] = (char)tok[p];
                }
                p++;
            }
            if (tok[p] == '"') p++;
            prompt_buf[pp] = '\0';
            has_prompt = 1;

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ';') {
                print_qmark = 1;
                p++;
            } else if (tok[p] == ',') {
                print_qmark = 0;
                p++;
            }
        }

        if (has_prompt) {
            os_puts(prompt_buf);
        }
        if (print_qmark) {
            os_puts("? ");
        }

        if (basic_read_line(in_buf, sizeof(in_buf)) == 0 && in_buf[0] == '\0') {
            s_running = 0;
            return -1;
        }

        {
            uint16_t ip = 0;
            for (;;) {
                char var_name[BASIC_MAX_VARNAME];
                basic_var_t *var;

                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
                    os_puts("Syntax error in INPUT\n");
                    return -1;
                }

                var = basic_get_or_create_var(var_name);
                if (!var) return -1;

                while (in_buf[ip] == ' ' || in_buf[ip] == '\t') ip++;
                if (var->type == BASIC_TYPE_STR) {
                    uint16_t sp = 0;
                    if (in_buf[ip] == '"') {
                        ip++;
                        while (in_buf[ip] != '\0' && in_buf[ip] != '"') {
                            if (sp < BASIC_MAX_STR_LEN) var->sval[sp++] = in_buf[ip];
                            ip++;
                        }
                        if (in_buf[ip] == '"') ip++;
                    } else {
                        while (in_buf[ip] != '\0' && in_buf[ip] != ',') {
                            if (sp < BASIC_MAX_STR_LEN) var->sval[sp++] = in_buf[ip];
                            ip++;
                        }
                    }
                    var->sval[sp] = '\0';
                } else if (var->type == BASIC_TYPE_FLOAT) {
                    var->fval = basic_atof(&in_buf[ip]);
                    while (in_buf[ip] != '\0' && in_buf[ip] != ',') ip++;
                } else {
                    var->ival = basic_atoi(&in_buf[ip]);
                    while (in_buf[ip] != '\0' && in_buf[ip] != ',') ip++;
                }

                if (in_buf[ip] == ',') ip++;

                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] == ',') {
                    p++;
                    continue;
                }
                break;
            }
        }
    }

    *pos = p;
    return 0;
}

/* --------------------------------------------------------------------------
 * Hardware Graphics & Sound Statements (Phase 5)
 * -------------------------------------------------------------------------- */

static void basic_delay_ms(uint16_t ms) {
    volatile uint16_t __far *bda_ticks = (volatile uint16_t __far *)MK_FP(0x0040, 0x006C);
    uint16_t ticks = (ms + 27) / 55;
    uint16_t start = *bda_ticks;
    if (ticks == 0) ticks = 1;
    while ((uint16_t)(*bda_ticks - start) < ticks);
}


static int basic_exec_screen(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_SCREEN */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] != 0x00 && tok[p] != ':') {
        basic_val_t m_val;
        int16_t m;
        if (basic_eval_expr(tok, &p, &m_val) != 0) return -1;
        m = m_val.ival;
        if (m == 0) {
            os_gfx_set_mode(0x03);
            s_screen_mode = 0;
            s_last_x = 0;
            s_last_y = 0;
            s_gfx_row = 0;
            s_gfx_col = 0;
            if (s_key_visible) {
                os_console_set_scroll_rows(24);
                basic_display_softkeys();
            } else {
                os_console_set_scroll_rows(25);
            }
        } else if (m == 1) {
            os_gfx_set_mode(0x04);
            s_screen_mode = 1;
            s_gfx_fg = 3;
            s_draw_color = 3;
            s_last_x = 160;
            s_last_y = 100;
            s_gfx_row = 0;
            s_gfx_col = 0;
        } else if (m == 2) {
            os_gfx_set_mode(0x06);
            s_screen_mode = 2;
            s_gfx_fg = 1;
            s_draw_color = 1;
            s_last_x = 320;
            s_last_y = 100;
            s_gfx_row = 0;
            s_gfx_col = 0;
        } else if (m == 12) {
            if (os_video_type() < VIDEO_TYPE_VGA) {
                return basic_error(5, "Illegal function call (VGA required)\n");
            }
            if (os_gfx_set_mode(0x12) != 0) {
                return basic_error(5, "Illegal function call\n");
            }
            s_screen_mode = 12;
            s_gfx_fg = 15;
            s_draw_color = 15;
            s_last_x = 320;
            s_last_y = 240;
            s_gfx_row = 0;
            s_gfx_col = 0;
        } else if (m == 13) {
            if (os_video_type() < VIDEO_TYPE_VGA) {
                return basic_error(5, "Illegal function call (VGA required)\n");
            }
            if (os_gfx_set_mode(0x13) != 0) {
                return basic_error(5, "Illegal function call\n");
            }
            s_screen_mode = 13;
            s_gfx_fg = 15;
            s_draw_color = 15;
            s_last_x = 160;
            s_last_y = 100;
            s_gfx_row = 0;
            s_gfx_col = 0;
        } else {
            return basic_error(5, "Illegal function call in SCREEN\n");
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        while (tok[p] == ',') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
                basic_val_t dummy;
                if (basic_eval_expr(tok, &p, &dummy) != 0) return -1;
            }
            while (tok[p] == ' ' || tok[p] == '\t') p++;
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_color(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_COLOR */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (s_screen_mode == 0) {
        /* Text mode: COLOR [fg] [, [bg] [, [border]]] */
        if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
            basic_val_t fg_val;
            if (basic_eval_expr(tok, &p, &fg_val) != 0) return -1;
            s_text_fg = (uint8_t)(fg_val.ival & 0x1F);
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
                basic_val_t bg_val;
                if (basic_eval_expr(tok, &p, &bg_val) != 0) return -1;
                s_text_bg = (uint8_t)(bg_val.ival & 0x07);
            }
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') {
                p++;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
                    basic_val_t border_val;
                    if (basic_eval_expr(tok, &p, &border_val) != 0) return -1;
                }
            }
        }
        s_text_attr = (uint8_t)((s_text_bg << 4) | (s_text_fg & 0x0F));
    } else if (s_screen_mode == 1) {
        /* CGA 320x200 mode: COLOR [bg] [, [palette]] */
        if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
            basic_val_t bg_val;
            if (basic_eval_expr(tok, &p, &bg_val) != 0) return -1;
            s_cga_bg = (uint8_t)(bg_val.ival & 0x0F);
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
                basic_val_t pal_val;
                if (basic_eval_expr(tok, &p, &pal_val) != 0) return -1;
                s_cga_palette = (uint8_t)(pal_val.ival & 0x01);
            }
        }
        os_gfx_set_palette(s_cga_palette, 1, s_cga_bg);
    } else {
        /* Screen mode 2, 12, 13: foreground color */
        if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
            basic_val_t fg_val;
            if (basic_eval_expr(tok, &p, &fg_val) != 0) return -1;
            s_gfx_fg = (uint8_t)fg_val.ival;
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_pset(const uint8_t __far *tok, uint16_t *pos, int is_preset) {
    uint16_t p = *pos;
    basic_val_t x_val, y_val;
    int16_t x, y;
    uint8_t color;

    p++; /* skip TOK_PSET / TOK_PRESET */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] != '(') {
        os_puts("Syntax error in PSET (expected '(')\n");
        return -1;
    }
    p++;

    if (basic_eval_expr(tok, &p, &x_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in PSET (expected ',')\n");
        return -1;
    }
    p++;
    if (basic_eval_expr(tok, &p, &y_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ')') {
        os_puts("Syntax error in PSET (expected ')')\n");
        return -1;
    }
    p++;

    x = x_val.ival;
    y = y_val.ival;

    if (is_preset) {
        color = 0;
    } else {
        color = (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1);
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        basic_val_t c_val;
        p++;
        if (basic_eval_expr(tok, &p, &c_val) != 0) return -1;
        color = (uint8_t)c_val.ival;
    }

    s_last_x = x;
    s_last_y = y;

    if (x >= 0 && y >= 0) {
        uint16_t max_x = (s_screen_mode == 2 || s_screen_mode == 12) ? 640 : 320;
        uint16_t max_y = (s_screen_mode == 12) ? 480 : 200;
        if ((uint16_t)x < max_x && (uint16_t)y < max_y) {
            os_gfx_putpixel((uint16_t)x, (uint16_t)y, color);
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_line_gfx(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    int16_t x1, y1, x2, y2;
    uint8_t color;
    int is_box = 0;
    int is_filled = 0;

    p++; /* skip TOK_LINE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == '(') {
        basic_val_t x1_val, y1_val;
        p++;
        if (basic_eval_expr(tok, &p, &x1_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            os_puts("Syntax error in LINE (expected ',')\n");
            return -1;
        }
        p++;
        if (basic_eval_expr(tok, &p, &y1_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ')') {
            os_puts("Syntax error in LINE (expected ')')\n");
            return -1;
        }
        p++;
        x1 = x1_val.ival;
        y1 = y1_val.ival;
    } else {
        x1 = s_last_x;
        y1 = s_last_y;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != '-' && tok[p] != TOK_MINUS) {
        os_puts("Syntax error in LINE (expected '-')\n");
        return -1;
    }
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] != '(') {
        os_puts("Syntax error in LINE (expected '(')\n");
        return -1;
    }
    p++;
    {
        basic_val_t x2_val, y2_val;
        if (basic_eval_expr(tok, &p, &x2_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            os_puts("Syntax error in LINE (expected ',')\n");
            return -1;
        }
        p++;
        if (basic_eval_expr(tok, &p, &y2_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ')') {
            os_puts("Syntax error in LINE (expected ')')\n");
            return -1;
        }
        p++;
        x2 = x2_val.ival;
        y2 = y2_val.ival;
    }

    color = (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1);
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
            basic_val_t c_val;
            if (basic_eval_expr(tok, &p, &c_val) != 0) return -1;
            color = (uint8_t)c_val.ival;
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == 'B' || tok[p] == 'b') {
                is_box = 1;
                p++;
                if (tok[p] == 'F' || tok[p] == 'f') {
                    is_filled = 1;
                    p++;
                }
            }
        }
    }

    s_last_x = x2;
    s_last_y = y2;

    if (is_box) {
        int16_t min_x = (x1 < x2) ? x1 : x2;
        int16_t max_x = (x1 > x2) ? x1 : x2;
        int16_t min_y = (y1 < y2) ? y1 : y2;
        int16_t max_y = (y1 > y2) ? y1 : y2;
        uint16_t w, h;
        if (min_x < 0) min_x = 0;
        if (min_y < 0) min_y = 0;
        w = (uint16_t)(max_x - min_x + 1);
        h = (uint16_t)(max_y - min_y + 1);
        os_gfx_rect((uint16_t)min_x, (uint16_t)min_y, w, h, color, (uint8_t)is_filled);
    } else {
        os_gfx_line(x1, y1, x2, y2, color);
    }

    *pos = p;
    return 0;
}

static void basic_draw_circle(int16_t xc, int16_t yc, int16_t r, uint8_t color) {
    int16_t x = 0;
    int16_t y = r;
    int16_t d = 1 - r;
    uint16_t max_x = (s_screen_mode == 2 || s_screen_mode == 12) ? 640 : 320;
    uint16_t max_y = (s_screen_mode == 12) ? 480 : 200;

    while (x <= y) {
        if (xc + x >= 0 && (uint16_t)(xc + x) < max_x && yc + y >= 0 && (uint16_t)(yc + y) < max_y)
            os_gfx_putpixel((uint16_t)(xc + x), (uint16_t)(yc + y), color);
        if (xc - x >= 0 && (uint16_t)(xc - x) < max_x && yc + y >= 0 && (uint16_t)(yc + y) < max_y)
            os_gfx_putpixel((uint16_t)(xc - x), (uint16_t)(yc + y), color);
        if (xc + x >= 0 && (uint16_t)(xc + x) < max_x && yc - y >= 0 && (uint16_t)(yc - y) < max_y)
            os_gfx_putpixel((uint16_t)(xc + x), (uint16_t)(yc - y), color);
        if (xc - x >= 0 && (uint16_t)(xc - x) < max_x && yc - y >= 0 && (uint16_t)(yc - y) < max_y)
            os_gfx_putpixel((uint16_t)(xc - x), (uint16_t)(yc - y), color);

        if (xc + y >= 0 && (uint16_t)(xc + y) < max_x && yc + x >= 0 && (uint16_t)(yc + x) < max_y)
            os_gfx_putpixel((uint16_t)(xc + y), (uint16_t)(yc + x), color);
        if (xc - y >= 0 && (uint16_t)(xc - y) < max_x && yc + x >= 0 && (uint16_t)(yc + x) < max_y)
            os_gfx_putpixel((uint16_t)(xc - y), (uint16_t)(yc + x), color);
        if (xc + y >= 0 && (uint16_t)(xc + y) < max_x && yc - x >= 0 && (uint16_t)(yc - x) < max_y)
            os_gfx_putpixel((uint16_t)(xc + y), (uint16_t)(yc - x), color);
        if (xc - y >= 0 && (uint16_t)(xc - y) < max_x && yc - x >= 0 && (uint16_t)(yc - x) < max_y)
            os_gfx_putpixel((uint16_t)(xc - y), (uint16_t)(yc - x), color);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

static int basic_exec_circle(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t xc_val, yc_val, r_val;
    int16_t xc, yc, r;
    uint8_t color;

    p += 2; /* skip 2-byte token TOK_CIRCLE (0xFE, 0x91) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] != '(') {
        os_puts("Syntax error in CIRCLE (expected '(')\n");
        return -1;
    }
    p++;
    if (basic_eval_expr(tok, &p, &xc_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in CIRCLE (expected ',')\n");
        return -1;
    }
    p++;
    if (basic_eval_expr(tok, &p, &yc_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ')') {
        os_puts("Syntax error in CIRCLE (expected ')')\n");
        return -1;
    }
    p++;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in CIRCLE (expected ',')\n");
        return -1;
    }
    p++;
    if (basic_eval_expr(tok, &p, &r_val) != 0) return -1;

    xc = xc_val.ival;
    yc = yc_val.ival;
    r = r_val.ival;
    color = (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1);

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        basic_val_t c_val;
        p++;
        if (basic_eval_expr(tok, &p, &c_val) != 0) return -1;
        color = (uint8_t)c_val.ival;
    }

    s_last_x = xc;
    s_last_y = yc;

    if (r > 0) {
        basic_draw_circle(xc, yc, r, color);
    }

    *pos = p;
    return 0;
}

/* --------------------------------------------------------------------------
 * Phase 14: Advanced Graphics (PAINT, GET/PUT Sprites, DRAW)
 * -------------------------------------------------------------------------- */

#define BASIC_PAINT_STACK_SIZE 512
static int16_t s_paint_stack_x[BASIC_PAINT_STACK_SIZE];
static int16_t s_paint_stack_y[BASIC_PAINT_STACK_SIZE];

static void basic_paint_fill(int16_t start_x, int16_t start_y, uint8_t paint_color, uint8_t border_color) {
    uint16_t max_x = (s_screen_mode == 2 || s_screen_mode == 12) ? 640 : 320;
    uint16_t max_y = (s_screen_mode == 12) ? 480 : 200;
    int sp = 0;

    if (start_x < 0 || (uint16_t)start_x >= max_x || start_y < 0 || (uint16_t)start_y >= max_y) return;

    {
        uint8_t init_c = bios_read_pixel((uint16_t)start_x, (uint16_t)start_y);
        if (init_c == border_color || init_c == paint_color) return;
    }

    s_paint_stack_x[sp] = start_x;
    s_paint_stack_y[sp] = start_y;
    sp++;

    while (sp > 0) {
        int16_t cur_x, cur_y;
        int16_t left_x, right_x;
        int16_t x;

        if (s_break_pending) break;
        if (s_running) basic_poll_events();

        sp--;
        cur_x = s_paint_stack_x[sp];
        cur_y = s_paint_stack_y[sp];

        if (cur_y < 0 || (uint16_t)cur_y >= max_y) continue;
        if (bios_read_pixel((uint16_t)cur_x, (uint16_t)cur_y) == border_color ||
            bios_read_pixel((uint16_t)cur_x, (uint16_t)cur_y) == paint_color) continue;

        /* Scan left */
        left_x = cur_x;
        while (left_x > 0) {
            uint8_t c = bios_read_pixel((uint16_t)(left_x - 1), (uint16_t)cur_y);
            if (c == border_color || c == paint_color) break;
            left_x--;
        }

        /* Scan right */
        right_x = cur_x;
        while ((uint16_t)(right_x + 1) < max_x) {
            uint8_t c = bios_read_pixel((uint16_t)(right_x + 1), (uint16_t)cur_y);
            if (c == border_color || c == paint_color) break;
            right_x++;
        }

        /* Draw horizontal scanline span from left_x to right_x */
        os_gfx_line((uint16_t)left_x, (uint16_t)cur_y, (uint16_t)right_x, (uint16_t)cur_y, paint_color);

        /* Inspect row above (cur_y - 1) */
        if (cur_y > 0) {
            int16_t ny = cur_y - 1;
            uint8_t in_span = 0;
            for (x = left_x; x <= right_x; x++) {
                uint8_t c = bios_read_pixel((uint16_t)x, (uint16_t)ny);
                if (c != border_color && c != paint_color) {
                    if (!in_span) {
                        if (sp < BASIC_PAINT_STACK_SIZE) {
                            s_paint_stack_x[sp] = x;
                            s_paint_stack_y[sp] = ny;
                            sp++;
                        }
                        in_span = 1;
                    }
                } else {
                    in_span = 0;
                }
            }
        }

        /* Inspect row below (cur_y + 1) */
        if ((uint16_t)(cur_y + 1) < max_y) {
            int16_t ny = cur_y + 1;
            uint8_t in_span = 0;
            for (x = left_x; x <= right_x; x++) {
                uint8_t c = bios_read_pixel((uint16_t)x, (uint16_t)ny);
                if (c != border_color && c != paint_color) {
                    if (!in_span) {
                        if (sp < BASIC_PAINT_STACK_SIZE) {
                            s_paint_stack_x[sp] = x;
                            s_paint_stack_y[sp] = ny;
                            sp++;
                        }
                        in_span = 1;
                    }
                } else {
                    in_span = 0;
                }
            }
        }
    }
}

static int basic_exec_paint(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t x_val, y_val;
    int16_t x, y;
    uint8_t paint_color, border_color;
    int is_step = 0;

    p += 2; /* skip 2-byte token TOK_PAINT (0xFE, 0x8F) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_STEP) {
        is_step = 1;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
    }

    if (tok[p] != '(') {
        return basic_error(2, "Syntax error in PAINT (expected '(')\n");
    }
    p++;

    if (basic_eval_expr(tok, &p, &x_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        return basic_error(2, "Syntax error in PAINT (expected ',')\n");
    }
    p++;
    if (basic_eval_expr(tok, &p, &y_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ')') {
        return basic_error(2, "Syntax error in PAINT (expected ')')\n");
    }
    p++;

    x = val_to_int(&x_val);
    y = val_to_int(&y_val);
    if (is_step) {
        x += s_last_x;
        y += s_last_y;
    }

    paint_color = (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1);
    border_color = paint_color;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',' && tok[p] != ':' && tok[p] != 0x00) {
            basic_val_t pc_val;
            if (basic_eval_expr(tok, &p, &pc_val) != 0) return -1;
            paint_color = (uint8_t)val_to_int(&pc_val);
            border_color = paint_color;
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] != ':' && tok[p] != 0x00) {
                basic_val_t bc_val;
                if (basic_eval_expr(tok, &p, &bc_val) != 0) return -1;
                border_color = (uint8_t)val_to_int(&bc_val);
            }
        }
    }

    s_last_x = x;
    s_last_y = y;

    if (s_screen_mode == 0) {
        return basic_error(5, "Illegal function call in PAINT\n");
    }

    basic_paint_fill(x, y, paint_color, border_color);

    *pos = p;
    return 0;
}

static int basic_exec_get_gfx(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t x1_val, y1_val, x2_val, y2_val;
    int16_t x1, y1, x2, y2;
    int is_step1 = 0, is_step2 = 0;
    char var_name[BASIC_MAX_VARNAME];
    basic_array_t *arr;
    uint16_t elem_offset = 0;
    uint16_t total_elems, elem_size, array_bytes;
    uint16_t min_x, max_x, min_y, max_y, w, h;
    uint16_t bits_per_pixel, width_in_bits, row_bytes, total_bytes;
    uint8_t __far *arr_ptr;

    p += 2; /* skip TOK_GET (0xFE, 0x89) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_STEP) {
        is_step1 = 1;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
    }

    if (tok[p] != '(') return basic_error(2, "Syntax error in GET (expected '(')\n");
    p++;
    if (basic_eval_expr(tok, &p, &x1_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') return basic_error(2, "Syntax error in GET (expected ',')\n");
    p++;
    if (basic_eval_expr(tok, &p, &y1_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ')') return basic_error(2, "Syntax error in GET (expected ')')\n");
    p++;

    x1 = val_to_int(&x1_val);
    y1 = val_to_int(&y1_val);
    if (is_step1) { x1 += s_last_x; y1 += s_last_y; }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != '-' && tok[p] != TOK_MINUS) return basic_error(2, "Syntax error in GET (expected '-')\n");
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_STEP) {
        is_step2 = 1;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
    }

    if (tok[p] != '(') return basic_error(2, "Syntax error in GET (expected '(')\n");
    p++;
    if (basic_eval_expr(tok, &p, &x2_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') return basic_error(2, "Syntax error in GET (expected ',')\n");
    p++;
    if (basic_eval_expr(tok, &p, &y2_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ')') return basic_error(2, "Syntax error in GET (expected ')')\n");
    p++;

    x2 = val_to_int(&x2_val);
    y2 = val_to_int(&y2_val);
    if (is_step2) { x2 += x1; y2 += y1; }

    s_last_x = x2;
    s_last_y = y2;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') return basic_error(2, "Syntax error in GET (expected ',')\n");
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
        return basic_error(2, "Syntax error in GET variable\n");
    }

    arr = basic_find_array(var_name);
    if (!arr) {
        arr = basic_create_default_array(var_name, 1);
        if (!arr) return -1;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == '(') {
        basic_val_t i1_val, i2_val;
        int16_t i1, i2 = 0;
        p++;
        if (basic_eval_expr(tok, &p, &i1_val) != 0) return -1;
        i1 = val_to_int(&i1_val);
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            if (basic_eval_expr(tok, &p, &i2_val) != 0) return -1;
            i2 = val_to_int(&i2_val);
            while (tok[p] == ' ' || tok[p] == '\t') p++;
        }
        if (tok[p] != ')') return basic_error(2, "Syntax error in array subscript (expected ')')\n");
        p++;
        if (i1 < 0 || (uint16_t)i1 >= arr->dim1 || (arr->num_dims == 2 && (i2 < 0 || (uint16_t)i2 >= arr->dim2))) {
            return basic_error(9, "Subscript out of range in GET\n");
        }
        {
            uint16_t linear_idx = (arr->num_dims == 1) ? (uint16_t)i1 : ((uint16_t)i1 * arr->dim2 + (uint16_t)i2);
            uint16_t sz = (arr->type == BASIC_TYPE_INT) ? 2 : ((arr->type == BASIC_TYPE_FLOAT) ? 4 : BASIC_STR_ELEM_LEN);
            elem_offset = linear_idx * sz;
        }
    }

    if (s_screen_mode == 0) return basic_error(5, "Illegal function call in GET\n");

    min_x = (x1 < x2) ? (uint16_t)x1 : (uint16_t)x2;
    max_x = (x1 > x2) ? (uint16_t)x1 : (uint16_t)x2;
    min_y = (y1 < y2) ? (uint16_t)y1 : (uint16_t)y2;
    max_y = (y1 > y2) ? (uint16_t)y1 : (uint16_t)y2;
    w = max_x - min_x + 1;
    h = max_y - min_y + 1;

    if (s_screen_mode == 1) {
        bits_per_pixel = 2;
    } else if (s_screen_mode == 2) {
        bits_per_pixel = 1;
    } else if (s_screen_mode == 12) {
        bits_per_pixel = 4;
    } else if (s_screen_mode == 13) {
        bits_per_pixel = 8;
    } else {
        bits_per_pixel = 2;
    }

    width_in_bits = w * bits_per_pixel;
    row_bytes = (width_in_bits + 7) >> 3;
    total_bytes = 4 + (row_bytes * h);

    total_elems = (arr->num_dims == 1) ? arr->dim1 : (arr->dim1 * arr->dim2);
    elem_size = (arr->type == BASIC_TYPE_INT) ? 2 : ((arr->type == BASIC_TYPE_FLOAT) ? 4 : BASIC_STR_ELEM_LEN);
    array_bytes = total_elems * elem_size;

    if (elem_offset + total_bytes > array_bytes) {
        return basic_error(9, "Subscript out of range (array too small for GET)\n");
    }

    arr_ptr = (uint8_t __far *)MK_FP(s_basic_seg, arr->data_off + elem_offset);
    arr_ptr[0] = (uint8_t)(width_in_bits & 0xFF);
    arr_ptr[1] = (uint8_t)(width_in_bits >> 8);
    arr_ptr[2] = (uint8_t)(h & 0xFF);
    arr_ptr[3] = (uint8_t)(h >> 8);

    {
        uint16_t off = 4;
        uint16_t ry, rx;
        for (ry = 0; ry < h; ry++) {
            uint16_t cur_y = min_y + ry;
            if (bits_per_pixel == 8) {
                for (rx = 0; rx < w; rx++) {
                    arr_ptr[off + rx] = bios_read_pixel(min_x + rx, cur_y);
                }
                off += w;
            } else if (bits_per_pixel == 2) {
                uint8_t byte_val = 0;
                uint8_t bit_cnt = 0;
                for (rx = 0; rx < w; rx++) {
                    uint8_t c = bios_read_pixel(min_x + rx, cur_y) & 0x03;
                    byte_val = (byte_val << 2) | c;
                    bit_cnt += 2;
                    if (bit_cnt == 8) {
                        arr_ptr[off++] = byte_val;
                        byte_val = 0;
                        bit_cnt = 0;
                    }
                }
                if (bit_cnt > 0) {
                    byte_val <<= (8 - bit_cnt);
                    arr_ptr[off++] = byte_val;
                }
            } else if (bits_per_pixel == 1) {
                uint8_t byte_val = 0;
                uint8_t bit_cnt = 0;
                for (rx = 0; rx < w; rx++) {
                    uint8_t c = bios_read_pixel(min_x + rx, cur_y) & 0x01;
                    byte_val = (byte_val << 1) | c;
                    bit_cnt++;
                    if (bit_cnt == 8) {
                        arr_ptr[off++] = byte_val;
                        byte_val = 0;
                        bit_cnt = 0;
                    }
                }
                if (bit_cnt > 0) {
                    byte_val <<= (8 - bit_cnt);
                    arr_ptr[off++] = byte_val;
                }
            } else if (bits_per_pixel == 4) {
                uint8_t byte_val = 0;
                uint8_t bit_cnt = 0;
                for (rx = 0; rx < w; rx++) {
                    uint8_t c = bios_read_pixel(min_x + rx, cur_y) & 0x0F;
                    byte_val = (byte_val << 4) | c;
                    bit_cnt += 4;
                    if (bit_cnt == 8) {
                        arr_ptr[off++] = byte_val;
                        byte_val = 0;
                        bit_cnt = 0;
                    }
                }
                if (bit_cnt > 0) {
                    byte_val <<= (8 - bit_cnt);
                    arr_ptr[off++] = byte_val;
                }
            }
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_put_gfx(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t x_val, y_val;
    int16_t x, y;
    int is_step = 0;
    char var_name[BASIC_MAX_VARNAME];
    basic_array_t *arr;
    uint16_t elem_offset = 0;
    uint8_t action = 4; /* Default = XOR */
    uint16_t bits_per_pixel, width_in_bits, w, h;
    uint16_t max_x, max_y;
    uint8_t mask;
    uint8_t __far *arr_ptr;

    p += 2; /* skip TOK_PUT (0xFE, 0x88) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_STEP) {
        is_step = 1;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
    }

    if (tok[p] != '(') return basic_error(2, "Syntax error in PUT (expected '(')\n");
    p++;
    if (basic_eval_expr(tok, &p, &x_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') return basic_error(2, "Syntax error in PUT (expected ',')\n");
    p++;
    if (basic_eval_expr(tok, &p, &y_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ')') return basic_error(2, "Syntax error in PUT (expected ')')\n");
    p++;

    x = val_to_int(&x_val);
    y = val_to_int(&y_val);
    if (is_step) { x += s_last_x; y += s_last_y; }
    s_last_x = x;
    s_last_y = y;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') return basic_error(2, "Syntax error in PUT (expected ',')\n");
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
        return basic_error(2, "Syntax error in PUT variable\n");
    }

    arr = basic_find_array(var_name);
    if (!arr) return basic_error(9, "Array not dimensioned in PUT\n");

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == '(') {
        basic_val_t i1_val, i2_val;
        int16_t i1, i2 = 0;
        p++;
        if (basic_eval_expr(tok, &p, &i1_val) != 0) return -1;
        i1 = val_to_int(&i1_val);
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            if (basic_eval_expr(tok, &p, &i2_val) != 0) return -1;
            i2 = val_to_int(&i2_val);
            while (tok[p] == ' ' || tok[p] == '\t') p++;
        }
        if (tok[p] != ')') return basic_error(2, "Syntax error in array subscript (expected ')')\n");
        p++;
        if (i1 < 0 || (uint16_t)i1 >= arr->dim1 || (arr->num_dims == 2 && (i2 < 0 || (uint16_t)i2 >= arr->dim2))) {
            return basic_error(9, "Subscript out of range in PUT\n");
        }
        {
            uint16_t linear_idx = (arr->num_dims == 1) ? (uint16_t)i1 : ((uint16_t)i1 * arr->dim2 + (uint16_t)i2);
            uint16_t sz = (arr->type == BASIC_TYPE_INT) ? 2 : ((arr->type == BASIC_TYPE_FLOAT) ? 4 : BASIC_STR_ELEM_LEN);
            elem_offset = linear_idx * sz;
        }
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == TOK_PSET) { action = 0; p++; }
        else if (tok[p] == TOK_PRESET) { action = 1; p++; }
        else if (tok[p] == TOK_AND) { action = 2; p++; }
        else if (tok[p] == TOK_OR) { action = 3; p++; }
        else if (tok[p] == TOK_XOR) { action = 4; p++; }
        else if (basic_strnicmp_far(&tok[p], "PSET", 4) == 0) { action = 0; p += 4; }
        else if (basic_strnicmp_far(&tok[p], "PRESET", 6) == 0) { action = 1; p += 6; }
        else if (basic_strnicmp_far(&tok[p], "AND", 3) == 0) { action = 2; p += 3; }
        else if (basic_strnicmp_far(&tok[p], "OR", 2) == 0) { action = 3; p += 2; }
        else if (basic_strnicmp_far(&tok[p], "XOR", 3) == 0) { action = 4; p += 3; }
    }

    if (s_screen_mode == 0) return basic_error(5, "Illegal function call in PUT\n");

    if (s_screen_mode == 1) {
        bits_per_pixel = 2;
        mask = 0x03;
    } else if (s_screen_mode == 2) {
        bits_per_pixel = 1;
        mask = 0x01;
    } else if (s_screen_mode == 12) {
        bits_per_pixel = 4;
        mask = 0x0F;
    } else if (s_screen_mode == 13) {
        bits_per_pixel = 8;
        mask = 0xFF;
    } else {
        bits_per_pixel = 2;
        mask = 0x03;
    }

    arr_ptr = (uint8_t __far *)MK_FP(s_basic_seg, arr->data_off + elem_offset);
    width_in_bits = (uint16_t)arr_ptr[0] | ((uint16_t)arr_ptr[1] << 8);
    h = (uint16_t)arr_ptr[2] | ((uint16_t)arr_ptr[3] << 8);
    w = width_in_bits / bits_per_pixel;

    max_x = (s_screen_mode == 2 || s_screen_mode == 12) ? 640 : 320;
    max_y = (s_screen_mode == 12) ? 480 : 200;

    {
        uint16_t off = 4;
        uint16_t ry, rx;
        for (ry = 0; ry < h; ry++) {
            int16_t cur_y = y + (int16_t)ry;
            if (bits_per_pixel == 8) {
                for (rx = 0; rx < w; rx++) {
                    int16_t cur_x = x + (int16_t)rx;
                    uint8_t sc = arr_ptr[off + rx];
                    if (cur_x >= 0 && (uint16_t)cur_x < max_x && cur_y >= 0 && (uint16_t)cur_y < max_y) {
                        uint8_t tc = bios_read_pixel((uint16_t)cur_x, (uint16_t)cur_y);
                        uint8_t fc;
                        if (action == 0) fc = sc;
                        else if (action == 1) fc = (~sc) & mask;
                        else if (action == 2) fc = tc & sc;
                        else if (action == 3) fc = tc | sc;
                        else fc = tc ^ sc;
                        os_gfx_putpixel((uint16_t)cur_x, (uint16_t)cur_y, fc);
                    }
                }
                off += w;
            } else if (bits_per_pixel == 2) {
                uint8_t byte_val = 0;
                uint8_t bit_cnt = 0;
                for (rx = 0; rx < w; rx++) {
                    int16_t cur_x = x + (int16_t)rx;
                    uint8_t sc;
                    if (bit_cnt == 0) {
                        byte_val = arr_ptr[off++];
                        bit_cnt = 8;
                    }
                    sc = (byte_val >> 6) & 0x03;
                    byte_val <<= 2;
                    bit_cnt -= 2;

                    if (cur_x >= 0 && (uint16_t)cur_x < max_x && cur_y >= 0 && (uint16_t)cur_y < max_y) {
                        uint8_t tc = bios_read_pixel((uint16_t)cur_x, (uint16_t)cur_y);
                        uint8_t fc;
                        if (action == 0) fc = sc;
                        else if (action == 1) fc = (~sc) & mask;
                        else if (action == 2) fc = tc & sc;
                        else if (action == 3) fc = tc | sc;
                        else fc = tc ^ sc;
                        os_gfx_putpixel((uint16_t)cur_x, (uint16_t)cur_y, fc);
                    }
                }
            } else if (bits_per_pixel == 1) {
                uint8_t byte_val = 0;
                uint8_t bit_cnt = 0;
                for (rx = 0; rx < w; rx++) {
                    int16_t cur_x = x + (int16_t)rx;
                    uint8_t sc;
                    if (bit_cnt == 0) {
                        byte_val = arr_ptr[off++];
                        bit_cnt = 8;
                    }
                    sc = (byte_val >> 7) & 0x01;
                    byte_val <<= 1;
                    bit_cnt--;

                    if (cur_x >= 0 && (uint16_t)cur_x < max_x && cur_y >= 0 && (uint16_t)cur_y < max_y) {
                        uint8_t tc = bios_read_pixel((uint16_t)cur_x, (uint16_t)cur_y);
                        uint8_t fc;
                        if (action == 0) fc = sc;
                        else if (action == 1) fc = (~sc) & mask;
                        else if (action == 2) fc = tc & sc;
                        else if (action == 3) fc = tc | sc;
                        else fc = tc ^ sc;
                        os_gfx_putpixel((uint16_t)cur_x, (uint16_t)cur_y, fc);
                    }
                }
            } else if (bits_per_pixel == 4) {
                uint8_t byte_val = 0;
                uint8_t bit_cnt = 0;
                for (rx = 0; rx < w; rx++) {
                    int16_t cur_x = x + (int16_t)rx;
                    uint8_t sc;
                    if (bit_cnt == 0) {
                        byte_val = arr_ptr[off++];
                        bit_cnt = 8;
                    }
                    sc = (byte_val >> 4) & 0x0F;
                    byte_val <<= 4;
                    bit_cnt -= 4;

                    if (cur_x >= 0 && (uint16_t)cur_x < max_x && cur_y >= 0 && (uint16_t)cur_y < max_y) {
                        uint8_t tc = bios_read_pixel((uint16_t)cur_x, (uint16_t)cur_y);
                        uint8_t fc;
                        if (action == 0) fc = sc;
                        else if (action == 1) fc = (~sc) & mask;
                        else if (action == 2) fc = tc & sc;
                        else if (action == 3) fc = tc | sc;
                        else fc = tc ^ sc;
                        os_gfx_putpixel((uint16_t)cur_x, (uint16_t)cur_y, fc);
                    }
                }
            }
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_draw(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t str_val;
    const char *s;

    p += 2; /* skip 2-byte token TOK_DRAW (0xFE, 0x92) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &str_val) != 0) return -1;
    if (str_val.type != BASIC_TYPE_STR || !str_val.sval) {
        return basic_error(13, "Type mismatch in DRAW\n");
    }

    if (s_screen_mode == 0) {
        return basic_error(5, "Illegal function call in DRAW\n");
    }

    s = str_val.sval;
    while (*s) {
        int blind = 0;
        int no_update = 0;
        char cmd;
        int16_t n = 1;
        int16_t dx = 0, dy = 0;
        int is_move = 0;
        int is_rel_move = 0;
        int16_t mx = 0, my = 0;

        while (*s == ' ' || *s == '\t' || *s == ';') s++;
        if (!*s) break;

        /* Check B (blind) and N (no-update) prefixes */
        for (;;) {
            if (*s == 'B' || *s == 'b') { blind = 1; s++; }
            else if (*s == 'N' || *s == 'n') { no_update = 1; s++; }
            else break;
        }
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;

        cmd = *s++;
        if (cmd >= 'a' && cmd <= 'z') cmd = (char)(cmd - 'a' + 'A');

        /* Parse number operand */
        while (*s == ' ' || *s == '\t') s++;
        if (cmd == 'M') {
            /* M [+/-]x, [+/-]y */
            int sx = 1, sy = 1;
            is_move = 1;
            if (*s == '+') { is_rel_move = 1; s++; }
            else if (*s == '-') { is_rel_move = 1; sx = -1; s++; }
            while (*s >= '0' && *s <= '9') {
                mx = mx * 10 + (*s++ - '0');
            }
            mx *= sx;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == ',') s++;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '+') { s++; }
            else if (*s == '-') { sy = -1; s++; }
            while (*s >= '0' && *s <= '9') {
                my = my * 10 + (*s++ - '0');
            }
            my *= sy;
        } else {
            if (*s >= '0' && *s <= '9') {
                n = 0;
                while (*s >= '0' && *s <= '9') {
                    n = n * 10 + (*s++ - '0');
                }
            } else {
                n = 1;
            }
        }

        if (cmd == 'C') {
            s_draw_color = (uint8_t)n;
            continue;
        } else if (cmd == 'S') {
            if (n < 1) n = 1;
            s_draw_scale = (uint8_t)n;
            continue;
        } else if (cmd == 'A') {
            s_draw_angle = (uint8_t)(n & 3);
            continue;
        } else if (cmd == 'X') {
            /* Substring execution: skip identifier */
            while (*s && *s != ';' && *s != ' ') s++;
            continue;
        }

        if (is_move) {
            int16_t target_x, target_y;
            if (is_rel_move) {
                target_x = s_last_x + mx;
                target_y = s_last_y + my;
            } else {
                target_x = mx;
                target_y = my;
            }
            if (!blind) {
                os_gfx_line((uint16_t)s_last_x, (uint16_t)s_last_y, (uint16_t)target_x, (uint16_t)target_y, s_draw_color);
            }
            if (!no_update) {
                s_last_x = target_x;
                s_last_y = target_y;
            }
            continue;
        }

        /* Directional commands: U, D, L, R, E, F, G, H */
        if (cmd == 'U') { dy = -n; }
        else if (cmd == 'D') { dy = n; }
        else if (cmd == 'L') { dx = -n; }
        else if (cmd == 'R') { dx = n; }
        else if (cmd == 'E') { dx = n; dy = -n; }
        else if (cmd == 'F') { dx = n; dy = n; }
        else if (cmd == 'G') { dx = -n; dy = n; }
        else if (cmd == 'H') { dx = -n; dy = -n; }
        else {
            continue;
        }

        /* Apply rotation angle: 0 = 0 deg, 1 = 90 deg, 2 = 180 deg, 3 = 270 deg */
        if (s_draw_angle == 1) {
            int16_t t = dx; dx = -dy; dy = t;
        } else if (s_draw_angle == 2) {
            dx = -dx; dy = -dy;
        } else if (s_draw_angle == 3) {
            int16_t t = dx; dx = dy; dy = -t;
        }

        /* Apply scale factor: scale / 4 */
        dx = (int16_t)(((int16_t)dx * s_draw_scale) >> 2);
        dy = (int16_t)(((int16_t)dy * s_draw_scale) >> 2);

        {
            int16_t target_x = s_last_x + dx;
            int16_t target_y = s_last_y + dy;
            if (!blind) {
                os_gfx_line((uint16_t)s_last_x, (uint16_t)s_last_y, (uint16_t)target_x, (uint16_t)target_y, s_draw_color);
            }
            if (!no_update) {
                s_last_x = target_x;
                s_last_y = target_y;
            }
        }
    }

    *pos = p;
    return 0;
}

static uint16_t basic_opl_calc_fnum(uint16_t freq, uint8_t shift);
#pragma aux basic_opl_calc_fnum = \
    "xor dx, dx" \
    "cmp cl, 16" \
    "jae shift_hi" \
    "mov ch, 16" \
    "sub ch, cl" \
    "mov dx, ax" \
    "xchg cl, ch" \
    "shr dx, cl" \
    "xchg cl, ch" \
    "shl ax, cl" \
    "jmp do_div" \
    "shift_hi:" \
    "sub cl, 16" \
    "shl ax, cl" \
    "mov dx, ax" \
    "xor ax, ax" \
    "do_div:" \
    "mov bx, 49716" \
    "div bx" \
    parm [ax] [cl] value [ax] modify [bx dx ch];

static void basic_sound_init_opl(void) {
    /* Waveform control enable */
    os_sound_opl_write(0x01, 0x20);
    os_sound_opl_write(0x08, 0x00);
    os_sound_opl_write(0xBD, 0x00);

    /* Channel 0 Modulator (Op 0) */
    os_sound_opl_write(0x20, 0x21); /* Multiplier=1, Sustain */
    os_sound_opl_write(0x40, 0x00); /* Attenuation 0 */
    os_sound_opl_write(0x60, 0xF2); /* Attack 15, Decay 2 */
    os_sound_opl_write(0x80, 0x0F); /* Fast release */
    os_sound_opl_write(0xE0, 0x00); /* Sine wave */

    /* Channel 0 Carrier (Op 3) */
    os_sound_opl_write(0x23, 0x21); /* Multiplier=1, Sustain */
    os_sound_opl_write(0x43, 0x3F); /* Start with carrier muted */
    os_sound_opl_write(0x63, 0xF2); /* Attack 15, Decay 2 */
    os_sound_opl_write(0x83, 0x0F); /* Fast release */
    os_sound_opl_write(0xE3, 0x00); /* Sine wave */

    /* Channel 0 Feedback & Output: Center stereo (bits 4-5 = 0x30) + FM mode */
    os_sound_opl_write(0xC0, 0x30);
}

static void basic_freq_to_opl(uint16_t freq, uint16_t *out_fnum, uint8_t *out_blk) {
    uint8_t blk;
    uint8_t shift;
    uint16_t fnum;

    if (freq <= 48) blk = 0;
    else if (freq <= 97) blk = 1;
    else if (freq <= 194) blk = 2;
    else if (freq <= 388) blk = 3;
    else if (freq <= 776) blk = 4;
    else if (freq <= 1553) blk = 5;
    else if (freq <= 3106) blk = 6;
    else blk = 7;

    shift = (uint8_t)(20 - blk);
    fnum = basic_opl_calc_fnum(freq, shift);
    if (fnum > 1023) fnum = 1023;

    *out_fnum = fnum;
    *out_blk = blk;
}

static void basic_sound_tone(uint16_t freq, uint16_t dur_ms) {
    if (freq < 19) {
        os_sound_opl_write(0xB0, 0x00);
        os_sound_opl_write(0x43, 0x3F);
        os_sound_stop();
        return;
    }

    /* Drive hobbyOS Unified Sound Subsystem (OPL FM / PC Speaker fallback) on Channel 0 */
    {
        uint16_t fnum;
        uint8_t blk;
        basic_freq_to_opl(freq, &fnum, &blk);
        os_sound_opl_write(0x43, 0x00); /* Unmute Carrier */
        os_sound_opl_write(0xA0, (uint8_t)(fnum & 0xFF));
        os_sound_opl_write(0xB0, (uint8_t)(0x20 | (blk << 2) | ((fnum >> 8) & 0x03)));
    }

    /* If duration specified, wait and mute */
    if (dur_ms > 0) {
        basic_delay_ms(dur_ms);
        os_sound_opl_write(0xB0, 0x00);
        os_sound_opl_write(0x43, 0x3F);
        os_sound_stop();
    }
}

static int basic_exec_beep(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_BEEP */
    basic_sound_tone(800, 150);
    *pos = p;
    return 0;
}

static int basic_exec_sound(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t freq_val, dur_val;
    uint16_t freq, dur_ms;

    p++; /* skip TOK_SOUND */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &freq_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in SOUND (expected ',')\n");
        return -1;
    }
    p++;
    if (basic_eval_expr(tok, &p, &dur_val) != 0) return -1;

    if (freq_val.ival < 37 || dur_val.ival < 0) {
        os_puts("Illegal function call in SOUND\n");
        return -1;
    }

    freq = (uint16_t)freq_val.ival;
    if (dur_val.ival > 1100) {
        dur_ms = 60000;
    } else {
        dur_ms = (uint16_t)dur_val.ival * 55;
    }

    basic_sound_tone(freq, dur_ms);

    *pos = p;
    return 0;
}

static const uint16_t c_octave4_freqs[12] = {
    262, /* 0: C  */
    277, /* 1: C# */
    294, /* 2: D  */
    311, /* 3: D# */
    330, /* 4: E  */
    349, /* 5: F  */
    370, /* 6: F# */
    392, /* 7: G  */
    415, /* 8: G# */
    440, /* 9: A  */
    466, /* 10: A# */
    494  /* 11: B */
};

static int basic_exec_play(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t mml_val;
    const char *s;

    p += 2; /* skip 2-byte token TOK_PLAY (0xFE, 0x93) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &mml_val) != 0) return -1;
    if (mml_val.type != BASIC_TYPE_STR || !mml_val.sval) {
        os_puts("Type mismatch in PLAY\n");
        return -1;
    }

    s = mml_val.sval;
    while (*s != '\0') {
        char ch = *s++;
        if (ch == ' ' || ch == '\t') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - ('a' - 'A'));

        if (ch == 'O') {
            if (*s >= '0' && *s <= '6') {
                s_play_octave = (uint8_t)(*s++ - '0');
            }
        } else if (ch == '<') {
            if (s_play_octave > 0) s_play_octave--;
        } else if (ch == '>') {
            if (s_play_octave < 6) s_play_octave++;
        } else if (ch == 'L') {
            uint16_t len = 0;
            while (*s >= '0' && *s <= '9') {
                len = len * 10 + (*s++ - '0');
            }
            if (len >= 1 && len <= 64) s_play_length = (uint8_t)len;
        } else if (ch == 'T') {
            uint16_t tempo = 0;
            while (*s >= '0' && *s <= '9') {
                tempo = tempo * 10 + (*s++ - '0');
            }
            if (tempo >= 32 && tempo <= 255) s_play_tempo = (uint8_t)tempo;
        } else if (ch == 'P') {
            uint16_t pause_len = s_play_length;
            uint16_t pause_ms;
            uint16_t quarter_ms, whole_ms;
            if (*s >= '0' && *s <= '9') {
                pause_len = 0;
                while (*s >= '0' && *s <= '9') {
                    pause_len = pause_len * 10 + (*s++ - '0');
                }
            }
            if (pause_len < 1 || pause_len > 64) pause_len = 4;
            quarter_ms = (uint16_t)(60000U / (uint16_t)s_play_tempo);
            whole_ms = quarter_ms * 4;
            pause_ms = whole_ms / pause_len;
            while (*s == '.') {
                s++;
                pause_ms += pause_ms / 2;
            }
            basic_sound_tone(0, 0);
            basic_delay_ms(pause_ms);
        } else if (ch == 'M') {
            char m2 = *s;
            if (m2 >= 'a' && m2 <= 'z') m2 = (char)(m2 - ('a' - 'A'));
            if (m2 == 'N') { s_play_style = 0; s++; }
            else if (m2 == 'L') { s_play_style = 1; s++; }
            else if (m2 == 'S') { s_play_style = 2; s++; }
            else if (m2 == 'F' || m2 == 'B') { s++; }
        } else if (ch >= 'A' && ch <= 'G') {
            int semitone;
            int octave = (int)s_play_octave;
            uint16_t note_len = s_play_length;
            uint16_t quarter_ms, whole_ms, dur_ms;
            uint16_t sound_dur, pause_dur;
            uint16_t freq;

            switch (ch) {
            case 'C': semitone = 0; break;
            case 'D': semitone = 2; break;
            case 'E': semitone = 4; break;
            case 'F': semitone = 5; break;
            case 'G': semitone = 7; break;
            case 'A': semitone = 9; break;
            case 'B': semitone = 11; break;
            default:  semitone = 0; break;
            }

            if (*s == '#' || *s == '+') {
                semitone++;
                s++;
            } else if (*s == '-') {
                semitone--;
                s++;
            }

            if (semitone < 0) {
                semitone += 12;
                if (octave > 0) octave--;
            } else if (semitone >= 12) {
                semitone -= 12;
                if (octave < 6) octave++;
            }

            if (*s >= '0' && *s <= '9') {
                note_len = 0;
                while (*s >= '0' && *s <= '9') {
                    note_len = note_len * 10 + (*s++ - '0');
                }
            }
            if (note_len < 1 || note_len > 64) note_len = s_play_length;

            quarter_ms = (uint16_t)(60000U / (uint16_t)s_play_tempo);
            whole_ms = quarter_ms * 4;
            dur_ms = whole_ms / note_len;

            while (*s == '.') {
                s++;
                dur_ms += dur_ms / 2;
            }

            if (s_play_style == 1) {
                sound_dur = dur_ms;
                pause_dur = 0;
            } else if (s_play_style == 2) {
                sound_dur = (dur_ms * 3) / 4;
                pause_dur = dur_ms - sound_dur;
            } else {
                sound_dur = (dur_ms * 7) / 8;
                pause_dur = dur_ms - sound_dur;
            }

            freq = c_octave4_freqs[semitone];
            if (octave < 4) {
                freq >>= (4 - octave);
            } else if (octave > 4) {
                freq <<= (octave - 4);
            }

            basic_sound_tone(freq, sound_dur);
            if (pause_dur > 0) {
                basic_sound_tone(0, 0);
                basic_delay_ms(pause_dur);
            }
        }
    }

    *pos = p;
    return 0;
}

/* --------------------------------------------------------------------------
 * System, Memory & Hardware Statements (Phase 7)
 * -------------------------------------------------------------------------- */

static int basic_exec_poke(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t addr_val, val_val;
    uint16_t addr;
    uint8_t val;

    p++; /* skip TOK_POKE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &addr_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in POKE (expected ',')\n");
        return -1;
    }
    p++;
    if (basic_eval_expr(tok, &p, &val_val) != 0) return -1;

    addr = (uint16_t)val_to_int(&addr_val);
    val = (uint8_t)(val_to_int(&val_val) & 0xFF);
    *((uint8_t __far *)MK_FP(s_def_seg, addr)) = val;

    *pos = p;
    return 0;
}

static int basic_exec_out(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t port_val, val_val;
    uint16_t port;
    uint8_t val;

    p++; /* skip TOK_OUT */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &port_val) != 0) return -1;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in OUT (expected ',')\n");
        return -1;
    }
    p++;
    if (basic_eval_expr(tok, &p, &val_val) != 0) return -1;

    port = (uint16_t)val_to_int(&port_val);
    val = (uint8_t)(val_to_int(&val_val) & 0xFF);
    outb(port, val);

    *pos = p;
    return 0;
}

static int basic_exec_def(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_DEF */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    /* DEF SEG [= segment] */
    if (basic_strnicmp_far(&tok[p], "SEG", 3) == 0) {
        p += 3;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == TOK_EQ || tok[p] == '=') {
            basic_val_t seg_val;
            p++;
            if (basic_eval_expr(tok, &p, &seg_val) != 0) return -1;
            s_def_seg = (uint16_t)val_to_int(&seg_val);
        } else if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != TOK_ELSE) {
            basic_val_t seg_val;
            if (basic_eval_expr(tok, &p, &seg_val) != 0) return -1;
            s_def_seg = (uint16_t)val_to_int(&seg_val);
        } else {
            s_def_seg = (uint16_t)get_ds();
        }
        *pos = p;
        return 0;
    }

    os_puts("Syntax error in DEF\n");
    return -1;
}

static int basic_exec_randomize(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_RANDOMIZE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != TOK_ELSE) {
        basic_val_t seed_val;
        if (basic_eval_expr(tok, &p, &seed_val) != 0) return -1;
        s_rnd_seed = (uint16_t)val_to_int(&seed_val);
    } else {
        char rbuf[32];
        os_puts("Random number seed (-32768 to 32767)? ");
        basic_read_line(rbuf, sizeof(rbuf));
        s_rnd_seed = (uint16_t)basic_atoi(rbuf);
    }

    *pos = p;
    return 0;
}

/* --------------------------------------------------------------------------
 * Phase 8: String Utilities & Formatting Helpers (MID$=, PRINT USING)
 * -------------------------------------------------------------------------- */

static void basic_out_char(uint8_t fnum, char c) {
    if (fnum == 0) {
        basic_term_putc(c);
    } else {
        basic_file_write_data(fnum, &c, 1);
    }
}

static void basic_out_str(uint8_t fnum, const char *s) {
    if (!s) return;
    if (fnum == 0) {
        basic_term_puts(s);
    } else {
        basic_file_write_data(fnum, s, basic_strlen(s));
    }
}

static const basic_float_t s_pow10_table[6] = {
    0x3F800000UL, /* 10^0 = 1.0 */
    0x41200000UL, /* 10^1 = 10.0 */
    0x42C80000UL, /* 10^2 = 100.0 */
    0x447A0000UL, /* 10^3 = 1000.0 */
    0x461C4000UL, /* 10^4 = 10000.0 */
    0x47C35000UL  /* 10^5 = 100000.0 */
};

static void basic_print_using_num(uint8_t fnum, const basic_val_t *arg,
                                  int num_digits_int, int num_digits_frac,
                                  int has_dot, int leading_sign, int trailing_sign,
                                  int star_fill, int dollar_sign, int thousands_comma) {
    basic_float_t f;
    int is_neg = 0;
    uint32_t scaled;
    char frac_digits[8];
    char int_digits[16];
    char int_buf[32];
    int fp_idx = 0;
    int ip_idx = 0;
    int ib_len = 0;
    int comma_cnt = 0;
    int k;
    int pad;
    int target_int_width;
    char sign_char = 0;

    if (num_digits_frac > 5) num_digits_frac = 5;

    f = val_to_float(arg);
    if ((f & 0x80000000UL) != 0 && f != 0) {
        is_neg = 1;
        f &= 0x7FFFFFFFUL;
    }

    /* Scale by 10^num_digits_frac and round */
    if (num_digits_frac > 0) {
        f = basic_float_mul(f, s_pow10_table[num_digits_frac]);
    }
    f = basic_float_add(f, 0x3F000000UL); /* + 0.5 */
    scaled = soft_to_uint32(f);

    /* Extract fractional digits (least significant first) */
    for (k = 0; k < num_digits_frac; k++) {
        uint16_t d = u32_div10(&scaled);
        frac_digits[fp_idx++] = (char)('0' + d);
    }

    /* Extract integer digits (least significant first) */
    if (scaled == 0) {
        int_digits[ip_idx++] = '0';
    } else {
        while (scaled > 0) {
            uint16_t d = u32_div10(&scaled);
            int_digits[ip_idx++] = (char)('0' + d);
        }
    }

    /* Check field overflow */
    if (ip_idx > num_digits_int) {
        basic_out_char(fnum, '%');
        if (arg->type == BASIC_TYPE_FLOAT) {
            char fbuf[24];
            basic_format_float(arg->fval, fbuf, sizeof(fbuf));
            basic_out_str(fnum, fbuf);
        } else if (arg->type == BASIC_TYPE_UINT) {
            char nbuf[16];
            uint16_t uv = (uint16_t)arg->ival;
            int np = 0;
            if (uv == 0) nbuf[np++] = '0';
            else {
                char tmp[8]; int tp = 0;
                while (uv > 0) { tmp[tp++] = (char)('0' + (uv % 10)); uv /= 10; }
                while (tp > 0) nbuf[np++] = tmp[--tp];
            }
            nbuf[np] = '\0';
            basic_out_str(fnum, nbuf);
        } else {
            char nbuf[16];
            int16_t v = arg->ival;
            int np = 0;
            if (v < 0) { nbuf[np++] = '-'; v = -v; }
            if (v == 0) nbuf[np++] = '0';
            else {
                char tmp[8]; int tp = 0;
                while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                while (tp > 0) nbuf[np++] = tmp[--tp];
            }
            nbuf[np] = '\0';
            basic_out_str(fnum, nbuf);
        }
        return;
    }

    /* Build formatted integer string with commas if requested */
    for (k = 0; k < ip_idx; k++) {
        if (thousands_comma && k > 0 && (k % 3) == 0) {
            int_buf[ib_len++] = ',';
            comma_cnt++;
        }
        int_buf[ib_len++] = int_digits[k];
    }
    /* Reverse int_buf */
    for (k = 0; k < ib_len / 2; k++) {
        char tmp = int_buf[k];
        int_buf[k] = int_buf[ib_len - 1 - k];
        int_buf[ib_len - 1 - k] = tmp;
    }
    int_buf[ib_len] = '\0';

    target_int_width = num_digits_int;
    if (thousands_comma) target_int_width += comma_cnt;

    pad = target_int_width - ib_len;

    if (leading_sign) {
        sign_char = is_neg ? '-' : '+';
    } else if (is_neg && !trailing_sign) {
        sign_char = '-';
    }

    if (sign_char != 0 && pad > 0) pad--;
    if (dollar_sign && pad > 0) pad--;

    /* Emit padding and prefix */
    if (star_fill) {
        while (pad-- > 0) basic_out_char(fnum, '*');
        if (dollar_sign) basic_out_char(fnum, '$');
        if (sign_char) basic_out_char(fnum, sign_char);
    } else {
        while (pad-- > 0) basic_out_char(fnum, ' ');
        if (sign_char) basic_out_char(fnum, sign_char);
        if (dollar_sign) basic_out_char(fnum, '$');
    }

    /* Emit integer part */
    basic_out_str(fnum, int_buf);

    /* Emit fractional part */
    if (has_dot) {
        basic_out_char(fnum, '.');
        for (k = num_digits_frac - 1; k >= 0; k--) {
            basic_out_char(fnum, frac_digits[k]);
        }
    }

    /* Emit trailing sign */
    if (trailing_sign == 1) {
        basic_out_char(fnum, is_neg ? '-' : '+');
    } else if (trailing_sign == -1) {
        basic_out_char(fnum, is_neg ? '-' : ' ');
    }
}

static int basic_exec_print_using(uint8_t fnum, const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t fmt_val;
    const char *fmt;
    uint16_t fmt_len;
    uint16_t fp = 0;
    int newline = 1;

    p++; /* skip TOK_USING */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &fmt_val) != 0) return -1;
    if (fmt_val.type != BASIC_TYPE_STR || !fmt_val.sval) {
        os_puts("Type mismatch in USING\n");
        return -1;
    }
    fmt = fmt_val.sval;
    fmt_len = basic_strlen(fmt);

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ';') p++;

    while (tok[p] != 0x00 && tok[p] != ':') {
        basic_val_t arg;
        int is_string = 0;
        int str_type = 0;
        int str_width = 0;
        int num_digits_int = 0;
        int num_digits_frac = 0;
        int has_dot = 0;
        int leading_sign = 0;
        int trailing_sign = 0;
        int star_fill = 0;
        int dollar_sign = 0;
        int thousands_comma = 0;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == 0x00 || tok[p] == ':' || tok[p] == TOK_ELSE) break;

        if (tok[p] == ';') {
            newline = 0;
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == 0x00 || tok[p] == ':' || tok[p] == TOK_ELSE) break;
            newline = 1;
        } else if (tok[p] == ',') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == 0x00 || tok[p] == ':' || tok[p] == TOK_ELSE) break;
            newline = 1;
        }

        if (basic_eval_expr(tok, &p, &arg) != 0) return -1;

        /* Scan format string for next field specifier */
        for (;;) {
            if (fp >= fmt_len) fp = 0;

            if (fmt[fp] == '_') {
                fp++;
                if (fp < fmt_len) basic_out_char(fnum, fmt[fp++]);
                continue;
            }

            /* String field: '!' */
            if (fmt[fp] == '!') {
                is_string = 1;
                str_type = '!';
                str_width = 1;
                fp++;
                break;
            }

            /* String field: '&' */
            if (fmt[fp] == '&') {
                is_string = 1;
                str_type = '&';
                fp++;
                break;
            }

            /* String field: '\ ... \' */
            if (fmt[fp] == '\\') {
                uint16_t sp_cnt = 0;
                uint16_t next_p = fp + 1;
                while (next_p < fmt_len && fmt[next_p] == ' ') {
                    sp_cnt++;
                    next_p++;
                }
                if (next_p < fmt_len && fmt[next_p] == '\\') {
                    is_string = 1;
                    str_type = '\\';
                    str_width = 2 + (int)sp_cnt;
                    fp = next_p + 1;
                    break;
                }
            }

            /* Numeric field starter: '+', '-', '#', '.', '*', '$' */
            if (fmt[fp] == '#' || fmt[fp] == '.' ||
                fmt[fp] == '+' ||
                (fmt[fp] == '*' && fp + 1 < fmt_len && fmt[fp + 1] == '*') ||
                (fmt[fp] == '$' && fp + 1 < fmt_len && fmt[fp + 1] == '$')) {

                is_string = 0;

                /* Check prefix */
                if (fmt[fp] == '+') {
                    leading_sign = 1;
                    fp++;
                }

                if (fp + 2 < fmt_len && fmt[fp] == '*' && fmt[fp + 1] == '*' && fmt[fp + 2] == '$') {
                    star_fill = 1;
                    dollar_sign = 1;
                    num_digits_int += 2;
                    fp += 3;
                } else if (fp + 1 < fmt_len && fmt[fp] == '*' && fmt[fp + 1] == '*') {
                    star_fill = 1;
                    num_digits_int += 2;
                    fp += 2;
                } else if (fp + 1 < fmt_len && fmt[fp] == '$' && fmt[fp + 1] == '$') {
                    dollar_sign = 1;
                    num_digits_int += 1;
                    fp += 2;
                }

                /* Scan '#' and ',' before decimal */
                while (fp < fmt_len && (fmt[fp] == '#' || fmt[fp] == ',')) {
                    if (fmt[fp] == '#') num_digits_int++;
                    else thousands_comma = 1;
                    fp++;
                }

                /* Scan decimal point and fractional '#' */
                if (fp < fmt_len && fmt[fp] == '.') {
                    has_dot = 1;
                    fp++;
                    while (fp < fmt_len && fmt[fp] == '#') {
                        num_digits_frac++;
                        fp++;
                    }
                }

                /* Scan trailing sign '+' or '-' */
                if (fp < fmt_len && fmt[fp] == '+') {
                    trailing_sign = 1;
                    fp++;
                } else if (fp < fmt_len && fmt[fp] == '-') {
                    trailing_sign = -1;
                    fp++;
                }

                break;
            }

            /* Literal character in format string */
            basic_out_char(fnum, fmt[fp++]);
        }

        /* Format the argument */
        if (is_string) {
            if (arg.type != BASIC_TYPE_STR) {
                os_puts("Type mismatch in PRINT USING\n");
                return -1;
            }
            if (str_type == '!') {
                const char *s = arg.sval ? arg.sval : "";
                basic_out_char(fnum, s[0] ? s[0] : ' ');
            } else if (str_type == '&') {
                basic_out_str(fnum, arg.sval ? arg.sval : "");
            } else if (str_type == '\\') {
                const char *s = arg.sval ? arg.sval : "";
                int slen = (int)basic_strlen(s);
                int out_len = (slen < str_width) ? slen : str_width;
                int pad_spaces = str_width - out_len;
                int k;
                for (k = 0; k < out_len; k++) basic_out_char(fnum, s[k]);
                while (pad_spaces-- > 0) basic_out_char(fnum, ' ');
            }
        } else {
            if (arg.type == BASIC_TYPE_STR) {
                os_puts("Type mismatch in PRINT USING\n");
                return -1;
            }
            basic_print_using_num(fnum, &arg, num_digits_int, num_digits_frac,
                                  has_dot, leading_sign, trailing_sign,
                                  star_fill, dollar_sign, thousands_comma);
        }
    }

    /* Print any trailing literal characters up to next field or end of format */
    while (fp < fmt_len) {
        if (fmt[fp] == '_' && fp + 1 < fmt_len) {
            fp++;
            basic_out_char(fnum, fmt[fp++]);
            continue;
        }
        if (fmt[fp] == '!' || fmt[fp] == '&' || fmt[fp] == '\\' ||
            fmt[fp] == '#' || fmt[fp] == '+' || fmt[fp] == '*' || fmt[fp] == '$') {
            break;
        }
        basic_out_char(fnum, fmt[fp++]);
    }

    if (newline) {
        basic_out_str(fnum, (fnum == 0) ? "\n" : "\r\n");
    }

    *pos = p;
    return 0;
}

static int basic_exec_mid_stmt(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    char var_name[BASIC_MAX_VARNAME];
    int is_array = 0;
    basic_val_t i1_val, i2_val;
    uint8_t num_dims = 1;
    basic_val_t start_val, len_val, expr_val;
    int16_t start, repl_len = 255;
    basic_var_t *var = 0;
    basic_array_t *arr = 0;
    char str_buf[BASIC_MAX_STR_LEN + 1];
    char *dst = 0;
    const char *src = 0;
    uint16_t dst_len, src_len, copy_len, i;

    p += 2; /* skip 0xFF, TOK_MID */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != '(') {
        os_puts("Syntax error in MID$ statement (expected '(')\n");
        return -1;
    }
    p++;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (!basic_parse_var_name(tok, &p, var_name, sizeof(var_name))) {
        os_puts("Syntax error in MID$ variable\n");
        return -1;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == '(') {
        /* Array element MID$(A$(i), ...) */
        is_array = 1;
        p++;
        if (basic_eval_expr(tok, &p, &i1_val) != 0) return -1;
        i2_val.ival = 0;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            num_dims = 2;
            if (basic_eval_expr(tok, &p, &i2_val) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
        }
        if (tok[p] != ')') {
            os_puts("Syntax error in array subscript\n");
            return -1;
        }
        p++;
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        os_puts("Syntax error in MID$ (expected ',')\n");
        return -1;
    }
    p++;

    if (basic_eval_expr(tok, &p, &start_val) != 0) return -1;
    start = val_to_int(&start_val);

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        if (basic_eval_expr(tok, &p, &len_val) != 0) return -1;
        repl_len = val_to_int(&len_val);
        while (tok[p] == ' ' || tok[p] == '\t') p++;
    }

    if (tok[p] != ')') {
        os_puts("Syntax error in MID$ (expected ')')\n");
        return -1;
    }
    p++;

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != TOK_EQ && tok[p] != '=') {
        os_puts("Syntax error in MID$ (expected '=')\n");
        return -1;
    }
    p++;

    if (basic_eval_expr(tok, &p, &expr_val) != 0) return -1;
    if (expr_val.type != BASIC_TYPE_STR) {
        os_puts("Type mismatch in MID$\n");
        return -1;
    }

    src = expr_val.sval ? expr_val.sval : "";
    src_len = basic_strlen(src);

    if (is_array) {
        basic_val_t cur_val;
        arr = basic_find_array(var_name);
        if (!arr || arr->type != BASIC_TYPE_STR) {
            os_puts("Type mismatch or array not found\n");
            return -1;
        }
        if (basic_read_array_elem(arr, i1_val.ival, i2_val.ival, &cur_val) != 0) return -1;
        basic_strncpy(str_buf, cur_val.sval ? cur_val.sval : "", sizeof(str_buf) - 1);
        str_buf[sizeof(str_buf) - 1] = '\0';
        dst = str_buf;
    } else {
        var = basic_find_var(var_name);
        if (!var || var->type != BASIC_TYPE_STR) {
            os_puts("Variable not found or type mismatch\n");
            return -1;
        }
        dst = var->sval;
    }

    dst_len = basic_strlen(dst);
    if (start > 0 && (uint16_t)start <= dst_len && repl_len > 0) {
        copy_len = (uint16_t)repl_len;
        if (copy_len > src_len) copy_len = src_len;
        if ((uint16_t)(start - 1) + copy_len > dst_len) {
            copy_len = dst_len - (uint16_t)(start - 1);
        }
        for (i = 0; i < copy_len; i++) {
            dst[(start - 1) + i] = src[i];
        }
    }

    if (is_array) {
        basic_val_t write_val;
        write_val.type = BASIC_TYPE_STR;
        write_val.sval = str_buf;
        write_val.ival = 0;
        write_val.fval = 0;
        if (basic_write_array_elem(arr, i1_val.ival, i2_val.ival, &write_val) != 0) return -1;
    }

    *pos = p;
    return 0;
}

/* --------------------------------------------------------------------------
 * Phase 9: Structured Error Handling (ON ERROR GOTO, RESUME, ERROR)
 * -------------------------------------------------------------------------- */

static int basic_exec_on(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_ON */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    /* ON ERROR GOTO <linenum> */
    if (tok[p] == TOK_ERROR) {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != TOK_GOTO) {
            return basic_error(2, "Syntax error in ON ERROR\n");
        }
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;

        {
            basic_val_t line_val;
            uint16_t target_line;
            if (basic_eval_expr(tok, &p, &line_val) != 0) return -1;
            target_line = (uint16_t)val_to_int(&line_val);
            s_error_goto_line = target_line;

            if (target_line == 0 && s_in_error_handler) {
                /* ON ERROR GOTO 0 inside error handler disables handler and re-raises error */
                s_in_error_handler = 0;
                s_err_reported = 1;
                s_running = 0;
                basic_print_error_name((uint8_t)s_err);
                os_puts(" in ");
                basic_print_uint(s_erl);
                os_putc('\n');
                return -1;
            }
        }
        *pos = p;
        return 0;
    }

    /* ON TIMER(interval) GOSUB <linenum> */
    if (tok[p] == 0xFE && tok[p + 1] == (uint8_t)(TOK_TIMER & 0xFF)) {
        basic_val_t int_val;
        int16_t interval;
        p += 2;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') return basic_error(2, "Syntax error in ON TIMER\n");
        p++;
        if (basic_eval_expr(tok, &p, &int_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;
        interval = val_to_int(&int_val);
        if (interval < 1) return basic_error(5, "Illegal function call\n");

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != TOK_GOSUB) return basic_error(2, "Syntax error in ON TIMER\n");
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;

        {
            basic_val_t line_val;
            if (basic_eval_expr(tok, &p, &line_val) != 0) return -1;
            s_timer_line = (uint16_t)val_to_int(&line_val);
            s_timer_interval = (uint16_t)interval;
            s_timer_pending = 0;
            s_timer_in_handler = 0;
        }
        *pos = p;
        return 0;
    }

    /* ON KEY(n) GOSUB <linenum> */
    if (tok[p] == TOK_KEY) {
        basic_val_t key_val;
        int16_t key_num;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') return basic_error(2, "Syntax error in ON KEY\n");
        p++;
        if (basic_eval_expr(tok, &p, &key_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;
        key_num = val_to_int(&key_val);
        if (key_num < 1 || key_num > 14) return basic_error(5, "Illegal function call\n");

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != TOK_GOSUB) return basic_error(2, "Syntax error in ON KEY\n");
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;

        {
            basic_val_t line_val;
            if (basic_eval_expr(tok, &p, &line_val) != 0) return -1;
            s_key_trap[key_num].line = (uint16_t)val_to_int(&line_val);
            s_key_trap[key_num].pending = 0;
            s_key_trap[key_num].in_handler = 0;
        }
        *pos = p;
        return 0;
    }

    /* Computed ON <expr> GOTO/GOSUB <line1>, <line2>, ... */
    {
        basic_val_t expr_val;
        int16_t idx;
        int is_gosub = 0;
        uint16_t cur_idx = 1;
        uint16_t target_line = 0;

        if (basic_eval_expr(tok, &p, &expr_val) != 0) return -1;
        idx = val_to_int(&expr_val);

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == TOK_GOTO) {
            is_gosub = 0;
        } else if (tok[p] == TOK_GOSUB) {
            is_gosub = 1;
        } else {
            return basic_error(2, "Syntax error in ON\n");
        }
        p++;

        while (tok[p] != 0x00 && tok[p] != ':' && tok[p] != TOK_ELSE) {
            basic_val_t line_val;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ',') p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == 0x00 || tok[p] == ':' || tok[p] == TOK_ELSE) break;

            if (basic_eval_expr(tok, &p, &line_val) != 0) return -1;
            if (cur_idx == (uint16_t)idx) {
                target_line = (uint16_t)val_to_int(&line_val);
            }
            cur_idx++;

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] != ',') break;
        }

        if (target_line != 0) {
            uint16_t target_off = basic_find_line(target_line);
            if (!target_off) {
                return basic_error(8, "Undefined line number\n");
            }
            if (is_gosub) {
                if (s_gosub_sp >= BASIC_MAX_GOSUB) {
                    return basic_error(7, "Out of memory\n");
                }
                s_gosub_stack[s_gosub_sp].line_off = s_cur_line_off;
                s_gosub_stack[s_gosub_sp].stmt_pos = p;
                s_gosub_sp++;
            }
            s_pc_line = target_off;
            s_pc_stmt = 4;
            s_jumped = 1;
        }

        *pos = p;
        return 0;
    }
}

/* --------------------------------------------------------------------------
 * Phase 10: TIMER and KEY Statements
 * -------------------------------------------------------------------------- */

static int basic_exec_timer(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p += 2; /* skip 0xFE, TOK_TIMER */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_ON) {
        p++;
        s_timer_state = 1;
        if (s_timer_interval > 0) {
            uint32_t now = *((const uint32_t __far *)MK_FP(0x0040, 0x006C));
            uint32_t dt = mul16(s_timer_interval, 18) + (uint32_t)(s_timer_interval / 5);
            s_timer_target_tick = now + dt;
        }
        *pos = p;
        return 0;
    }
    if (tok[p] == TOK_OFF) {
        p++;
        s_timer_state = 0;
        s_timer_pending = 0;
        *pos = p;
        return 0;
    }
    if (tok[p] == TOK_STOP) {
        p++;
        s_timer_state = 2;
        *pos = p;
        return 0;
    }

    return basic_error(2, "Syntax error in TIMER\n");
}

static int basic_exec_key(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_KEY */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    /* KEY(n) ON / OFF / STOP */
    if (tok[p] == '(') {
        basic_val_t key_val;
        int16_t k;
        p++;
        if (basic_eval_expr(tok, &p, &key_val) != 0) return -1;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;
        k = val_to_int(&key_val);
        if (k < 1 || k > 14) {
            return basic_error(5, "Illegal function call\n");
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == TOK_ON) {
            p++;
            s_key_trap[k].state = 1;
            *pos = p;
            return 0;
        }
        if (tok[p] == TOK_OFF) {
            p++;
            s_key_trap[k].state = 0;
            s_key_trap[k].pending = 0;
            *pos = p;
            return 0;
        }
        if (tok[p] == TOK_STOP) {
            p++;
            s_key_trap[k].state = 2;
            *pos = p;
            return 0;
        }
        return basic_error(2, "Syntax error in KEY\n");
    }

    /* KEY ON */
    if (tok[p] == TOK_ON) {
        p++;
        s_key_visible = 1;
        os_console_set_scroll_rows(24);
        {
            uint16_t cur = os_get_cursor();
            if ((uint8_t)(cur >> 8) >= 24) {
                os_set_cursor(23, (uint8_t)(cur & 0xFF));
            }
        }
        basic_display_softkeys();
        *pos = p;
        return 0;
    }

    /* KEY OFF */
    if (tok[p] == TOK_OFF) {
        p++;
        s_key_visible = 0;
        os_console_set_scroll_rows(25);
        basic_clear_softkeys();
        *pos = p;
        return 0;
    }

    /* KEY LIST */
    if (tok[p] == TOK_LIST) {
        int i;
        p++;
        for (i = 1; i <= 10; i++) {
            os_puts("F");
            basic_print_uint((uint16_t)i);
            os_puts(" ");
            os_puts(s_fkey_strings[i]);
            os_putc('\n');
        }
        *pos = p;
        return 0;
    }

    /* KEY n, "string" */
    {
        basic_val_t num_val;
        basic_val_t str_val;
        int16_t k;
        int i;

        if (basic_eval_expr(tok, &p, &num_val) != 0) return -1;
        k = val_to_int(&num_val);
        if (k < 1 || k > 10) {
            return basic_error(5, "Illegal function call\n");
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != ',') {
            return basic_error(2, "Syntax error in KEY (expected ',')\n");
        }
        p++;

        if (basic_eval_expr(tok, &p, &str_val) != 0) return -1;
        if (str_val.type != BASIC_TYPE_STR) {
            return basic_error(13, "Type mismatch\n");
        }

        for (i = 0; i < 15 && str_val.sval && str_val.sval[i] != '\0'; i++) {
            s_fkey_strings[k][i] = str_val.sval[i];
        }
        s_fkey_strings[k][i] = '\0';

        if (s_key_visible) {
            basic_display_softkeys();
        }

        *pos = p;
        return 0;
    }
}

static int basic_exec_resume(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p++; /* skip TOK_RESUME */

    if (!s_in_error_handler) {
        return basic_error(20, "RESUME without error\n");
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == TOK_NEXT) {
        /* RESUME NEXT */
        p++;
        s_in_error_handler = 0;
        if (s_err_next_line == 0) {
            /* Past the end of program */
            s_running = 0;
        } else {
            s_pc_line = s_err_next_line;
            s_pc_stmt = s_err_next_stmt;
            s_jumped = 1;
        }
        *pos = p;
        return 0;
    }

    if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != TOK_ELSE) {
        /* RESUME <linenum> or RESUME 0 */
        basic_val_t line_val;
        uint16_t target_line;
        if (basic_eval_expr(tok, &p, &line_val) != 0) return -1;
        target_line = (uint16_t)val_to_int(&line_val);
        if (target_line == 0) {
            /* RESUME 0 -> retry faulting statement */
            s_in_error_handler = 0;
            s_pc_line = s_err_line_off;
            s_pc_stmt = s_err_stmt_pos;
            s_jumped = 1;
        } else {
            uint16_t target_off = basic_find_line(target_line);
            if (!target_off) {
                return basic_error(8, "Undefined line number\n");
            }
            s_in_error_handler = 0;
            s_pc_line = target_off;
            s_pc_stmt = 4;
            s_jumped = 1;
        }
        *pos = p;
        return 0;
    }

    /* Bare RESUME -> retry faulting statement */
    s_in_error_handler = 0;
    s_pc_line = s_err_line_off;
    s_pc_stmt = s_err_stmt_pos;
    s_jumped = 1;
    *pos = p;
    return 0;
}

static int basic_exec_error_stmt(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t code_val;
    int16_t code;

    p++; /* skip TOK_ERROR */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &code_val) != 0) return -1;
    code = val_to_int(&code_val);
    if (code < 1 || code > 255) {
        return basic_error(5, "Illegal function call\n");
    }

    *pos = p;
    return basic_error((uint8_t)code, 0);
}

static int basic_exec_kill(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t f_val;
    char fname[32];
    p += 2; /* skip 0xFE, TOK_KILL */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &f_val) != 0) return -1;
    if (f_val.type != BASIC_TYPE_STR || !f_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(fname, f_val.sval, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
    if (os_file_delete(fname) != 0) {
        return basic_error(53, "File not found\n");
    }
    *pos = p;
    return 0;
}

static int basic_exec_name(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t old_val, new_val;
    char old_name[32];
    char new_name[32];
    p += 2; /* skip 0xFE, TOK_NAME */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &old_val) != 0) return -1;
    if (old_val.type != BASIC_TYPE_STR || !old_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(old_name, old_val.sval, sizeof(old_name) - 1);
    old_name[sizeof(old_name) - 1] = '\0';

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_strnicmp_far(&tok[p], "AS", 2) == 0) {
        p += 2;
    } else {
        return basic_error(2, "Syntax error in NAME (expected AS)\n");
    }

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &new_val) != 0) return -1;
    if (new_val.type != BASIC_TYPE_STR || !new_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(new_name, new_val.sval, sizeof(new_name) - 1);
    new_name[sizeof(new_name) - 1] = '\0';

    if (os_file_rename(old_name, (const char __far *)FP_HERE(new_name)) != 0) {
        return basic_error(53, "File not found\n");
    }
    *pos = p;
    return 0;
}

static int basic_exec_chdir(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t d_val;
    char dname[64];
    p += 2; /* skip 0xFE, TOK_CHDIR */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &d_val) != 0) return -1;
    if (d_val.type != BASIC_TYPE_STR || !d_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(dname, d_val.sval, sizeof(dname) - 1);
    dname[sizeof(dname) - 1] = '\0';
    if (os_chdir(dname) != 0) {
        return basic_error(76, "Path not found\n");
    }
    *pos = p;
    return 0;
}

static int basic_exec_mkdir(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t d_val;
    char dname[64];
    p += 2; /* skip 0xFE, TOK_MKDIR */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &d_val) != 0) return -1;
    if (d_val.type != BASIC_TYPE_STR || !d_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(dname, d_val.sval, sizeof(dname) - 1);
    dname[sizeof(dname) - 1] = '\0';
    if (os_mkdir(dname) != 0) {
        return basic_error(76, "Path not found\n");
    }
    *pos = p;
    return 0;
}

static int basic_exec_rmdir(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t d_val;
    char dname[64];
    p += 2; /* skip 0xFE, TOK_RMDIR */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &d_val) != 0) return -1;
    if (d_val.type != BASIC_TYPE_STR || !d_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(dname, d_val.sval, sizeof(dname) - 1);
    dname[sizeof(dname) - 1] = '\0';
    if (os_rmdir(dname) != 0) {
        return basic_error(76, "Path not found\n");
    }
    *pos = p;
    return 0;
}

static int basic_exec_shell(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    p += 2; /* skip 0xFE, TOK_SHELL */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == 0x00 || tok[p] == ':' || tok[p] == TOK_ELSE) {
        os_exec("shell.exe", "");
    } else {
        basic_val_t c_val;
        char cmd[64];
        char prog[32];
        const char *args = "";
        int cp = 0, pp = 0;
        if (basic_eval_expr(tok, &p, &c_val) != 0) return -1;
        if (c_val.type != BASIC_TYPE_STR || !c_val.sval) {
            return basic_error(13, "Type mismatch\n");
        }
        basic_strncpy(cmd, c_val.sval, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';

        while (cmd[cp] == ' ' || cmd[cp] == '\t') cp++;
        while (cmd[cp] != '\0' && cmd[cp] != ' ' && cmd[cp] != '\t' && pp < sizeof(prog) - 1) {
            prog[pp++] = cmd[cp++];
        }
        prog[pp] = '\0';
        while (cmd[cp] == ' ' || cmd[cp] == '\t') cp++;
        args = &cmd[cp];

        if (pp > 0) {
            if (os_exec(prog, args) < 0) {
                os_puts("Bad command or file name\n");
            }
        }
    }
    *pos = p;
    return 0;
}

static int basic_exec_bsave(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t f_val, off_val, len_val;
    char fname[32];
    uint16_t offset;
    uint16_t length;
    uint8_t hdr[7];
    uint16_t written;

    p++; /* skip TOK_BSAVE (0xC2) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &f_val) != 0) return -1;
    if (f_val.type != BASIC_TYPE_STR || !f_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(fname, f_val.sval, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        return basic_error(2, "Syntax error in BSAVE (expected ',')\n");
    }
    p++;

    if (basic_eval_expr(tok, &p, &off_val) != 0) return -1;
    offset = (uint16_t)val_to_int(&off_val);

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != ',') {
        return basic_error(2, "Syntax error in BSAVE (expected ',')\n");
    }
    p++;

    if (basic_eval_expr(tok, &p, &len_val) != 0) return -1;
    length = (uint16_t)val_to_int(&len_val);

    hdr[0] = 0xFD;
    hdr[1] = (uint8_t)(s_def_seg & 0xFF);
    hdr[2] = (uint8_t)(s_def_seg >> 8);
    hdr[3] = (uint8_t)(offset & 0xFF);
    hdr[4] = (uint8_t)(offset >> 8);
    hdr[5] = (uint8_t)(length & 0xFF);
    hdr[6] = (uint8_t)(length >> 8);

    written = os_file_write(fname, FP_HERE(hdr), 7);
    if (written < 7) {
        return basic_error(54, "Bad file mode\n");
    }

    if (length > 0) {
        written = os_file_append(fname, (const void __far *)MK_FP(s_def_seg, offset), length);
        if (written < length) {
            return basic_error(54, "Bad file mode\n");
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_bload(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t f_val;
    char fname[32];
    uint16_t target_offset = 0;
    int has_offset_override = 0;
    uint8_t hdr[7];
    uint16_t file_off, file_len;
    uint16_t nread;

    p++; /* skip TOK_BLOAD (0xC3) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &f_val) != 0) return -1;
    if (f_val.type != BASIC_TYPE_STR || !f_val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    basic_strncpy(fname, f_val.sval, sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        basic_val_t off_val;
        p++;
        if (basic_eval_expr(tok, &p, &off_val) != 0) return -1;
        target_offset = (uint16_t)val_to_int(&off_val);
        has_offset_override = 1;
    }

    nread = os_file_read_offset(fname, FP_HERE(hdr), 7, 0);
    if (nread < 7 || hdr[0] != 0xFD) {
        return basic_error(54, "Bad file mode\n");
    }

    file_off = (uint16_t)hdr[3] | ((uint16_t)hdr[4] << 8);
    file_len = (uint16_t)hdr[5] | ((uint16_t)hdr[6] << 8);

    if (!has_offset_override) {
        target_offset = file_off;
    }

    if (file_len > 0) {
        nread = os_file_read_offset(fname, (void __far *)MK_FP(s_def_seg, target_offset), file_len, 7);
        if (nread < file_len) {
            return basic_error(54, "Bad file mode\n");
        }
    }

    *pos = p;
    return 0;
}

static int basic_exec_width(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t w_val;
    int16_t cols;

    p++; /* skip TOK_WIDTH (0xA0) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_eval_expr(tok, &p, &w_val) != 0) return -1;
    cols = val_to_int(&w_val);

    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        basic_val_t r_val;
        p++;
        if (basic_eval_expr(tok, &p, &r_val) != 0) return -1;
    }

    if (cols == 40) {
        os_gfx_set_mode(0x01);
        s_screen_mode = 0;
        os_cls(s_text_attr ? s_text_attr : 0x07);
        os_set_cursor(0, 0);
    } else if (cols == 80) {
        os_gfx_set_mode(0x03);
        s_screen_mode = 0;
        os_cls(s_text_attr ? s_text_attr : 0x07);
        os_set_cursor(0, 0);
    } else {
        return basic_error(5, "Illegal function call\n");
    }

    if (s_key_visible) {
        os_console_set_scroll_rows(24);
        basic_display_softkeys();
    } else {
        os_console_set_scroll_rows(25);
    }

    *pos = p;
    return 0;
}

static int basic_exec_date_stmt(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t val;
    p += 2; /* skip 0xFE, TOK_DATE */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != TOK_EQ && tok[p] != '=') {
        return basic_error(2, "Syntax error in DATE$ (expected '=')\n");
    }
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &val) != 0) return -1;
    if (val.type != BASIC_TYPE_STR || !val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    if (basic_set_date_str(val.sval) != 0) return -1;
    *pos = p;
    return 0;
}

static int basic_exec_time_stmt(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    basic_val_t val;
    p += 2; /* skip 0xFE, TOK_TIME */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] != TOK_EQ && tok[p] != '=') {
        return basic_error(2, "Syntax error in TIME$ (expected '=')\n");
    }
    p++;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_eval_expr(tok, &p, &val) != 0) return -1;
    if (val.type != BASIC_TYPE_STR || !val.sval) {
        return basic_error(13, "Type mismatch\n");
    }
    if (basic_set_time_str(val.sval) != 0) return -1;
    *pos = p;
    return 0;
}

static int basic_exec_renum(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    uint16_t new_start = 10;
    uint16_t old_start = 0;
    uint16_t step = 10;

    p++; /* skip TOK_RENUM (0xAB) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (basic_is_digit((char)tok[p])) {
        new_start = basic_parse_uint_far(tok, &p);
    }
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (basic_is_digit((char)tok[p])) {
            old_start = basic_parse_uint_far(tok, &p);
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (basic_is_digit((char)tok[p])) {
                step = basic_parse_uint_far(tok, &p);
            }
        }
    }

    *pos = p;
    return basic_cmd_renum(new_start, old_start, step);
}

static int basic_exec_delete(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    uint16_t start = 0;
    uint16_t end = 65535;

    p++; /* skip TOK_DELETE (0xA9) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == '-' || tok[p] == TOK_MINUS) {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (basic_is_digit((char)tok[p])) {
            end = basic_parse_uint_far(tok, &p);
        } else {
            return basic_error(2, "Syntax error in DELETE\n");
        }
    } else if (basic_is_digit((char)tok[p])) {
        start = basic_parse_uint_far(tok, &p);
        end = start;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == '-' || tok[p] == TOK_MINUS) {
            p++;
            end = 65535;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (basic_is_digit((char)tok[p])) {
                end = basic_parse_uint_far(tok, &p);
            }
        }
    } else {
        return basic_error(2, "Syntax error in DELETE\n");
    }

    *pos = p;
    return basic_cmd_delete(start, end);
}

static int basic_exec_auto(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;
    uint16_t start = 10;
    uint16_t step = 10;

    p++; /* skip TOK_AUTO (0xAA) */
    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (basic_is_digit((char)tok[p])) {
        start = basic_parse_uint_far(tok, &p);
    }
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (tok[p] == ',') {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (basic_is_digit((char)tok[p])) {
            step = basic_parse_uint_far(tok, &p);
        }
    }

    s_auto_line = start;
    s_auto_step = (step > 0) ? step : 10;
    s_auto_mode = 1;
    *pos = p;
    return 0;
}

static int basic_exec_statement(const uint8_t __far *tok, uint16_t *pos) {
    uint16_t p = *pos;

    while (tok[p] == ' ' || tok[p] == '\t') p++;

    if (tok[p] == 0x00 || tok[p] == ':') {
        if (tok[p] == ':') p++;
        *pos = p;
        return 0;
    }

    if (tok[p] == TOK_ELSE) {
        while (tok[p] != 0x00) p++;
        *pos = p;
        return 0;
    }

    /* ON statement (ON ERROR GOTO, ON expr GOTO/GOSUB) */
    if (tok[p] == TOK_ON) {
        *pos = p;
        return basic_exec_on(tok, pos);
    }

    /* RESUME statement */
    if (tok[p] == TOK_RESUME) {
        *pos = p;
        return basic_exec_resume(tok, pos);
    }

    /* ERROR statement */
    if (tok[p] == TOK_ERROR) {
        *pos = p;
        return basic_exec_error_stmt(tok, pos);
    }

    /* KEY statement (KEY ON/OFF, KEY LIST, KEY n, "str", KEY(n) ON/OFF/STOP) */
    if (tok[p] == TOK_KEY) {
        *pos = p;
        return basic_exec_key(tok, pos);
    }

    /* TIMER statement (TIMER ON/OFF/STOP) */
    if (tok[p] == 0xFE && tok[p + 1] == (uint8_t)(TOK_TIMER & 0xFF)) {
        *pos = p;
        return basic_exec_timer(tok, pos);
    }

    /* TRON statement */
    if (tok[p] == TOK_TRON) {
        p++;
        s_tron = 1;
        *pos = p;
        return 0;
    }

    /* TROFF statement */
    if (tok[p] == TOK_TROFF) {
        p++;
        s_tron = 0;
        *pos = p;
        return 0;
    }

    /* CONT statement */
    if (tok[p] == TOK_CONT) {
        p++;
        *pos = p;
        return basic_cmd_cont();
    }

    /* DELETE statement */
    if (tok[p] == TOK_DELETE) {
        *pos = p;
        return basic_exec_delete(tok, pos);
    }

    /* AUTO statement */
    if (tok[p] == TOK_AUTO) {
        *pos = p;
        return basic_exec_auto(tok, pos);
    }

    /* RENUM statement */
    if (tok[p] == TOK_RENUM) {
        *pos = p;
        return basic_exec_renum(tok, pos);
    }

    /* WIDTH statement */
    if (tok[p] == TOK_WIDTH) {
        *pos = p;
        return basic_exec_width(tok, pos);
    }

    /* BSAVE statement */
    if (tok[p] == TOK_BSAVE) {
        *pos = p;
        return basic_exec_bsave(tok, pos);
    }

    /* BLOAD statement */
    if (tok[p] == TOK_BLOAD) {
        *pos = p;
        return basic_exec_bload(tok, pos);
    }

    /* Multi-byte 0xFE statements */
    if (tok[p] == 0xFE) {
        uint8_t fn = tok[p + 1];

        /* KILL statement (0xFE87) */
        if (fn == (uint8_t)(TOK_KILL & 0xFF)) {
            *pos = p;
            return basic_exec_kill(tok, pos);
        }

        /* NAME statement (0xFE84) */
        if (fn == (uint8_t)(TOK_NAME & 0xFF)) {
            *pos = p;
            return basic_exec_name(tok, pos);
        }

        /* CHDIR statement (0xFE97) */
        if (fn == (uint8_t)(TOK_CHDIR & 0xFF)) {
            *pos = p;
            return basic_exec_chdir(tok, pos);
        }

        /* MKDIR statement (0xFE98) */
        if (fn == (uint8_t)(TOK_MKDIR & 0xFF)) {
            *pos = p;
            return basic_exec_mkdir(tok, pos);
        }

        /* RMDIR statement (0xFE99) */
        if (fn == (uint8_t)(TOK_RMDIR & 0xFF)) {
            *pos = p;
            return basic_exec_rmdir(tok, pos);
        }

        /* SHELL statement (0xFE9A) */
        if (fn == (uint8_t)(TOK_SHELL & 0xFF)) {
            *pos = p;
            return basic_exec_shell(tok, pos);
        }

        /* DATE$ = expr$ (0xFE8D) */
        if (fn == (uint8_t)(TOK_DATE & 0xFF)) {
            *pos = p;
            return basic_exec_date_stmt(tok, pos);
        }

        /* TIME$ = expr$ (0xFE8E) */
        if (fn == (uint8_t)(TOK_TIME & 0xFF)) {
            *pos = p;
            return basic_exec_time_stmt(tok, pos);
        }
    }

    /* MID$(var$, start [, len]) = expr$ statement */

    /* MID$(var$, start [, len]) = expr$ statement */
    if (tok[p] == 0xFF && tok[p + 1] == (uint8_t)(TOK_MID & 0xFF)) {
        *pos = p;
        return basic_exec_mid_stmt(tok, pos);
    }

    /* PRINT statement (Console and File PRINT #n) */
    if (tok[p] == TOK_PRINT || tok[p] == '?') {
        int newline = 1;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;

        if (tok[p] == '#') {
            /* PRINT #filenum, expr ... */
            basic_val_t fnum_val;
            uint8_t fnum;
            p++;
            if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
            fnum = (uint8_t)fnum_val.ival;

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] != ',') {
                os_puts("Syntax error in PRINT # (expected ',')\n");
                return -1;
            }
            p++;

            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == TOK_USING) {
                *pos = p;
                return basic_exec_print_using(fnum, tok, pos);
            }

            while (tok[p] != 0x00 && tok[p] != ':') {
                basic_val_t val;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] == 0x00 || tok[p] == ':') break;

                if (tok[p] == ';') {
                    newline = 0;
                    p++;
                } else if (tok[p] == ',') {
                    basic_file_write_data(fnum, "              ", 14);
                    newline = 0;
                    p++;
                } else if (tok[p] == TOK_TAB) {
                    basic_val_t t_val;
                    int16_t target_col;
                    int16_t cur_col;
                    p++;
                    while (tok[p] == ' ' || tok[p] == '\t') p++;
                    if (basic_eval_expr(tok, &p, &t_val) != 0) return -1;
                    while (tok[p] == ' ' || tok[p] == '\t') p++;
                    if (tok[p] == ')') p++;
                    target_col = val_to_int(&t_val);
                    if (target_col < 1) target_col = 1;
                    cur_col = (int16_t)(((uint16_t)s_files[fnum - 1].file_pos % 80U) + 1);
                    if (target_col < cur_col) {
                        basic_file_write_data(fnum, "\r\n", 2);
                        cur_col = 1;
                    }
                    while (cur_col < target_col) {
                        basic_file_write_data(fnum, " ", 1);
                        cur_col++;
                    }
                    newline = 0;
                } else if (tok[p] == TOK_SPC) {
                    basic_val_t s_val;
                    int16_t count, k;
                    p++;
                    while (tok[p] == ' ' || tok[p] == '\t') p++;
                    if (basic_eval_expr(tok, &p, &s_val) != 0) return -1;
                    while (tok[p] == ' ' || tok[p] == '\t') p++;
                    if (tok[p] == ')') p++;
                    count = val_to_int(&s_val);
                    for (k = 0; k < count; k++) {
                        basic_file_write_data(fnum, " ", 1);
                    }
                    newline = 0;
                } else {
                    if (basic_eval_expr(tok, &p, &val) != 0) return -1;
                    if (val.type == BASIC_TYPE_STR) {
                        if (val.sval) basic_file_write_data(fnum, val.sval, basic_strlen(val.sval));
                    } else if (val.type == BASIC_TYPE_FLOAT) {
                        char fbuf[24];
                        int flen = basic_format_float(val.fval, fbuf, sizeof(fbuf));
                        fbuf[flen++] = ' ';
                        basic_file_write_data(fnum, fbuf, (uint16_t)flen);
                    } else if (val.type == BASIC_TYPE_UINT) {
                        char nbuf[16];
                        uint16_t uv = (uint16_t)val.ival;
                        int np = 0;
                        nbuf[np++] = ' ';
                        if (uv == 0) nbuf[np++] = '0';
                        else {
                            char tmp[8];
                            int tp = 0;
                            while (uv > 0) { tmp[tp++] = (char)('0' + (uv % 10)); uv /= 10; }
                            while (tp > 0) nbuf[np++] = tmp[--tp];
                        }
                        nbuf[np++] = ' ';
                        basic_file_write_data(fnum, nbuf, (uint16_t)np);
                    } else {
                        char nbuf[16];
                        int16_t v = val.ival;
                        int np = 0;
                        if (v >= 0) nbuf[np++] = ' ';
                        else { nbuf[np++] = '-'; v = -v; }
                        if (v == 0) nbuf[np++] = '0';
                        else {
                            char tmp[8];
                            int tp = 0;
                            while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                            while (tp > 0) nbuf[np++] = tmp[--tp];
                        }
                        nbuf[np++] = ' ';
                        basic_file_write_data(fnum, nbuf, (uint16_t)np);
                    }
                    newline = 1;
                }
            }
            if (newline) {
                basic_file_write_data(fnum, "\r\n", 2);
            }
            *pos = p;
            return 0;
        }

        /* Console PRINT */
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == TOK_USING) {
            *pos = p;
            return basic_exec_print_using(0, tok, pos);
        }

        while (tok[p] != 0x00 && tok[p] != ':') {
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == 0x00 || tok[p] == ':' || tok[p] == TOK_ELSE) break;

            if (tok[p] == ';') {
                newline = 0;
                p++;
            } else if (tok[p] == ',') {
                uint8_t col = (s_screen_mode == 0) ? (uint8_t)(os_get_cursor() & 0xFF) : s_gfx_col;
                uint8_t max_cols = (s_screen_mode == 2 || s_screen_mode == 12) ? 80 : 40;
                uint8_t next_tab = (uint8_t)(((col / 14) + 1) * 14);
                if (next_tab >= max_cols) {
                    basic_term_putc('\n');
                } else {
                    while (col < next_tab) {
                        basic_term_putc(' ');
                        col++;
                    }
                }
                newline = 0;
                p++;
            } else if (tok[p] == TOK_TAB) {
                basic_val_t t_val;
                int16_t target_col;
                uint8_t cur_col;
                uint8_t max_cols = (s_screen_mode == 2 || s_screen_mode == 12) ? 80 : 40;
                p++;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (basic_eval_expr(tok, &p, &t_val) != 0) return -1;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] == ')') p++;
                target_col = val_to_int(&t_val);
                if (target_col < 1) target_col = 1;
                if (target_col > (int16_t)max_cols) target_col = (int16_t)(((target_col - 1) % max_cols) + 1);
                cur_col = (s_screen_mode == 0) ? (uint8_t)((os_get_cursor() & 0xFF) + 1) : (uint8_t)(s_gfx_col + 1);
                if (target_col < (int16_t)cur_col) {
                    basic_term_putc('\n');
                    cur_col = 1;
                }
                while ((int16_t)cur_col < target_col) {
                    basic_term_putc(' ');
                    cur_col++;
                }
                newline = 0;
            } else if (tok[p] == TOK_SPC) {
                basic_val_t s_val;
                int16_t count, k;
                p++;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (basic_eval_expr(tok, &p, &s_val) != 0) return -1;
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] == ')') p++;
                count = val_to_int(&s_val);
                for (k = 0; k < count; k++) {
                    basic_term_putc(' ');
                }
                newline = 0;
            } else {
                basic_val_t val;
                if (basic_eval_expr(tok, &p, &val) != 0) {
                    return -1;
                }
                if (val.type == BASIC_TYPE_STR) {
                    basic_term_puts(val.sval ? val.sval : "");
                } else if (val.type == BASIC_TYPE_FLOAT) {
                    char fbuf[24];
                    basic_format_float(val.fval, fbuf, sizeof(fbuf));
                    basic_term_puts(fbuf);
                    basic_term_putc(' ');
                } else if (val.type == BASIC_TYPE_UINT) {
                    basic_term_putc(' ');
                    {
                        char nbuf[8];
                        uint16_t uv = (uint16_t)val.ival;
                        int np = 0;
                        if (uv == 0) {
                            basic_term_putc('0');
                        } else {
                            while (uv > 0) {
                                nbuf[np++] = (char)('0' + (uv % 10));
                                uv /= 10;
                            }
                            while (np > 0) basic_term_putc(nbuf[--np]);
                        }
                    }
                    basic_term_putc(' ');
                } else {
                    if (val.ival >= 0) basic_term_putc(' ');
                    {
                        char nbuf[8];
                        int16_t v = val.ival;
                        int np = 0;
                        if (v < 0) { basic_term_putc('-'); v = -v; }
                        if (v == 0) {
                            basic_term_putc('0');
                        } else {
                            while (v > 0) {
                                nbuf[np++] = (char)('0' + (v % 10));
                                v /= 10;
                            }
                            while (np > 0) basic_term_putc(nbuf[--np]);
                        }
                    }
                    basic_term_putc(' ');
                }
                newline = 1;
            }
        }

        if (newline) {
            basic_term_putc('\n');
        }
        *pos = p;
        return 0;
    }

    /* WRITE statement */
    if (tok[p] == TOK_WRITE) {
        *pos = p;
        return basic_exec_write_file(tok, pos);
    }

    /* LINE statement (LINE (x1,y1)-(x2,y2) or LINE INPUT) */
    if (tok[p] == TOK_LINE) {
        uint16_t save_p = p;
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == TOK_INPUT) {
            *pos = p;
            return basic_exec_line_input(tok, pos);
        }
        p = save_p;
        *pos = p;
        return basic_exec_line_gfx(tok, pos);
    }

    /* SCREEN statement */
    if (tok[p] == TOK_SCREEN) {
        *pos = p;
        return basic_exec_screen(tok, pos);
    }

    /* COLOR statement */
    if (tok[p] == TOK_COLOR) {
        *pos = p;
        return basic_exec_color(tok, pos);
    }

    /* PSET statement */
    if (tok[p] == TOK_PSET) {
        *pos = p;
        return basic_exec_pset(tok, pos, 0);
    }

    /* PRESET statement */
    if (tok[p] == TOK_PRESET) {
        *pos = p;
        return basic_exec_pset(tok, pos, 1);
    }

    /* CIRCLE statement (2-byte token 0xFE, 0x91) */
    if (tok[p] == (uint8_t)(TOK_CIRCLE >> 8) && tok[p + 1] == (uint8_t)(TOK_CIRCLE & 0xFF)) {
        *pos = p;
        return basic_exec_circle(tok, pos);
    }

    /* PAINT statement (2-byte token 0xFE, 0x8F) - Phase 14 */
    if (tok[p] == (uint8_t)(TOK_PAINT >> 8) && tok[p + 1] == (uint8_t)(TOK_PAINT & 0xFF)) {
        *pos = p;
        return basic_exec_paint(tok, pos);
    }

    /* DRAW statement (2-byte token 0xFE, 0x92) - Phase 14 */
    if (tok[p] == (uint8_t)(TOK_DRAW >> 8) && tok[p + 1] == (uint8_t)(TOK_DRAW & 0xFF)) {
        *pos = p;
        return basic_exec_draw(tok, pos);
    }

    /* BEEP statement */
    if (tok[p] == TOK_BEEP) {
        *pos = p;
        return basic_exec_beep(tok, pos);
    }

    /* SOUND statement */
    if (tok[p] == TOK_SOUND) {
        *pos = p;
        return basic_exec_sound(tok, pos);
    }

    /* PLAY statement (2-byte token 0xFE, 0x93) */
    if (tok[p] == (uint8_t)(TOK_PLAY >> 8) && tok[p + 1] == (uint8_t)(TOK_PLAY & 0xFF)) {
        *pos = p;
        return basic_exec_play(tok, pos);
    }

    /* POKE statement (Phase 7) */
    if (tok[p] == TOK_POKE) {
        *pos = p;
        return basic_exec_poke(tok, pos);
    }

    /* OUT statement (Phase 7) */
    if (tok[p] == TOK_OUT) {
        *pos = p;
        return basic_exec_out(tok, pos);
    }

    /* DEF statement (DEF SEG) (Phase 7) */
    if (tok[p] == TOK_DEF) {
        *pos = p;
        return basic_exec_def(tok, pos);
    }

    /* RANDOMIZE statement (Phase 7) */
    if (tok[p] == TOK_RANDOMIZE) {
        *pos = p;
        return basic_exec_randomize(tok, pos);
    }

    /* SYSTEM statement in program (2-byte token 0xFE, 0x83) */
    if (tok[p] == (uint8_t)(TOK_SYSTEM >> 8) && tok[p + 1] == (uint8_t)(TOK_SYSTEM & 0xFF)) {
        os_set_break_mode(BREAK_MODE_NORMAL);
        os_console_set_scroll_rows(25);
        basic_clear_softkeys();
        basic_sound_tone(0, 0);
        if (s_screen_mode != 0) {
            os_gfx_set_mode(0x03);
            s_screen_mode = 0;
        }
        basic_close_all_files();
        if (s_basic_seg) {
            os_mem_free(s_basic_seg);
            s_basic_seg = 0;
        }
        s_running = 0;
        os_exit(0);
        return 0;
    }

    /* LET statement */
    if (tok[p] == TOK_LET) {
        p++;
        *pos = p;
        return basic_exec_let(tok, pos);
    }

    /* INPUT statement */
    if (tok[p] == TOK_INPUT) {
        *pos = p;
        return basic_exec_input(tok, pos);
    }

    /* OPEN statement */
    if (tok[p] == TOK_OPEN) {
        *pos = p;
        return basic_exec_open(tok, pos);
    }

    /* CLOSE statement */
    if (tok[p] == TOK_CLOSE) {
        *pos = p;
        return basic_exec_close(tok, pos);
    }

    /* FIELD statement (2-byte token 0xFE, 0x82) - Phase 11 */
    if (tok[p] == (uint8_t)(TOK_FIELD >> 8) && tok[p + 1] == (uint8_t)(TOK_FIELD & 0xFF)) {
        *pos = p;
        return basic_exec_field(tok, pos);
    }

    /* LSET statement (2-byte token 0xFE, 0x85) - Phase 11 */
    if (tok[p] == (uint8_t)(TOK_LSET >> 8) && tok[p + 1] == (uint8_t)(TOK_LSET & 0xFF)) {
        *pos = p;
        return basic_exec_lset_rset(tok, pos, 0);
    }

    /* RSET statement (2-byte token 0xFE, 0x86) - Phase 11 */
    if (tok[p] == (uint8_t)(TOK_RSET >> 8) && tok[p + 1] == (uint8_t)(TOK_RSET & 0xFF)) {
        *pos = p;
        return basic_exec_lset_rset(tok, pos, 1);
    }

    /* PUT statement (2-byte token 0xFE, 0x88) - Phase 11 / 14 */
    if (tok[p] == (uint8_t)(TOK_PUT >> 8) && tok[p + 1] == (uint8_t)(TOK_PUT & 0xFF)) {
        uint16_t chk_p = p + 2;
        while (tok[chk_p] == ' ' || tok[chk_p] == '\t') chk_p++;
        if (tok[chk_p] == '(' || tok[chk_p] == TOK_STEP) {
            *pos = p;
            return basic_exec_put_gfx(tok, pos);
        }
        *pos = p;
        return basic_exec_put(tok, pos);
    }

    /* GET statement (2-byte token 0xFE, 0x89) - Phase 11 / 14 */
    if (tok[p] == (uint8_t)(TOK_GET >> 8) && tok[p + 1] == (uint8_t)(TOK_GET & 0xFF)) {
        uint16_t chk_p = p + 2;
        while (tok[chk_p] == ' ' || tok[chk_p] == '\t') chk_p++;
        if (tok[chk_p] == '(' || tok[chk_p] == TOK_STEP) {
            *pos = p;
            return basic_exec_get_gfx(tok, pos);
        }
        *pos = p;
        return basic_exec_get(tok, pos);
    }

    /* SAVE statement */
    if (tok[p] == TOK_SAVE) {
        *pos = p;
        return basic_exec_save(tok, pos);
    }

    /* LOAD statement */
    if (tok[p] == TOK_LOAD) {
        *pos = p;
        return basic_exec_load(tok, pos);
    }

    /* FILES statement (2-byte token 0xFE, 0x81) */
    if (tok[p] == (uint8_t)(TOK_FILES >> 8) && tok[p + 1] == (uint8_t)(TOK_FILES & 0xFF)) {
        char pat[32];
        p += 2;
        pat[0] = '\0';
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != 0 && tok[p] != ':') {
            basic_val_t pat_val;
            basic_eval_expr(tok, &p, &pat_val);
            if (pat_val.type == BASIC_TYPE_STR && pat_val.sval) {
                basic_strncpy(pat, pat_val.sval, sizeof(pat) - 1);
                pat[sizeof(pat) - 1] = '\0';
            }
        }
        basic_cmd_files(pat);
        *pos = p;
        return 0;
    }

    /* IF statement */
    if (tok[p] == TOK_IF) {
        *pos = p;
        return basic_exec_if(tok, pos);
    }

    /* FOR statement */
    if (tok[p] == TOK_FOR) {
        *pos = p;
        return basic_exec_for(tok, pos);
    }

    /* NEXT statement */
    if (tok[p] == TOK_NEXT) {
        *pos = p;
        return basic_exec_next(tok, pos);
    }

    /* WHILE statement */
    if (tok[p] == TOK_WHILE) {
        *pos = p;
        return basic_exec_while(tok, pos);
    }

    /* WEND statement */
    if (tok[p] == TOK_WEND) {
        *pos = p;
        return basic_exec_wend(tok, pos);
    }

    /* DIM statement */
    if (tok[p] == TOK_DIM) {
        *pos = p;
        return basic_exec_dim(tok, pos);
    }

    /* DATA statement (skip during execution) */
    if (tok[p] == TOK_DATA) {
        while (tok[p] != 0x00 && tok[p] != ':') p++;
        *pos = p;
        return 0;
    }

    /* READ statement */
    if (tok[p] == TOK_READ) {
        *pos = p;
        return basic_exec_read(tok, pos);
    }

    /* RESTORE statement */
    if (tok[p] == TOK_RESTORE) {
        *pos = p;
        return basic_exec_restore(tok, pos);
    }

    /* LOCATE statement */
    if (tok[p] == TOK_LOCATE) {
        *pos = p;
        return basic_exec_locate(tok, pos);
    }

    /* SWAP statement */
    if (tok[p] == TOK_SWAP) {
        *pos = p;
        return basic_exec_swap(tok, pos);
    }

    /* GOTO statement */
    if (tok[p] == TOK_GOTO) {
        *pos = p;
        return basic_exec_goto(tok, pos);
    }

    /* GOSUB statement */
    if (tok[p] == TOK_GOSUB) {
        *pos = p;
        return basic_exec_gosub(tok, pos);
    }

    /* RETURN statement */
    if (tok[p] == TOK_RETURN) {
        *pos = p;
        return basic_exec_return(tok, pos);
    }

    /* CLS statement */
    if (tok[p] == TOK_CLS) {
        p++;
        if (s_screen_mode == 0) {
            os_cls(s_text_attr ? s_text_attr : 0x07);
            os_set_cursor(0, 0);
            if (s_key_visible) {
                os_console_set_scroll_rows(24);
                basic_display_softkeys();
            } else {
                os_console_set_scroll_rows(25);
            }
        } else {
            os_gfx_clear(0);
            s_gfx_row = 0;
            s_gfx_col = 0;
        }
        *pos = p;
        return 0;
    }

    /* CLEAR statement */
    if (tok[p] == TOK_CLEAR) {
        p++;
        basic_clear_vars();
        s_num_arrays = 0;
        s_array_heap_free = s_array_heap_start;
        *pos = p;
        return 0;
    }

    /* REM / ' statement: ignore remainder of line */
    if (tok[p] == TOK_REM || tok[p] == TOK_SQUOTE) {
        while (tok[p] != 0x00) p++;
        *pos = p;
        return 0;
    }

    /* STOP statement */
    if (tok[p] == TOK_STOP) {
        s_running = 0;
        s_cont_line_off = s_cur_line_off;
        s_cont_stmt_pos = s_cur_next_stmt;
        s_can_cont = 1;
        while (tok[p] != 0x00) p++;
        *pos = p;
        os_puts("Break in ");
        basic_print_uint(s_cur_line_num);
        os_putc('\n');
        return 0;
    }

    /* END statement */
    if (tok[p] == TOK_END) {
        s_running = 0;
        s_can_cont = 0;
        while (tok[p] != 0x00) p++;
        *pos = p;
        return 0;
    }

    /* Implicit LET (variable or array assignment starting with letter) */
    if (basic_is_alpha((char)tok[p])) {
        *pos = p;
        return basic_exec_let(tok, pos);
    }

    os_puts("Syntax error\n");
    s_running = 0;
    return -1;
}

static int basic_exec_line(const uint8_t __far *tok) {
    uint16_t p = 0;
    while (tok[p] != 0x00 && s_running && !s_jumped) {
        if (basic_exec_statement(tok, &p) != 0) {
            return -1;
        }
        if (s_jumped) break;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ':') {
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
        } else if (tok[p] == TOK_ELSE) {
            while (tok[p] != 0x00) p++;
        }
    }
    return 0;
}

static void basic_scan_stmt_boundary(const uint8_t __far *line_tok, uint16_t cur_pos,
                                     uint16_t next_line,
                                     uint16_t *out_next_line, uint16_t *out_next_stmt) {
    uint16_t p = cur_pos;
    while (line_tok[p] != 0x00) {
        if (line_tok[p] == '"') {
            p++;
            while (line_tok[p] != 0x00 && line_tok[p] != '"') p++;
            if (line_tok[p] == '"') p++;
            continue;
        }
        if (line_tok[p] == TOK_REM || line_tok[p] == TOK_SQUOTE) {
            break;
        }
        if (line_tok[p] == ':') {
            p++;
            while (line_tok[p] == ' ' || line_tok[p] == '\t') p++;
            *out_next_line = s_cur_line_off;
            *out_next_stmt = p;
            return;
        }
        if (line_tok[p] == TOK_ELSE) {
            break;
        }
        p++;
    }
    *out_next_line = next_line;
    *out_next_stmt = 4;
}

static void basic_run_from_pc(void) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);

    while (s_pc_line != 0 && s_running) {
        uint16_t next_line = (uint16_t)mem[s_pc_line] | ((uint16_t)mem[s_pc_line + 1] << 8);
        uint16_t num       = (uint16_t)mem[s_pc_line + 2] | ((uint16_t)mem[s_pc_line + 3] << 8);

        s_cur_line_off = s_pc_line;
        s_cur_line_num = num;

        if (s_break_pending) {
            s_break_pending = 0;
            basic_sound_tone(0, 0);
            if (s_screen_mode != 0) {
                os_gfx_set_mode(0x03);
                s_screen_mode = 0;
                if (s_key_visible) {
                    os_console_set_scroll_rows(24);
                    basic_display_softkeys();
                } else {
                    os_console_set_scroll_rows(25);
                }
            }
            s_cont_line_off = s_cur_line_off;
            s_cont_stmt_pos = s_pc_stmt;
            s_can_cont = 1;
            os_puts("\nBreak in ");
            basic_print_uint(num);
            os_putc('\n');
            s_running = 0;
            break;
        }

        s_jumped = 0;

        if (s_tron && s_pc_stmt == 4) {
            os_putc('[');
            basic_print_uint(num);
            os_putc(']');
        }

        while (mem[s_cur_line_off + s_pc_stmt] != 0x00 && s_running && !s_jumped) {
            uint16_t pos = s_pc_stmt;
            s_cur_stmt_pos = s_pc_stmt;
            basic_scan_stmt_boundary(&mem[s_cur_line_off], s_pc_stmt, next_line,
                                     &s_cur_next_line, &s_cur_next_stmt);

            /* Poll events at statement boundary */
            basic_poll_events();
            if (s_jumped) {
                /* Event triggered GOSUB */
                break;
            }

            if (basic_exec_statement(&mem[s_cur_line_off], &pos) != 0) {
                if (s_jumped) {
                    /* Trapped by ON ERROR GOTO */
                    break;
                }
                if (s_err_reported) {
                    s_err_reported = 0;
                    s_running = 0;
                    break;
                }
                if (s_screen_mode != 0) {
                    os_gfx_set_mode(0x03);
                    s_screen_mode = 0;
                }
                os_puts(" in ");
                basic_print_uint(num);
                os_putc('\n');
                s_running = 0;
                break;
            }
            if (s_jumped) break;

            while (mem[s_cur_line_off + pos] == ' ' || mem[s_cur_line_off + pos] == '\t') pos++;
            if (mem[s_cur_line_off + pos] == ':') {
                pos++;
                while (mem[s_cur_line_off + pos] == ' ' || mem[s_cur_line_off + pos] == '\t') pos++;
            } else if (mem[s_cur_line_off + pos] == TOK_ELSE) {
                while (mem[s_cur_line_off + pos] != 0x00) pos++;
            }
            s_pc_stmt = pos;
        }

        if (!s_running) break;

        if (!s_jumped) {
            s_pc_line = next_line;
            s_pc_stmt = 4;
        }
    }

    s_running = 0;
    if (s_pc_line == 0) {
        s_can_cont = 0;
    }
}

static int basic_cmd_cont(void) {
    if (!s_can_cont || s_cont_line_off == 0) {
        return basic_error(17, "Can't continue\n");
    }
    s_running = 1;
    s_pc_line = s_cont_line_off;
    s_pc_stmt = s_cont_stmt_pos;
    basic_run_from_pc();
    return 0;
}

static void basic_cmd_run(void) {
    basic_clear_vars();
    basic_close_all_files();
    s_gosub_sp = 0;
    s_for_sp = 0;
    s_while_sp = 0;
    s_num_arrays = 0;
    s_array_heap_start = (s_prog_tail + 15) & ~15;
    s_array_heap_free  = s_array_heap_start;
    s_data_line_off = s_prog_head;
    s_data_stmt_pos = 0;
    s_last_x = 0;
    s_last_y = 0;
    s_gfx_row = 0;
    s_gfx_col = 0;
    s_error_goto_line = 0;
    s_err = 0;
    s_erl = 0;
    s_in_error_handler = 0;
    s_err_reported = 0;
    s_timer_state = 0;
    s_timer_pending = 0;
    s_timer_in_handler = 0;
    s_timer_interval = 0;
    s_timer_line = 0;
    {
        int k;
        for (k = 1; k <= 14; k++) {
            s_key_trap[k].line = 0;
            s_key_trap[k].state = 0;
            s_key_trap[k].pending = 0;
            s_key_trap[k].in_handler = 0;
        }
    }
    s_key_fifo_head = 0;
    s_key_fifo_count = 0;
    s_break_pending = 0;
    s_can_cont = 0;
    s_auto_mode = 0;
    s_draw_angle = 0;
    s_draw_scale = 4;
    s_draw_color = (s_screen_mode == 1) ? 3 : ((s_screen_mode == 12 || s_screen_mode == 13) ? s_gfx_fg : 1);
    s_cont_line_off = 0;
    s_cont_stmt_pos = 4;
    s_err_reported = 0;
    s_running = 1;
    s_pc_line = s_prog_head;
    s_pc_stmt = 4;

    basic_run_from_pc();
}

/* --------------------------------------------------------------------------
 * Interactive REPL & Console Input
 * -------------------------------------------------------------------------- */

static uint16_t basic_read_line(char *buf, uint16_t max_len) {
    uint16_t len = 0;
    for (;;) {
        uint16_t key = basic_getc();
        uint8_t  u   = (uint8_t)(key & 0xFF);
        char     c   = (char)u;

        if (c == 0x03) {
            basic_sound_tone(0, 0);
            if (s_screen_mode != 0) {
                os_gfx_set_mode(0x03);
                s_screen_mode = 0;
                if (s_key_visible) {
                    os_console_set_scroll_rows(24);
                    basic_display_softkeys();
                } else {
                    os_console_set_scroll_rows(25);
                }
            }
            os_puts("^C\n");
            buf[0] = '\0';
            return 0;
        } else if (c == '\r' || c == '\n') {
            os_putc('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b' || u == 0x7F) {
            if (len > 0) {
                len--;
                os_puts("\b \b");
            }
        } else if (u >= 32 && u < 127) {
            if (len + 1 < max_len) {
                buf[len++] = c;
                os_putc(c);
            }
        }
    }
}

static void basic_process_input(char *line) {
    uint16_t p = 0;
    uint8_t tok_buf[BASIC_MAX_LINE_LEN];

    while (line[p] == ' ' || line[p] == '\t') p++;
    if (line[p] == '\0') return;

    /* Check if line starts with a number (Program Line) */
    if (basic_is_digit(line[p])) {
        uint16_t line_num = basic_parse_uint(line, &p);

        while (line[p] == ' ' || line[p] == '\t') p++;

        if (line[p] == '\0') {
            basic_delete_line(line_num);
        } else {
            uint16_t tok_len = basic_tokenize(&line[p], tok_buf, sizeof(tok_buf));
            basic_insert_line(line_num, tok_buf, tok_len);
        }
        return;
    }

    /* Immediate mode command */
    {
        uint16_t tok_len = basic_tokenize(&line[p], tok_buf, sizeof(tok_buf));

        if (tok_buf[0] == TOK_NEW) {
            basic_new();
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_LIST) {
            uint16_t start = 0;
            uint16_t end = 65535;
            uint16_t lp = 1;

            while (tok_buf[lp] == ' ' || tok_buf[lp] == '\t') lp++;
            if (basic_is_digit((char)tok_buf[lp])) {
                start = basic_parse_uint((const char *)tok_buf, &lp);
                end = start;
                while (tok_buf[lp] == ' ' || tok_buf[lp] == '\t') lp++;
                if (tok_buf[lp] == TOK_MINUS || tok_buf[lp] == '-' || tok_buf[lp] == ',') {
                    lp++;
                    end = 65535;
                    while (tok_buf[lp] == ' ' || tok_buf[lp] == '\t') lp++;
                    if (basic_is_digit((char)tok_buf[lp])) {
                        end = basic_parse_uint((const char *)tok_buf, &lp);
                    }
                }
            } else if (tok_buf[lp] == TOK_MINUS || tok_buf[lp] == '-' || tok_buf[lp] == ',') {
                lp++;
                start = 0;
                end = 65535;
                while (tok_buf[lp] == ' ' || tok_buf[lp] == '\t') lp++;
                if (basic_is_digit((char)tok_buf[lp])) {
                    end = basic_parse_uint((const char *)tok_buf, &lp);
                }
            }

            basic_cmd_list(start, end);
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_RUN) {
            basic_cmd_run();
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_CONT) {
            basic_cmd_cont();
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_AUTO) {
            uint16_t ap = 0;
            basic_exec_auto((const uint8_t __far *)FP_HERE(tok_buf), &ap);
            if (!s_auto_mode) os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_RENUM) {
            uint16_t rp = 0;
            basic_exec_renum((const uint8_t __far *)FP_HERE(tok_buf), &rp);
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_DELETE) {
            uint16_t dp = 0;
            basic_exec_delete((const uint8_t __far *)FP_HERE(tok_buf), &dp);
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_TRON) {
            s_tron = 1;
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == TOK_TROFF) {
            s_tron = 0;
            os_puts(BASIC_PROMPT);
            return;
        }

        if (tok_buf[0] == (uint8_t)(TOK_SYSTEM >> 8) && tok_buf[1] == (uint8_t)(TOK_SYSTEM & 0xFF)) {
            os_set_break_mode(BREAK_MODE_NORMAL);
            os_console_set_scroll_rows(25);
            basic_clear_softkeys();
            basic_sound_tone(0, 0);
            if (s_screen_mode != 0) {
                os_gfx_set_mode(0x03);
                s_screen_mode = 0;
            }
            basic_close_all_files();
            if (s_basic_seg) {
                os_mem_free(s_basic_seg);
                s_basic_seg = 0;
            }
            s_running = 0;
            os_exit(0);
            return;
        }

        /* Execute immediate statement */
        s_running = 1;
        s_jumped = 0;
        s_cur_line_off = 0;
        basic_exec_line((const uint8_t __far *)FP_HERE(tok_buf));
        s_running = 0;
        os_puts(BASIC_PROMPT);
    }
}

/* --------------------------------------------------------------------------
 * Main Entry Point
 * -------------------------------------------------------------------------- */

int app_main(void) {
    static char line_buf[BASIC_MAX_LINE_LEN];

    os_set_break_mode(BREAK_MODE_RAW);

    s_basic_seg = os_mem_alloc(BASIC_SEG_PARAS);
    if (s_basic_seg == 0) {
        os_puts("BASIC: Out of conventional memory (64KB required)\n");
        return 1;
    }

    basic_new();
    basic_math_init();
    basic_sound_init_opl();
    basic_sound_tone(0, 0);

    os_cls(0x07);
    os_set_cursor(0, 0);

    os_puts("hobbyOS BASIC Version 1.0 (GW-BASIC Compatible)\n");
    {
        char nbuf[8];
        uint16_t fmem = basic_get_free_mem();
        int np = 0;
        if (fmem == 0) {
            os_putc('0');
        } else {
            while (fmem > 0) {
                nbuf[np++] = (char)('0' + (fmem % 10));
                fmem /= 10;
            }
            while (np > 0) os_putc(nbuf[--np]);
        }
        os_puts(" Bytes free\n");
    }

    if (s_key_visible) {
        os_console_set_scroll_rows(24);
        basic_display_softkeys();
    } else {
        os_console_set_scroll_rows(25);
    }

    {
        const char *args = os_get_args();
        int has_file_arg = 0;
        if (args) {
            while (*args == ' ' || *args == '\t') args++;
            if (*args != '\0') {
                char cmd[80];
                int cp = 0;
                has_file_arg = 1;
                cmd[cp++] = 'L'; cmd[cp++] = 'O'; cmd[cp++] = 'A'; cmd[cp++] = 'D'; cmd[cp++] = ' ';
                cmd[cp++] = '"';
                while (*args != '\0' && *args != ' ' && *args != '\t' && cp < 60) {
                    cmd[cp++] = *args++;
                }
                cmd[cp++] = '"';
                cmd[cp++] = ',';
                cmd[cp++] = 'R';
                cmd[cp] = '\0';
                basic_process_input(cmd);
            }
        }
        if (has_file_arg) {
            os_set_break_mode(BREAK_MODE_NORMAL);
            os_console_set_scroll_rows(25);
            basic_clear_softkeys();
            basic_sound_tone(0, 0);
            if (s_screen_mode != 0) {
                os_gfx_set_mode(0x03);
                s_screen_mode = 0;
            }
            basic_close_all_files();
            os_mem_free(s_basic_seg);
            os_exit(0);
            return 0;
        }
    }

    os_puts(BASIC_PROMPT);

    for (;;) {
        uint16_t len;
        if (s_auto_mode) {
            char pbuf[16];
            int pp = 0;
            uint16_t v = s_auto_line;
            char tmp[8];
            int tp = 0;
            if (basic_find_line(s_auto_line) != 0) {
                pbuf[pp++] = '*';
            }
            if (v == 0) pbuf[pp++] = '0';
            else {
                while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                while (tp > 0) pbuf[pp++] = tmp[--tp];
            }
            pbuf[pp++] = ' ';
            pbuf[pp] = '\0';
            os_puts(pbuf);
        }
        len = basic_read_line(line_buf, sizeof(line_buf));
        if (s_break_pending) {
            s_break_pending = 0;
            s_auto_mode = 0;
            os_puts(BASIC_PROMPT);
            continue;
        }
        if (len > 0) {
            if (s_auto_mode) {
                uint16_t sp = 0;
                while (line_buf[sp] == ' ' || line_buf[sp] == '\t') sp++;
                if (basic_is_digit(line_buf[sp])) {
                    basic_process_input(line_buf);
                } else {
                    char combined[BASIC_MAX_LINE_LEN];
                    char num_str[8];
                    int np = 0;
                    uint16_t v = s_auto_line;
                    char tmp[8];
                    int tp = 0;
                    if (v == 0) num_str[np++] = '0';
                    else {
                        while (v > 0) { tmp[tp++] = (char)('0' + (v % 10)); v /= 10; }
                        while (tp > 0) num_str[np++] = tmp[--tp];
                    }
                    num_str[np++] = ' ';
                    num_str[np] = '\0';
                    basic_strncpy(combined, num_str, sizeof(combined) - 1);
                    basic_strncpy(combined + np, line_buf, sizeof(combined) - 1 - np);
                    basic_process_input(combined);
                    s_auto_line += s_auto_step;
                }
            } else {
                basic_process_input(line_buf);
            }
        } else {
            if (s_auto_mode) {
                s_auto_mode = 0;
                os_puts(BASIC_PROMPT);
            } else {
                os_puts(BASIC_PROMPT);
            }
        }
    }

    os_set_break_mode(BREAK_MODE_NORMAL);
    os_console_set_scroll_rows(25);
    basic_clear_softkeys();
    basic_sound_tone(0, 0);
    if (s_screen_mode != 0) {
        os_gfx_set_mode(0x03);
        s_screen_mode = 0;
    }
    basic_close_all_files();
    os_mem_free(s_basic_seg);
    return 0;
}
