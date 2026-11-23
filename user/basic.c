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

#define FP_HERE(ptr)        ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))
#define BASIC_SEG_PARAS     4096    /* 64 KB program segment */
#define BASIC_MAX_LINE_LEN  256
#define BASIC_PROMPT        "Ok\n"

#define BASIC_MAX_VARS      96
#define BASIC_MAX_VARNAME   16
#define BASIC_MAX_STR_LEN   128
#define BASIC_NUM_TEMP_STRS 8

#define BASIC_MAX_GOSUB     32
#define BASIC_MAX_FOR_NEST  16
#define BASIC_MAX_WHILE_NEST 16
#define BASIC_MAX_ARRAYS    32
#define BASIC_STR_ELEM_LEN  32

#define BASIC_MAX_FILES     4
#define BASIC_FILE_BUF_SIZE 256

#define BASIC_TYPE_INT      1
#define BASIC_TYPE_STR      2

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
    uint8_t type;       /* BASIC_TYPE_INT or BASIC_TYPE_STR */
    int16_t ival;       /* Integer value */
    char    *sval;      /* Pointer to string (in DS) */
} basic_val_t;

/* Variable structure */
typedef struct {
    char    name[BASIC_MAX_VARNAME];
    uint8_t type;
    int16_t ival;
    char    sval[BASIC_MAX_STR_LEN + 1];
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

/* File handle structure */
typedef struct {
    int      is_open;
    uint8_t  mode;      /* 'I', 'O', 'A' */
    char     filename[32];
    uint32_t file_pos;
    uint32_t file_len;
    uint8_t  buf[BASIC_FILE_BUF_SIZE];
    uint16_t buf_pos;
    uint16_t buf_len;
    uint8_t  eof;
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

/* Graphics and Sound state (Phase 5) */
static uint8_t  s_screen_mode       = 0;    /* 0 = text 80x25, 1 = CGA 320x200, 2 = CGA 640x200 */
static uint8_t  s_text_fg           = 7;
static uint8_t  s_text_bg           = 0;
static uint8_t  s_text_attr         = 0x07;
static uint8_t  s_cga_bg            = 0;
static uint8_t  s_cga_palette       = 1;
static int16_t  s_last_x            = 0;
static int16_t  s_last_y            = 0;
static uint8_t  s_gfx_row           = 0;
static uint8_t  s_gfx_col           = 0;
static uint8_t  s_play_octave       = 4;
static uint8_t  s_play_length       = 4;
static uint8_t  s_play_tempo        = 120;
static uint8_t  s_play_style        = 0;    /* 0 = normal (7/8), 1 = legato (8/8), 2 = staccato (6/8) */

/* Forward declarations */
static int basic_eval_expr(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val);
static int basic_exec_statement(const uint8_t __far *tok, uint16_t *pos);
static int basic_skip_to_next(const uint8_t __far *tok, uint16_t *pos, const char *var_name);
static int basic_skip_to_wend(const uint8_t __far *tok, uint16_t *pos);
static void basic_close_all_files(void);
static void basic_cmd_run(void);
static int basic_insert_line(uint16_t line_num, const uint8_t *tok_data, uint16_t tok_len);
static uint16_t basic_tokenize(const char *in, uint8_t *out, uint16_t max_out);

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

static void basic_print_uint(uint16_t val) {
    char buf[8];
    int p = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0) {
        buf[p++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (p > 0) {
        os_putc(buf[--p]);
    }
}

static void basic_print_int(int16_t val) {
    if (val < 0) {
        os_putc('-');
        if (val == -32768) {
            os_puts("32768");
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
        v->ival = 0;
    } else {
        v->type = BASIC_TYPE_INT;
        v->ival = 0;
        v->sval[0] = '\0';
    }
    return v;
}

static int basic_parse_var_name(const uint8_t __far *tok, uint16_t *pos, char *out_name, uint16_t max_len) {
    uint16_t p = *pos;
    uint16_t np = 0;
    while (tok[p] == ' ' || tok[p] == '\t') p++;
    if (!basic_is_alpha((char)tok[p])) return 0;

    while (basic_is_alpha((char)tok[p]) || basic_is_digit((char)tok[p]) || tok[p] == '.') {
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
    uint8_t type = (len > 0 && name[len - 1] == '$') ? BASIC_TYPE_STR : BASIC_TYPE_INT;
    uint16_t elem_size = (type == BASIC_TYPE_STR) ? BASIC_STR_ELEM_LEN : 2;
    uint16_t bytes = total_elems * elem_size;
    basic_array_t *arr;
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t b;

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
        os_puts("Subscript out of range\n");
        return -1;
    }
    if (arr->num_dims == 2 && (i2 < 0 || (uint16_t)i2 >= arr->dim2)) {
        os_puts("Subscript out of range\n");
        return -1;
    }

    linear_idx = (arr->num_dims == 1) ? (uint16_t)i1 : ((uint16_t)i1 * arr->dim2 + (uint16_t)i2);

    if (arr->type == BASIC_TYPE_INT) {
        off = arr->data_off + linear_idx * 2;
        out_val->type = BASIC_TYPE_INT;
        out_val->ival = (int16_t)((uint16_t)mem[off] | ((uint16_t)mem[off + 1] << 8));
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
    }
    return 0;
}

static int basic_write_array_elem(basic_array_t *arr, int16_t i1, int16_t i2, const basic_val_t *val) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t linear_idx;
    uint16_t off;

    if (i1 < 0 || (uint16_t)i1 >= arr->dim1) {
        os_puts("Subscript out of range\n");
        return -1;
    }
    if (arr->num_dims == 2 && (i2 < 0 || (uint16_t)i2 >= arr->dim2)) {
        os_puts("Subscript out of range\n");
        return -1;
    }

    linear_idx = (arr->num_dims == 1) ? (uint16_t)i1 : ((uint16_t)i1 * arr->dim2 + (uint16_t)i2);

    if (arr->type == BASIC_TYPE_INT) {
        if (val->type != BASIC_TYPE_INT) {
            os_puts("Type mismatch\n");
            return -1;
        }
        off = arr->data_off + linear_idx * 2;
        mem[off] = (uint8_t)(val->ival & 0xFF);
        mem[off + 1] = (uint8_t)(val->ival >> 8);
    } else {
        uint16_t k;
        if (val->type != BASIC_TYPE_STR) {
            os_puts("Type mismatch\n");
            return -1;
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
                    char last_kw_char = kw[klen - 1];
                    char next_in_char = in[ip + klen];

                    if (basic_is_alpha(last_kw_char) || basic_is_digit(last_kw_char)) {
                        if (basic_is_alpha(next_in_char) || basic_is_digit(next_in_char) || next_in_char == '.') {
                            continue;
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

static uint16_t basic_detokenize_line(uint16_t line_num, const uint8_t __far *tok_data, char *out_buf, uint16_t max_out) {
    uint16_t p = 0;
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

static void basic_list_line(uint16_t line_num, const uint8_t __far *tok_data) {
    char buf[BASIC_MAX_LINE_LEN];
    basic_detokenize_line(line_num, tok_data, buf, sizeof(buf));
    os_puts(buf);
    os_putc('\n');
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
    if (s_basic_seg) {
        uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
        mem[0] = 0; mem[1] = 0; mem[2] = 0; mem[3] = 0;
    }
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

static void basic_delete_line(uint16_t line_num) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);
    uint16_t prev = 0;
    uint16_t cur = s_prog_head;

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

    /* Decimal number literal */
    if (basic_is_digit((char)tok[p])) {
        int16_t val = 0;
        while (basic_is_digit((char)tok[p])) {
            val = (val * 10) + (int16_t)(tok[p++] - '0');
        }
        out_val->type = BASIC_TYPE_INT;
        out_val->ival = val;
        out_val->sval = 0;
        *pos = p;
        return 0;
    }

    /* Built-in function: INKEY$ (token 0xDE) */
    if (tok[p] == TOK_INKEY) {
        char *str = basic_alloc_temp_str();
        p++;
        if (os_kbhit()) {
            uint16_t k = os_getc();
            str[0] = (char)(k & 0xFF);
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
        char *str = basic_alloc_temp_str();
        int idx = 0;

        p += 2; /* skip TOK_INPUT and '$' */
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] != '(') {
            os_puts("Syntax error in INPUT$ (expected '(')\n");
            return -1;
        }
        p++;

        if (basic_eval_expr(tok, &p, &n_val) != 0) return -1;
        n = n_val.ival;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            basic_val_t f_val;
            p++;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == '#') p++;
            if (basic_eval_expr(tok, &p, &f_val) != 0) return -1;
            fnum = (uint8_t)f_val.ival;
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ')') p++;

        if (n < 0 || n > BASIC_MAX_STR_LEN) {
            os_puts("Illegal function call in INPUT$\n");
            return -1;
        }

        if (fnum == 0) {
            while (idx < n) {
                uint16_t key = os_getc();
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
        out_val->ival = 0;
        *pos = p;
        return 0;
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
                basic_eval_expr(tok, &p, &dummy);
                while (tok[p] == ' ' || tok[p] == '\t') p++;
                if (tok[p] == ')') p++;
            }
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = basic_rand();
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

        /* EOF(filenum) */
        if (fn == (uint8_t)(TOK_EOF & 0xFF)) {
            basic_val_t arg;
            uint8_t fnum;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            fnum = (uint8_t)arg.ival;
            out_val->type = BASIC_TYPE_INT;
            if (fnum >= 1 && fnum <= BASIC_MAX_FILES && s_files[fnum - 1].is_open) {
                basic_file_t *f = &s_files[fnum - 1];
                int is_eof = f->eof || (f->buf_pos >= f->buf_len && f->file_pos >= f->file_len);
                out_val->ival = is_eof ? -1 : 0;
            } else {
                out_val->ival = -1;
            }
            out_val->sval = 0;
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
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = basic_atoi(arg.sval);
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
            out_val->type = BASIC_TYPE_STR;
            out_val->sval = str;
            out_val->ival = 0;
            *pos = p;
            return 0;
        }

        /* ABS(X) */
        if (fn == (uint8_t)(TOK_ABS & 0xFF)) {
            basic_val_t arg;
            if (basic_eval_expr(tok, &p, &arg) != 0) return -1;
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == ')') p++;
            out_val->type = BASIC_TYPE_INT;
            out_val->ival = (arg.ival < 0) ? -arg.ival : arg.ival;
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
            out_val->ival = (arg.ival > 0) ? 1 : ((arg.ival < 0) ? -1 : 0);
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
                out_val->ival = var->ival;
                out_val->sval = var->sval;
            } else {
                uint16_t len = basic_strlen(var_name);
                if (len > 0 && var_name[len - 1] == '$') {
                    char *str = basic_alloc_temp_str();
                    out_val->type = BASIC_TYPE_STR;
                    out_val->sval = str;
                    out_val->ival = 0;
                } else {
                    out_val->type = BASIC_TYPE_INT;
                    out_val->ival = 0;
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
        out_val->ival = -out_val->ival;
        return 0;
    }
    if (tok[p] == TOK_PLUS || tok[p] == '+') {
        p++;
        *pos = p;
        return eval_unary(tok, pos, out_val);
    }
    return eval_primary(tok, pos, out_val);
}

static int eval_mul(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_unary(tok, pos, out_val) != 0) return -1;

    for (;;) {
        uint16_t p = *pos;
        uint8_t op;
        basic_val_t right;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        op = tok[p];

        if (op != TOK_TIMES && op != TOK_DIV && op != TOK_INTDIV && op != TOK_MOD &&
            op != '*' && op != '/' && op != '\\') {
            break;
        }

        p++;
        *pos = p;
        if (eval_unary(tok, pos, &right) != 0) return -1;

        if (op == TOK_TIMES || op == '*') {
            out_val->ival = out_val->ival * right.ival;
        } else if (op == TOK_DIV || op == '/' || op == TOK_INTDIV || op == '\\') {
            if (right.ival == 0) {
                os_puts("Division by zero\n");
                return -1;
            }
            out_val->ival = out_val->ival / right.ival;
        } else if (op == TOK_MOD) {
            if (right.ival == 0) {
                os_puts("Division by zero\n");
                return -1;
            }
            out_val->ival = out_val->ival % right.ival;
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
            } else {
                out_val->ival = out_val->ival + right.ival;
            }
        } else {
            out_val->ival = out_val->ival - right.ival;
        }
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
        out_val->ival = ~out_val->ival;
        return 0;
    }
    return eval_add(tok, pos, out_val);
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
        out_val->ival = out_val->ival & right.ival;
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
            out_val->ival = out_val->ival | right.ival;
        } else {
            out_val->ival = out_val->ival ^ right.ival;
        }
    }
    return 0;
}

static int basic_eval_expr(const uint8_t __far *tok, uint16_t *pos, basic_val_t *out_val) {
    if (eval_or(tok, pos, out_val) != 0) return -1;

    for (;;) {
        int cmp_op = basic_match_cmp_op(tok, pos);
        basic_val_t right;
        int cond = 0;

        if (cmp_op == OP_CMP_NONE) break;

        if (eval_or(tok, pos, &right) != 0) return -1;

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
        out_val->sval = 0;
    }
    return 0;
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
        /* Syntax 1: OPEN "mode", #n, "filename" */
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
    } else if (tok[p] == TOK_FOR) {
        /* Syntax 2: OPEN "filename" FOR INPUT/OUTPUT/APPEND AS #n */
        basic_val_t fnum_val;
        basic_strncpy(fname, arg1.sval, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = '\0';

        p++; /* skip TOK_FOR */
        while (tok[p] == ' ' || tok[p] == '\t') p++;

        if (basic_strnicmp((const char *)&tok[p], "OUTPUT", 6) == 0) {
            mode = 'O';
            p += 6;
        } else if (tok[p] == TOK_INPUT || basic_strnicmp((const char *)&tok[p], "INPUT", 5) == 0) {
            mode = 'I';
            p = (tok[p] == TOK_INPUT) ? (p + 1) : (p + 5);
        } else if (basic_strnicmp((const char *)&tok[p], "APPEND", 6) == 0) {
            mode = 'A';
            p += 6;
        } else {
            os_puts("Bad file mode in OPEN\n");
            return -1;
        }

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (basic_strnicmp((const char *)&tok[p], "AS", 2) == 0) {
            p += 2;
        }
        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == '#') p++;

        if (basic_eval_expr(tok, &p, &fnum_val) != 0) return -1;
        fnum = (uint8_t)fnum_val.ival;
    } else {
        os_puts("Syntax error in OPEN\n");
        return -1;
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
                    os_puts("Undefined line number ");
                    basic_print_uint(target_line);
                    os_putc('\n');
                    return -1;
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
            if (val.type != BASIC_TYPE_STR) {
                os_puts("Type mismatch\n");
                return -1;
            }
            basic_strncpy(var->sval, val.sval ? val.sval : "", sizeof(var->sval) - 1);
            var->sval[sizeof(var->sval) - 1] = '\0';
        } else {
            if (val.type != BASIC_TYPE_INT) {
                os_puts("Type mismatch\n");
                return -1;
            }
            var->ival = val.ival;
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
        dim1 = (uint16_t)(d1_val.ival + 1);
        num_dims = 1;
        dim2 = 1;

        while (tok[p] == ' ' || tok[p] == '\t') p++;
        if (tok[p] == ',') {
            p++;
            if (basic_eval_expr(tok, &p, &d2_val) != 0) return -1;
            dim2 = (uint16_t)(d2_val.ival + 1);
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
        type = (len > 0 && var_name[len - 1] == '$') ? BASIC_TYPE_STR : BASIC_TYPE_INT;

        total_elems = (num_dims == 1) ? dim1 : (dim1 * dim2);
        elem_size = (type == BASIC_TYPE_STR) ? BASIC_STR_ELEM_LEN : 2;
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
        step = step_val.ival;
    }

    var = basic_get_or_create_var(var_name);
    if (!var) return -1;
    var->ival = start_val.ival;

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
    s_for_stack[slot].end_val  = end_val.ival;
    s_for_stack[slot].step_val = step;
    s_for_stack[slot].line_off = s_cur_line_off;
    s_for_stack[slot].stmt_pos = p;

    if ((step > 0 && var->ival > end_val.ival) || (step < 0 && var->ival < end_val.ival)) {
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
            os_puts("NEXT without FOR\n");
            return -1;
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
                os_puts("NEXT without FOR\n");
                return -1;
            }
        } else {
            slot = (int)s_for_sp - 1;
        }

        var = basic_find_var(s_for_stack[slot].var_name);
        if (!var) return -1;

        var->ival += s_for_stack[slot].step_val;

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
        os_puts("WEND without WHILE\n");
        return -1;
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
            os_puts("Out of DATA\n");
            return -1;
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
    if (row > 24) row = 24;
    if (col > 79) col = 79;
    s_gfx_row = row;
    s_gfx_col = col;
    if (s_screen_mode == 0) {
        os_set_cursor(row, col);
    }

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

    if (v1->type == BASIC_TYPE_INT) {
        int16_t tmp = v1->ival;
        v1->ival = v2->ival;
        v2->ival = tmp;
    } else {
        char tmp_str[BASIC_MAX_STR_LEN + 1];
        basic_strcpy(tmp_str, v1->sval);
        basic_strcpy(v1->sval, v2->sval);
        basic_strcpy(v2->sval, tmp_str);
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
        os_puts("Undefined line number ");
        basic_print_uint(target_line);
        os_putc('\n');
        return -1;
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
        os_puts("Undefined line number ");
        basic_print_uint(target_line);
        os_putc('\n');
        return -1;
    }

    if (s_gosub_sp >= BASIC_MAX_GOSUB) {
        os_puts("Out of memory (GOSUB overflow)\n");
        return -1;
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
        os_puts("RETURN without GOSUB\n");
        return -1;
    }

    s_gosub_sp--;
    s_pc_line = s_gosub_stack[s_gosub_sp].line_off;
    s_pc_stmt = s_gosub_stack[s_gosub_sp].stmt_pos;
    s_jumped = 1;

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
            return basic_exec_goto(tok, &p);
        } else {
            return basic_skip_to_else(tok, &p);
        }
    } else if (tok[p] == TOK_THEN) {
        p++;
        while (tok[p] == ' ' || tok[p] == '\t') p++;

        if (cond.ival != 0) {
            if (basic_is_digit((char)tok[p])) {
                uint16_t target_line = basic_parse_uint_far(tok, &p);
                uint16_t target_off = basic_find_line(target_line);
                if (target_off == 0) {
                    os_puts("Undefined line number ");
                    basic_print_uint(target_line);
                    os_putc('\n');
                    return -1;
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
            return basic_skip_to_else(tok, &p);
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

static void basic_gfx_print_str(const char *str, uint8_t color) {
    uint16_t max_cols = (s_screen_mode == 2) ? 80 : 40;
    while (*str) {
        char c = *str++;
        if (c == '\r') {
            s_gfx_col = 0;
            continue;
        }
        if (c == '\n') {
            s_gfx_col = 0;
            s_gfx_row++;
            if (s_gfx_row >= 25) s_gfx_row = 0;
            continue;
        }
        if (s_gfx_col >= max_cols) {
            s_gfx_col = 0;
            s_gfx_row++;
            if (s_gfx_row >= 25) s_gfx_row = 0;
        }
        {
            char single[2];
            single[0] = c;
            single[1] = '\0';
            os_gfx_text((uint16_t)s_gfx_col * 8, (uint16_t)s_gfx_row * 8, (const char __far *)FP_HERE(single), color);
        }
        s_gfx_col++;
    }
}

static void basic_term_putc(char c) {
    if (s_screen_mode == 0) {
        os_putc(c);
    } else {
        char s[2];
        s[0] = c;
        s[1] = '\0';
        basic_gfx_print_str(s, (s_screen_mode == 1) ? 3 : 1);
    }
}

static void basic_term_puts(const char *s) {
    if (s_screen_mode == 0) {
        os_puts(s);
    } else {
        basic_gfx_print_str(s, (s_screen_mode == 1) ? 3 : 1);
    }
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
        } else if (m == 1) {
            os_gfx_set_mode(0x04);
            s_screen_mode = 1;
            s_last_x = 160;
            s_last_y = 100;
            s_gfx_row = 0;
            s_gfx_col = 0;
        } else if (m == 2) {
            os_gfx_set_mode(0x06);
            s_screen_mode = 2;
            s_last_x = 320;
            s_last_y = 100;
            s_gfx_row = 0;
            s_gfx_col = 0;
        } else {
            os_puts("Illegal function call in SCREEN\n");
            return -1;
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
        /* Screen mode 2: foreground color */
        if (tok[p] != 0x00 && tok[p] != ':' && tok[p] != ',') {
            basic_val_t fg_val;
            if (basic_eval_expr(tok, &p, &fg_val) != 0) return -1;
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
        color = (s_screen_mode == 1) ? 3 : 1;
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
        uint16_t max_x = (s_screen_mode == 2) ? 640 : 320;
        if ((uint16_t)x < max_x && (uint16_t)y < 200) {
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

    color = (s_screen_mode == 1) ? 3 : 1;
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
    uint16_t max_x = (s_screen_mode == 2) ? 640 : 320;

    while (x <= y) {
        if (xc + x >= 0 && (uint16_t)(xc + x) < max_x && yc + y >= 0 && (uint16_t)(yc + y) < 200)
            os_gfx_putpixel((uint16_t)(xc + x), (uint16_t)(yc + y), color);
        if (xc - x >= 0 && (uint16_t)(xc - x) < max_x && yc + y >= 0 && (uint16_t)(yc + y) < 200)
            os_gfx_putpixel((uint16_t)(xc - x), (uint16_t)(yc + y), color);
        if (xc + x >= 0 && (uint16_t)(xc + x) < max_x && yc - y >= 0 && (uint16_t)(yc - y) < 200)
            os_gfx_putpixel((uint16_t)(xc + x), (uint16_t)(yc - y), color);
        if (xc - x >= 0 && (uint16_t)(xc - x) < max_x && yc - y >= 0 && (uint16_t)(yc - y) < 200)
            os_gfx_putpixel((uint16_t)(xc - x), (uint16_t)(yc - y), color);

        if (xc + y >= 0 && (uint16_t)(xc + y) < max_x && yc + x >= 0 && (uint16_t)(yc + x) < 200)
            os_gfx_putpixel((uint16_t)(xc + y), (uint16_t)(yc + x), color);
        if (xc - y >= 0 && (uint16_t)(xc - y) < max_x && yc + x >= 0 && (uint16_t)(yc + x) < 200)
            os_gfx_putpixel((uint16_t)(xc - y), (uint16_t)(yc + x), color);
        if (xc + y >= 0 && (uint16_t)(xc + y) < max_x && yc - x >= 0 && (uint16_t)(yc - x) < 200)
            os_gfx_putpixel((uint16_t)(xc + y), (uint16_t)(yc - x), color);
        if (xc - y >= 0 && (uint16_t)(xc - y) < max_x && yc - x >= 0 && (uint16_t)(yc - x) < 200)
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
    color = (s_screen_mode == 1) ? 3 : 1;

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
                } else {
                    if (basic_eval_expr(tok, &p, &val) != 0) return -1;
                    if (val.type == BASIC_TYPE_STR) {
                        if (val.sval) basic_file_write_data(fnum, val.sval, basic_strlen(val.sval));
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
        while (tok[p] != 0x00 && tok[p] != ':') {
            while (tok[p] == ' ' || tok[p] == '\t') p++;
            if (tok[p] == 0x00 || tok[p] == ':' || tok[p] == TOK_ELSE) break;

            if (tok[p] == ';') {
                newline = 0;
                p++;
            } else if (tok[p] == ',') {
                uint8_t col = (s_screen_mode == 0) ? (uint8_t)(os_get_cursor() & 0xFF) : s_gfx_col;
                uint8_t max_cols = (s_screen_mode == 1) ? 40 : 80;
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
            } else {
                basic_val_t val;
                if (basic_eval_expr(tok, &p, &val) != 0) {
                    return -1;
                }
                if (val.type == BASIC_TYPE_STR) {
                    basic_term_puts(val.sval ? val.sval : "");
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

    /* SYSTEM statement in program (2-byte token 0xFE, 0x83) */
    if (tok[p] == (uint8_t)(TOK_SYSTEM >> 8) && tok[p + 1] == (uint8_t)(TOK_SYSTEM & 0xFF)) {
        basic_sound_tone(0, 0);
        if (s_screen_mode != 0) {
            os_gfx_set_mode(0x03);
            s_screen_mode = 0;
        }
        basic_close_all_files();
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

    /* END / STOP statement */
    if (tok[p] == TOK_END || tok[p] == TOK_STOP) {
        s_running = 0;
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

static void basic_cmd_run(void) {
    uint8_t __far *mem = (uint8_t __far *)MK_FP(s_basic_seg, 0);

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
    s_running = 1;
    s_pc_line = s_prog_head;
    s_pc_stmt = 4;

    while (s_pc_line != 0 && s_running) {
        uint16_t next_line = (uint16_t)mem[s_pc_line] | ((uint16_t)mem[s_pc_line + 1] << 8);
        uint16_t num       = (uint16_t)mem[s_pc_line + 2] | ((uint16_t)mem[s_pc_line + 3] << 8);

        s_cur_line_off = s_pc_line;
        s_cur_line_num = num;

        if (os_kbhit() && (os_getc() & 0xFF) == 0x03) {
            basic_sound_tone(0, 0);
            if (s_screen_mode != 0) {
                os_gfx_set_mode(0x03);
                s_screen_mode = 0;
            }
            os_puts("\nBreak in ");
            basic_print_uint(num);
            os_putc('\n');
            s_running = 0;
            break;
        }

        s_jumped = 0;

        while (mem[s_cur_line_off + s_pc_stmt] != 0x00 && s_running && !s_jumped) {
            uint16_t pos = s_pc_stmt;
            if (basic_exec_statement(&mem[s_cur_line_off], &pos) != 0) {
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
}

/* --------------------------------------------------------------------------
 * Interactive REPL & Console Input
 * -------------------------------------------------------------------------- */

static uint16_t basic_read_line(char *buf, uint16_t max_len) {
    uint16_t len = 0;
    for (;;) {
        uint16_t key = os_getc();
        uint8_t  u   = (uint8_t)(key & 0xFF);
        char     c   = (char)u;

        if (c == 0x03) {
            basic_sound_tone(0, 0);
            if (s_screen_mode != 0) {
                os_gfx_set_mode(0x03);
                s_screen_mode = 0;
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

        if (tok_buf[0] == (uint8_t)(TOK_SYSTEM >> 8) && tok_buf[1] == (uint8_t)(TOK_SYSTEM & 0xFF)) {
            basic_sound_tone(0, 0);
            if (s_screen_mode != 0) {
                os_gfx_set_mode(0x03);
                s_screen_mode = 0;
            }
            basic_close_all_files();
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

    s_basic_seg = os_mem_alloc(BASIC_SEG_PARAS);
    if (s_basic_seg == 0) {
        os_puts("BASIC: Out of conventional memory (64KB required)\n");
        return 1;
    }

    basic_new();
    basic_sound_init_opl();
    basic_sound_tone(0, 0);

    os_puts("hobbyOS BASIC Version 1.0 (GW-BASIC Compatible)\n");
    os_puts("61440 Bytes free\n");
    os_puts(BASIC_PROMPT);

    for (;;) {
        uint16_t len = basic_read_line(line_buf, sizeof(line_buf));
        if (len > 0) {
            basic_process_input(line_buf);
        } else {
            os_puts(BASIC_PROMPT);
        }
    }

    basic_sound_tone(0, 0);
    if (s_screen_mode != 0) {
        os_gfx_set_mode(0x03);
        s_screen_mode = 0;
    }
    basic_close_all_files();
    os_mem_free(s_basic_seg);
    return 0;
}
