/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HobbyOS Z-Machine Version 3 Interpreter (BIN/ZCODE.BIN)
 * Plays classic Infocom text adventures (Zork I, II, III, Planetfall, etc.)
 * with authentic status line, word-wrapping, and MDA/CGA/VGA display support.
 */

#include "hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

/* Z-Machine Header Field Offsets */
#define ZH_VERSION        0x00
#define ZH_FLAGS1         0x01
#define ZH_RELEASE        0x02
#define ZH_HIMEM          0x04
#define ZH_PC             0x06
#define ZH_DICT           0x08
#define ZH_OBJECTS        0x0A
#define ZH_GLOBALS        0x0C
#define ZH_STATICMEM      0x0E
#define ZH_FLAGS2         0x10
#define ZH_SERIAL         0x12
#define ZH_ABBREV         0x18
#define ZH_FILE_LEN       0x1A
#define ZH_CHECKSUM       0x1C

/* Execution and Stack Limits */
#define Z_STACK_SIZE      1024
#define Z_MAX_CALL_DEPTH  32

/* Call Frame Structure */
typedef struct {
    uint32_t return_pc;
    uint16_t stack_ptr;
    uint16_t num_locals;
    uint16_t locals[15];
    uint8_t  store_var;
} z_call_frame_t;

/* Global Interpreter State */
static uint16_t s_story_seg = 0;
static uint32_t s_story_len = 0;
static uint16_t s_dynamic_size = 0;
static uint32_t s_pc = 0;

static uint16_t s_stack[Z_STACK_SIZE];
static uint16_t s_sp = 0;

static z_call_frame_t s_call_stack[Z_MAX_CALL_DEPTH];
static uint16_t s_frame_ptr = 0;

static uint16_t s_random_seed = 12345;
static int      s_running = 1;

/* Terminal Window & Scrolling State */
static int      s_cur_col = 0;
static int      s_cur_row = 1;
static int      s_rows_printed = 0;
static char     s_word_buf[82];
static int      s_word_len = 0;
static uint16_t s_vram_seg = 0xB800;

/* Alphabets for Text Decoding */
static const char s_alpha0[26] = "abcdefghijklmnopqrstuvwxyz";
static const char s_alpha1[26] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char s_alpha2[26] = " \n0123456789.,!?_#'\"/\\-:()";

/* --------------------------------------------------------------------------
 * Memory Access Routines
 * -------------------------------------------------------------------------- */

static uint8_t z_read_byte(uint32_t addr) {
    uint16_t seg = s_story_seg + (uint16_t)(addr >> 4);
    uint16_t off = (uint16_t)(addr & 0x0F);
    return *((const uint8_t __far *)MK_FP(seg, off));
}

static uint16_t z_read_word(uint32_t addr) {
    return ((uint16_t)z_read_byte(addr) << 8) | (uint16_t)z_read_byte(addr + 1);
}

static void z_write_byte(uint16_t addr, uint8_t val) {
    if (addr < s_dynamic_size) {
        *((uint8_t __far *)MK_FP(s_story_seg, addr)) = val;
    }
}

static void z_write_word(uint16_t addr, uint16_t val) {
    z_write_byte(addr, (uint8_t)(val >> 8));
    z_write_byte(addr + 1, (uint8_t)(val & 0xFF));
}

static uint8_t z_fetch_byte(void) {
    return z_read_byte(s_pc++);
}

static uint16_t z_fetch_word(void) {
    uint16_t w = z_read_word(s_pc);
    s_pc += 2;
    return w;
}

/* --------------------------------------------------------------------------
 * Variable Access
 * -------------------------------------------------------------------------- */

static uint16_t z_get_var(uint8_t var) {
    if (var == 0) {
        if (s_sp > 0) {
            return s_stack[--s_sp];
        }
        return 0;
    } else if (var >= 1 && var <= 15) {
        if (s_frame_ptr > 0 && (var - 1) < s_call_stack[s_frame_ptr - 1].num_locals) {
            return s_call_stack[s_frame_ptr - 1].locals[var - 1];
        }
        return 0;
    } else {
        uint16_t glob_base = z_read_word(ZH_GLOBALS);
        return z_read_word((uint32_t)glob_base + ((uint32_t)(var - 16) << 1));
    }
}

static void z_set_var(uint8_t var, uint16_t val) {
    if (var == 0) {
        if (s_sp < Z_STACK_SIZE) {
            s_stack[s_sp++] = val;
        }
    } else if (var >= 1 && var <= 15) {
        if (s_frame_ptr > 0 && (var - 1) < s_call_stack[s_frame_ptr - 1].num_locals) {
            s_call_stack[s_frame_ptr - 1].locals[var - 1] = val;
        }
    } else {
        uint16_t glob_base = z_read_word(ZH_GLOBALS);
        z_write_word((uint16_t)(glob_base + ((var - 16) << 1)), val);
    }
}

/* --------------------------------------------------------------------------
 * Screen & Status Line Output
 * -------------------------------------------------------------------------- */

static void z_flush_word(void);
static void z_update_status_line(void);

static void z_page_pause(void) {
    os_puts("[MORE]");
    while (1) {
        if (os_kbhit()) {
            uint16_t k = os_getc();
            (void)k;
            break;
        }
    }
    os_puts("\b\b\b\b\b\b      \b\b\b\b\b\b");
    s_rows_printed = 0;
}

static void z_putc(char c) {
    if (c == '\n') {
        z_flush_word();
        os_putc('\n');
        s_cur_col = 0;
        s_cur_row++;
        s_rows_printed++;
        if (s_rows_printed >= 23) {
            z_page_pause();
        }
        return;
    }

    if (c == ' ') {
        z_flush_word();
        if (s_cur_col < 79) {
            os_putc(' ');
            s_cur_col++;
        } else {
            os_putc('\n');
            s_cur_col = 0;
            s_cur_row++;
            s_rows_printed++;
            if (s_rows_printed >= 23) {
                z_page_pause();
            }
        }
        return;
    }

    /* Accumulate word buffer for clean word-wrapping */
    if (s_word_len < 80) {
        s_word_buf[s_word_len++] = c;
    }
}

static void z_flush_word(void) {
    int i;
    if (s_word_len == 0) return;

    if (s_cur_col + s_word_len > 79) {
        os_putc('\n');
        s_cur_col = 0;
        s_cur_row++;
        s_rows_printed++;
        if (s_rows_printed >= 23) {
            z_page_pause();
        }
    }

    for (i = 0; i < s_word_len; i++) {
        os_putc(s_word_buf[i]);
        s_cur_col++;
    }
    s_word_len = 0;
}

static void z_puts(const char *str) {
    while (str && *str) {
        z_putc(*str++);
    }
}

static void z_print_num(int16_t val) {
    char buf[8];
    int i = 0;
    uint16_t uval;

    if (val < 0) {
        z_putc('-');
        uval = (uint16_t)(-val);
    } else {
        uval = (uint16_t)val;
    }

    if (uval == 0) {
        z_putc('0');
        return;
    }
    while (uval > 0 && i < 7) {
        buf[i++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    while (i > 0) {
        z_putc(buf[--i]);
    }
}

/* --------------------------------------------------------------------------
 * Z-String Text Decoding
 * -------------------------------------------------------------------------- */

static uint32_t z_decode_text(uint32_t addr, char *out_buf, int max_out, int print_to_screen) {
    int alphabet = 0;
    int shift = 0;
    int abbrev_mode = 0;
    int multi_mode = 0;
    uint16_t multi_char = 0;
    int out_idx = 0;

    while (1) {
        uint16_t word = z_read_word(addr);
        int is_last = (word & 0x8000) != 0;
        int i;
        addr += 2;

        for (i = 0; i < 3; i++) {
            uint8_t zc;
            if (i == 0)      zc = (uint8_t)((word >> 10) & 0x1F);
            else if (i == 1) zc = (uint8_t)((word >> 5) & 0x1F);
            else             zc = (uint8_t)(word & 0x1F);

            if (abbrev_mode) {
                uint16_t abbr_base = z_read_word(ZH_ABBREV);
                uint16_t abbr_num = (uint16_t)(32 * (abbrev_mode - 1) + zc);
                uint16_t paddr = z_read_word((uint32_t)abbr_base + ((uint32_t)abbr_num << 1));
                uint32_t byte_addr = (uint32_t)paddr << 1;
                char abbr_str[64];
                int s;
                z_decode_text(byte_addr, abbr_str, sizeof(abbr_str) - 1, 0);
                for (s = 0; abbr_str[s]; s++) {
                    if (print_to_screen) z_putc(abbr_str[s]);
                    if (out_buf && out_idx + 1 < max_out) out_buf[out_idx++] = abbr_str[s];
                }
                abbrev_mode = 0;
                continue;
            }

            if (multi_mode == 1) {
                multi_char = (uint16_t)(zc << 5);
                multi_mode = 2;
                continue;
            } else if (multi_mode == 2) {
                char ch = (char)(multi_char | zc);
                if (print_to_screen) z_putc(ch);
                if (out_buf && out_idx + 1 < max_out) out_buf[out_idx++] = ch;
                multi_mode = 0;
                continue;
            }

            if (zc == 0) {
                if (print_to_screen) z_putc(' ');
                if (out_buf && out_idx + 1 < max_out) out_buf[out_idx++] = ' ';
            } else if (zc >= 1 && zc <= 3) {
                abbrev_mode = zc;
            } else if (zc == 4) {
                shift = 1;
            } else if (zc == 5) {
                shift = 2;
            } else {
                int cur_alpha = (shift != 0) ? shift : alphabet;
                shift = 0;
                if (cur_alpha == 2 && zc == 6) {
                    multi_mode = 1;
                } else {
                    char ch;
                    if (cur_alpha == 0)      ch = s_alpha0[zc - 6];
                    else if (cur_alpha == 1) ch = s_alpha1[zc - 6];
                    else                     ch = s_alpha2[zc - 6];

                    if (print_to_screen) z_putc(ch);
                    if (out_buf && out_idx + 1 < max_out) out_buf[out_idx++] = ch;
                }
            }
        }

        if (is_last) break;
    }

    if (out_buf) out_buf[out_idx] = '\0';
    return addr;
}

/* --------------------------------------------------------------------------
 * Object Table Operations
 * -------------------------------------------------------------------------- */

static uint32_t z_obj_addr(uint8_t obj) {
    uint16_t obj_base = z_read_word(ZH_OBJECTS);
    return (uint32_t)obj_base + 62 + (uint16_t)((uint16_t)(obj - 1) * 9);
}

static uint8_t z_obj_parent(uint8_t obj) {
    if (obj == 0) return 0;
    return z_read_byte(z_obj_addr(obj) + 4);
}

static uint8_t z_obj_sibling(uint8_t obj) {
    if (obj == 0) return 0;
    return z_read_byte(z_obj_addr(obj) + 5);
}

static uint8_t z_obj_child(uint8_t obj) {
    if (obj == 0) return 0;
    return z_read_byte(z_obj_addr(obj) + 6);
}

static uint16_t z_obj_prop_table(uint8_t obj) {
    if (obj == 0) return 0;
    return z_read_word(z_obj_addr(obj) + 7);
}

static void z_obj_set_parent(uint8_t obj, uint8_t val) {
    if (obj == 0) return;
    z_write_byte((uint16_t)(z_obj_addr(obj) + 4), val);
}

static void z_obj_set_sibling(uint8_t obj, uint8_t val) {
    if (obj == 0) return;
    z_write_byte((uint16_t)(z_obj_addr(obj) + 5), val);
}

static void z_obj_set_child(uint8_t obj, uint8_t val) {
    if (obj == 0) return;
    z_write_byte((uint16_t)(z_obj_addr(obj) + 6), val);
}

static int z_obj_test_attr(uint8_t obj, uint8_t attr) {
    uint32_t a = z_obj_addr(obj) + (attr >> 3);
    uint8_t bit = (uint8_t)(0x80 >> (attr & 7));
    return (z_read_byte(a) & bit) != 0;
}

static void z_obj_set_attr(uint8_t obj, uint8_t attr) {
    uint16_t a = (uint16_t)(z_obj_addr(obj) + (attr >> 3));
    uint8_t bit = (uint8_t)(0x80 >> (attr & 7));
    z_write_byte(a, (uint8_t)(z_read_byte(a) | bit));
}

static void z_obj_clear_attr(uint8_t obj, uint8_t attr) {
    uint16_t a = (uint16_t)(z_obj_addr(obj) + (attr >> 3));
    uint8_t bit = (uint8_t)(0x80 >> (attr & 7));
    z_write_byte(a, (uint8_t)(z_read_byte(a) & ~bit));
}

static void z_obj_remove(uint8_t obj) {
    uint8_t parent = z_obj_parent(obj);
    uint8_t child;
    uint8_t prev;

    if (parent == 0) return;

    child = z_obj_child(parent);
    if (child == obj) {
        z_obj_set_child(parent, z_obj_sibling(obj));
    } else {
        prev = child;
        while (prev != 0 && z_obj_sibling(prev) != obj) {
            prev = z_obj_sibling(prev);
        }
        if (prev != 0) {
            z_obj_set_sibling(prev, z_obj_sibling(obj));
        }
    }
    z_obj_set_parent(obj, 0);
    z_obj_set_sibling(obj, 0);
}

static void z_obj_insert(uint8_t obj, uint8_t dest) {
    z_obj_remove(obj);
    z_obj_set_sibling(obj, z_obj_child(dest));
    z_obj_set_child(dest, obj);
    z_obj_set_parent(obj, dest);
}

static uint32_t z_obj_get_prop_addr(uint8_t obj, uint8_t prop) {
    uint32_t pt;
    uint8_t name_len;

    if (obj == 0) return 0;
    pt = z_obj_prop_table(obj);
    name_len = z_read_byte(pt);
    pt += 1 + ((uint32_t)name_len << 1);

    while (1) {
        uint8_t pb = z_read_byte(pt);
        uint8_t num = (uint8_t)(pb & 0x1F);
        uint8_t len = (uint8_t)((pb >> 5) + 1);
        if (pb == 0) break;
        if (num == prop) {
            return pt + 1;
        }
        if (num < prop) break;
        pt += 1 + len;
    }
    return 0;
}

static uint16_t z_obj_get_prop(uint8_t obj, uint8_t prop) {
    uint32_t pa = z_obj_get_prop_addr(obj, prop);
    if (pa != 0) {
        uint8_t pb = z_read_byte(pa - 1);
        uint8_t len = (uint8_t)((pb >> 5) + 1);
        if (len == 1) return z_read_byte(pa);
        return z_read_word(pa);
    } else {
        uint16_t obj_base = z_read_word(ZH_OBJECTS);
        return z_read_word((uint32_t)obj_base + ((uint32_t)(prop - 1) << 1));
    }
}

static void z_obj_put_prop(uint8_t obj, uint8_t prop, uint16_t val) {
    uint32_t pa = z_obj_get_prop_addr(obj, prop);
    if (pa != 0) {
        uint8_t pb = z_read_byte(pa - 1);
        uint8_t len = (uint8_t)((pb >> 5) + 1);
        if (len == 1) {
            z_write_byte((uint16_t)pa, (uint8_t)(val & 0xFF));
        } else {
            z_write_word((uint16_t)pa, val);
        }
    }
}

static uint8_t z_obj_get_next_prop(uint8_t obj, uint8_t prop) {
    uint32_t pt = z_obj_prop_table(obj);
    uint8_t name_len = z_read_byte(pt);
    pt += 1 + ((uint32_t)name_len << 1);

    if (prop == 0) {
        uint8_t pb = z_read_byte(pt);
        return (uint8_t)(pb & 0x1F);
    }

    while (1) {
        uint8_t pb = z_read_byte(pt);
        uint8_t num = (uint8_t)(pb & 0x1F);
        uint8_t len = (uint8_t)((pb >> 5) + 1);
        if (pb == 0) break;
        if (num == prop) {
            uint8_t next_pb = z_read_byte(pt + 1 + len);
            return (uint8_t)(next_pb & 0x1F);
        }
        pt += 1 + len;
    }
    return 0;
}

static void z_print_obj_name(uint8_t obj) {
    uint32_t pt = z_obj_prop_table(obj);
    uint8_t name_len = z_read_byte(pt);
    if (name_len > 0) {
        z_decode_text(pt + 1, 0, 0, 1);
    }
}

/* --------------------------------------------------------------------------
 * Status Line
 * -------------------------------------------------------------------------- */

static void z_update_status_line(void) {
    uint8_t room_obj = (uint8_t)z_get_var(16);
    int16_t val1 = (int16_t)z_get_var(17);
    int16_t val2 = (int16_t)z_get_var(18);
    uint8_t is_time = (z_read_byte(ZH_FLAGS1) & 0x02) != 0;
    char line[81];
    char room_name[40];
    int len = 0;
    int i;
    uint16_t __far *vram = (uint16_t __far *)MK_FP(s_vram_seg, 0);

    /* Fetch room short name */
    room_name[0] = '\0';
    if (room_obj != 0) {
        uint32_t pt = z_obj_prop_table(room_obj);
        uint8_t nwords = z_read_byte(pt);
        if (nwords > 0) {
            z_decode_text(pt + 1, room_name, sizeof(room_name) - 1, 0);
        }
    }

    /* Clear line with spaces */
    for (i = 0; i < 80; i++) line[i] = ' ';
    line[80] = '\0';

    /* Insert room name at column 1 */
    len = 0;
    while (room_name[len] && len < 45) {
        line[1 + len] = room_name[len];
        len++;
    }

    /* Format Score / Turns (or Time) at right */
    {
        char right_buf[32];
        int rlen = 0;
        if (!is_time) {
            /* Score: X  Moves: Y */
            const char *s = "Score: ";
            int u;
            while (*s) right_buf[rlen++] = *s++;
            if (val1 < 0) {
                right_buf[rlen++] = '-';
                u = -val1;
            } else u = val1;
            if (u == 0) right_buf[rlen++] = '0';
            else {
                char nb[6]; int ni = 0;
                while (u > 0) { nb[ni++] = (char)('0' + (u % 10)); u /= 10; }
                while (ni > 0) right_buf[rlen++] = nb[--ni];
            }

            s = "  Moves: ";
            while (*s) right_buf[rlen++] = *s++;
            u = (uint16_t)val2;
            if (u == 0) right_buf[rlen++] = '0';
            else {
                char nb[6]; int ni = 0;
                while (u > 0) { nb[ni++] = (char)('0' + (u % 10)); u /= 10; }
                while (ni > 0) right_buf[rlen++] = nb[--ni];
            }
        } else {
            /* Time: HH:MM */
            const char *s = "Time: ";
            while (*s) right_buf[rlen++] = *s++;
            right_buf[rlen++] = (char)('0' + ((val1 / 10) % 10));
            right_buf[rlen++] = (char)('0' + (val1 % 10));
            right_buf[rlen++] = ':';
            right_buf[rlen++] = (char)('0' + ((val2 / 10) % 10));
            right_buf[rlen++] = (char)('0' + (val2 % 10));
        }

        if (rlen <= 30) {
            int start_col = 79 - rlen;
            for (i = 0; i < rlen; i++) {
                line[start_col + i] = right_buf[i];
            }
        }
    }

    /* Write directly to Row 0 with reverse video attribute (0x70) */
    for (i = 0; i < 80; i++) {
        vram[i] = ((uint16_t)0x70 << 8) | (uint8_t)line[i];
    }
}

/* --------------------------------------------------------------------------
 * Dictionary Lookup & Tokenization
 * -------------------------------------------------------------------------- */

static int z_is_separator(char c, uint32_t dict_base, uint8_t num_sep) {
    uint8_t i;
    for (i = 0; i < num_sep; i++) {
        if (c == (char)z_read_byte(dict_base + 1 + i)) return 1;
    }
    return 0;
}

static uint32_t z_encode_word(const char *word, int len) {
    uint8_t zchars[6];
    int i;
    uint16_t w1, w2;

    for (i = 0; i < 6; i++) zchars[i] = 5; /* Pad character */

    for (i = 0; i < len && i < 6; i++) {
        char c = word[i];
        if (c >= 'a' && c <= 'z') {
            zchars[i] = (uint8_t)(c - 'a' + 6);
        } else if (c >= 'A' && c <= 'Z') {
            zchars[i] = (uint8_t)(c - 'A' + 6);
        } else {
            int k;
            zchars[i] = 5;
            for (k = 0; k < 26; k++) {
                if (s_alpha2[k] == c) {
                    zchars[i] = (uint8_t)(k + 6);
                    break;
                }
            }
        }
    }

    w1 = (uint16_t)(((uint16_t)zchars[0] << 10) | ((uint16_t)zchars[1] << 5) | zchars[2]);
    w2 = (uint16_t)(((uint16_t)zchars[3] << 10) | ((uint16_t)zchars[4] << 5) | zchars[5]);
    w2 |= 0x8000; /* Set end-of-string bit */

    return ((uint32_t)w1 << 16) | w2;
}

static uint16_t z_dict_lookup(uint32_t encoded_key) {
    uint16_t dict_base = z_read_word(ZH_DICT);
    uint8_t num_sep = z_read_byte(dict_base);
    uint32_t table_base = dict_base + 1 + num_sep;
    uint8_t entry_len = z_read_byte(table_base);
    uint16_t num_entries = z_read_word(table_base + 1);
    uint32_t entries_base = table_base + 3;

    int low = 0;
    int high = (int)num_entries - 1;

    while (low <= high) {
        int mid = (low + high) >> 1;
        uint32_t entry_addr = entries_base + (uint16_t)((uint16_t)mid * entry_len);
        uint32_t entry_key = ((uint32_t)z_read_word(entry_addr) << 16) | z_read_word(entry_addr + 2);

        if (encoded_key == entry_key) {
            return (uint16_t)entry_addr;
        } else if (encoded_key < entry_key) {
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }

    return 0; /* Word not found in dictionary */
}

static void z_tokenise(uint16_t text_buf, uint16_t parse_buf) {
    uint16_t dict_base = z_read_word(ZH_DICT);
    uint8_t num_sep = z_read_byte(dict_base);
    uint8_t max_words = z_read_byte(parse_buf);
    uint8_t num_words = 0;
    int pos = 1;

    while (1) {
        char c = (char)z_read_byte(text_buf + pos);
        if (c == 0) break;

        /* Skip leading whitespace */
        if (c == ' ' || c == '\t') {
            pos++;
            continue;
        }

        /* Check word separator */
        if (z_is_separator(c, dict_base, num_sep)) {
            if (num_words < max_words) {
                char single[2];
                uint32_t key;
                uint16_t dict_addr;
                single[0] = c; single[1] = '\0';
                key = z_encode_word(single, 1);
                dict_addr = z_dict_lookup(key);
                {
                    uint16_t token_addr = (uint16_t)(parse_buf + 2 + (num_words * 4));
                    z_write_word(token_addr, dict_addr);
                    z_write_byte(token_addr + 2, 1);
                    z_write_byte(token_addr + 3, (uint8_t)pos);
                }
                num_words++;
            }
            pos++;
            continue;
        }

        /* Normal word */
        {
            int start = pos;
            char word[32];
            int wlen = 0;
            uint32_t key;
            uint16_t dict_addr;

            while (1) {
                c = (char)z_read_byte(text_buf + pos);
                if (c == 0 || c == ' ' || c == '\t' || z_is_separator(c, dict_base, num_sep)) {
                    break;
                }
                if (wlen < 31) {
                    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
                    word[wlen++] = c;
                }
                pos++;
            }
            word[wlen] = '\0';

            if (num_words < max_words) {
                key = z_encode_word(word, wlen);
                dict_addr = z_dict_lookup(key);
                {
                    uint16_t token_addr = (uint16_t)(parse_buf + 2 + (num_words * 4));
                    z_write_word(token_addr, dict_addr);
                    z_write_byte(token_addr + 2, (uint8_t)(pos - start));
                    z_write_byte(token_addr + 3, (uint8_t)start);
                }
                num_words++;
            }
        }
    }

    z_write_byte(parse_buf + 1, num_words);
}

/* --------------------------------------------------------------------------
 * Subroutine Calls and Branching
 * -------------------------------------------------------------------------- */

static void z_do_branch(int condition) {
    uint8_t b1 = z_fetch_byte();
    int branch_on_true = (b1 & 0x80) != 0;
    int16_t offset;

    if (b1 & 0x40) {
        /* 1-byte short branch (0..63) */
        offset = (int16_t)(b1 & 0x3F);
    } else {
        /* 2-byte long branch (14-bit signed) */
        uint8_t b2 = z_fetch_byte();
        offset = (int16_t)(((uint16_t)(b1 & 0x3F) << 8) | b2);
        if (offset & 0x2000) {
            offset = (int16_t)(offset | 0xC000); /* Sign-extend */
        }
    }

    if ((condition && branch_on_true) || (!condition && !branch_on_true)) {
        if (offset == 0) {
            /* Return false */
            if (s_frame_ptr > 0) {
                z_call_frame_t *f = &s_call_stack[--s_frame_ptr];
                s_pc = f->return_pc;
                s_sp = f->stack_ptr;
                if (f->store_var != 0xFF) {
                    z_set_var(f->store_var, 0);
                }
            }
        } else if (offset == 1) {
            /* Return true */
            if (s_frame_ptr > 0) {
                z_call_frame_t *f = &s_call_stack[--s_frame_ptr];
                s_pc = f->return_pc;
                s_sp = f->stack_ptr;
                if (f->store_var != 0xFF) {
                    z_set_var(f->store_var, 1);
                }
            }
        } else {
            /* Relative branch */
            s_pc = (uint32_t)((int32_t)s_pc + offset - 2);
        }
    }
}

static void z_call_routine(uint16_t paddr, const uint16_t *args, uint8_t num_args, uint8_t store_var) {
    uint32_t byte_addr;
    uint8_t nlocals;
    int i;
    z_call_frame_t *f;

    if (paddr == 0) {
        if (store_var != 0xFF) {
            z_set_var(store_var, 0);
        }
        return;
    }

    if (s_frame_ptr >= Z_MAX_CALL_DEPTH) {
        os_puts("\nzcode: Call stack overflow!\n");
        s_running = 0;
        return;
    }

    byte_addr = (uint32_t)paddr << 1;
    nlocals = z_read_byte(byte_addr++);

    f = &s_call_stack[s_frame_ptr++];
    f->return_pc = s_pc;
    f->stack_ptr = s_sp;
    f->num_locals = nlocals;
    f->store_var = store_var;

    for (i = 0; i < nlocals; i++) {
        f->locals[i] = z_read_word(byte_addr);
        byte_addr += 2;
    }

    for (i = 0; i < num_args && i < nlocals; i++) {
        f->locals[i] = args[i];
    }

    s_pc = byte_addr;
}

/* --------------------------------------------------------------------------
 * Save & Restore State
 * -------------------------------------------------------------------------- */

static int z_save_game(void) {
    /* Save header + PC + SP + stack + call frames + dynamic memory to ZORK1.SAV */
    uint16_t magic = 0x535A; /* "ZS" */
    char fname[16];
    int i;

    fname[0] = 'Z'; fname[1] = 'O'; fname[2] = 'R'; fname[3] = 'K'; fname[4] = '1';
    fname[5] = '.'; fname[6] = 'S'; fname[7] = 'A'; fname[8] = 'V'; fname[9] = '\0';

    if (os_file_write(fname, (const void __far *)FP_HERE(&magic), 2) != 2) return 0;
    os_file_append(fname, (const void __far *)FP_HERE(&s_pc), 4);
    os_file_append(fname, (const void __far *)FP_HERE(&s_sp), 2);
    os_file_append(fname, (const void __far *)FP_HERE(s_stack), s_sp * 2);
    os_file_append(fname, (const void __far *)FP_HERE(&s_frame_ptr), 2);
    for (i = 0; i < s_frame_ptr; i++) {
        os_file_append(fname, (const void __far *)FP_HERE(&s_call_stack[i]), sizeof(z_call_frame_t));
    }
    /* Append dynamic memory */
    os_file_append(fname, (const void __far *)MK_FP(s_story_seg, 0), s_dynamic_size);

    return 1;
}

static int z_restore_game(void) {
    uint16_t magic = 0;
    char fname[16];
    uint32_t offset = 0;
    int i;

    fname[0] = 'Z'; fname[1] = 'O'; fname[2] = 'R'; fname[3] = 'K'; fname[4] = '1';
    fname[5] = '.'; fname[6] = 'S'; fname[7] = 'A'; fname[8] = 'V'; fname[9] = '\0';

    if (os_file_read_offset(fname, (void __far *)FP_HERE(&magic), 2, offset) != 2 || magic != 0x535A) {
        return 0;
    }
    offset += 2;

    os_file_read_offset(fname, (void __far *)FP_HERE(&s_pc), 4, offset); offset += 4;
    os_file_read_offset(fname, (void __far *)FP_HERE(&s_sp), 2, offset); offset += 2;
    os_file_read_offset(fname, (void __far *)FP_HERE(s_stack), s_sp * 2, offset); offset += (uint16_t)(s_sp << 1);
    os_file_read_offset(fname, (void __far *)FP_HERE(&s_frame_ptr), 2, offset); offset += 2;
    for (i = 0; i < s_frame_ptr; i++) {
        os_file_read_offset(fname, (void __far *)FP_HERE(&s_call_stack[i]), sizeof(z_call_frame_t), offset);
        offset += sizeof(z_call_frame_t);
    }
    os_file_read_offset(fname, (void __far *)MK_FP(s_story_seg, 0), s_dynamic_size, offset);

    return 1;
}

/* --------------------------------------------------------------------------
 * Main Interpreter Loop
 * -------------------------------------------------------------------------- */

static void z_execute_instruction(void) {
    uint8_t opcode_byte = z_fetch_byte();
    uint8_t op_count = 0;
    uint16_t operands[8];
    uint8_t op_type[8];
    uint8_t op_num;
    int i;

    /* Decode Operand Count and Types */
    if ((opcode_byte & 0xC0) == 0xC0) {
        /* VAR / 2OP Form */
        uint8_t types;
        op_num = (uint8_t)(opcode_byte & 0x1F);
        types = z_fetch_byte();
        for (i = 0; i < 4; i++) {
            uint8_t t = (uint8_t)((types >> (6 - (i * 2))) & 3);
            if (t == 3) break;
            op_type[op_count++] = t;
        }
    } else if ((opcode_byte & 0xC0) == 0x80) {
        /* 1OP Form */
        uint8_t t = (uint8_t)((opcode_byte >> 4) & 3);
        op_num = (uint8_t)(opcode_byte & 0x0F);
        if (t != 3) {
            op_type[op_count++] = t;
        }
    } else {
        /* 2OP Form */
        op_num = (uint8_t)(opcode_byte & 0x1F);
        op_type[op_count++] = (opcode_byte & 0x40) ? 2 : 1;
        op_type[op_count++] = (opcode_byte & 0x20) ? 2 : 1;
    }

    /* Fetch Operands */
    for (i = 0; i < op_count; i++) {
        if (op_type[i] == 0) {
            operands[i] = z_fetch_word(); /* Large constant */
        } else if (op_type[i] == 1) {
            operands[i] = z_fetch_byte(); /* Small constant */
        } else {
            operands[i] = z_get_var(z_fetch_byte()); /* Variable */
        }
    }

    /* ----------------------------------------------------------------------
     * 2OP Opcodes
     * ---------------------------------------------------------------------- */
    if (opcode_byte < 0x80 || (opcode_byte >= 0xC0 && !(opcode_byte & 0x20))) {
        switch (op_num) {
        case 0x01: { /* je */
            int match = 0;
            for (i = 1; i < op_count; i++) {
                if (operands[0] == operands[i]) { match = 1; break; }
            }
            z_do_branch(match);
            return;
        }
        case 0x02: z_do_branch((int16_t)operands[0] < (int16_t)operands[1]); return; /* jl */
        case 0x03: z_do_branch((int16_t)operands[0] > (int16_t)operands[1]); return; /* jg */
        case 0x04: { /* dec_chk */
            uint8_t v = (uint8_t)operands[0];
            int16_t val = (int16_t)z_get_var(v) - 1;
            z_set_var(v, (uint16_t)val);
            z_do_branch(val < (int16_t)operands[1]);
            return;
        }
        case 0x05: { /* inc_chk */
            uint8_t v = (uint8_t)operands[0];
            int16_t val = (int16_t)z_get_var(v) + 1;
            z_set_var(v, (uint16_t)val);
            z_do_branch(val > (int16_t)operands[1]);
            return;
        }
        case 0x06: z_do_branch(z_obj_parent((uint8_t)operands[0]) == (uint8_t)operands[1]); return; /* jin */
        case 0x07: z_do_branch((operands[0] & operands[1]) == operands[1]); return; /* test */
        case 0x08: z_set_var(z_fetch_byte(), operands[0] | operands[1]); return; /* or */
        case 0x09: z_set_var(z_fetch_byte(), operands[0] & operands[1]); return; /* and */
        case 0x0A: z_do_branch(z_obj_test_attr((uint8_t)operands[0], (uint8_t)operands[1])); return; /* test_attr */
        case 0x0B: z_obj_set_attr((uint8_t)operands[0], (uint8_t)operands[1]); return; /* set_attr */
        case 0x0C: z_obj_clear_attr((uint8_t)operands[0], (uint8_t)operands[1]); return; /* clear_attr */
        case 0x0D: z_set_var((uint8_t)operands[0], operands[1]); return; /* store */
        case 0x0E: z_obj_insert((uint8_t)operands[0], (uint8_t)operands[1]); return; /* insert_obj */
        case 0x0F: z_set_var(z_fetch_byte(), z_read_word((uint32_t)operands[0] + ((uint32_t)operands[1] << 1))); return; /* loadw */
        case 0x10: z_set_var(z_fetch_byte(), z_read_byte((uint32_t)operands[0] + operands[1])); return; /* loadb */
        case 0x11: z_set_var(z_fetch_byte(), z_obj_get_prop((uint8_t)operands[0], (uint8_t)operands[1])); return; /* get_prop */
        case 0x12: z_set_var(z_fetch_byte(), (uint16_t)z_obj_get_prop_addr((uint8_t)operands[0], (uint8_t)operands[1])); return; /* get_prop_addr */
        case 0x13: z_set_var(z_fetch_byte(), z_obj_get_next_prop((uint8_t)operands[0], (uint8_t)operands[1])); return; /* get_next_prop */
        case 0x14: z_set_var(z_fetch_byte(), (uint16_t)((int16_t)operands[0] + (int16_t)operands[1])); return; /* add */
        case 0x15: z_set_var(z_fetch_byte(), (uint16_t)((int16_t)operands[0] - (int16_t)operands[1])); return; /* sub */
        case 0x16: z_set_var(z_fetch_byte(), (uint16_t)((int16_t)operands[0] * (int16_t)operands[1])); return; /* mul */
        case 0x17: { /* div */
            if (operands[1] != 0) {
                z_set_var(z_fetch_byte(), (uint16_t)((int16_t)operands[0] / (int16_t)operands[1]));
            } else {
                z_set_var(z_fetch_byte(), 0);
            }
            return;
        }
        case 0x18: { /* mod */
            if (operands[1] != 0) {
                z_set_var(z_fetch_byte(), (uint16_t)((int16_t)operands[0] % (int16_t)operands[1]));
            } else {
                z_set_var(z_fetch_byte(), 0);
            }
            return;
        }
        }
    }

    /* ----------------------------------------------------------------------
     * 1OP Opcodes
     * ---------------------------------------------------------------------- */
    if (opcode_byte >= 0x80 && opcode_byte < 0xB0) {
        switch (op_num) {
        case 0x00: z_do_branch(operands[0] == 0); return; /* jz */
        case 0x01: { /* get_sibling */
            uint8_t sib = z_obj_sibling((uint8_t)operands[0]);
            z_set_var(z_fetch_byte(), sib);
            z_do_branch(sib != 0);
            return;
        }
        case 0x02: { /* get_child */
            uint8_t ch = z_obj_child((uint8_t)operands[0]);
            z_set_var(z_fetch_byte(), ch);
            z_do_branch(ch != 0);
            return;
        }
        case 0x03: z_set_var(z_fetch_byte(), z_obj_parent((uint8_t)operands[0])); return; /* get_parent */
        case 0x04: { /* get_prop_len */
            if (operands[0] != 0) {
                uint8_t pb = z_read_byte((uint32_t)operands[0] - 1);
                z_set_var(z_fetch_byte(), (uint8_t)((pb >> 5) + 1));
            } else {
                z_set_var(z_fetch_byte(), 0);
            }
            return;
        }
        case 0x05: { /* inc */
            uint8_t v = (uint8_t)operands[0];
            z_set_var(v, (uint16_t)((int16_t)z_get_var(v) + 1));
            return;
        }
        case 0x06: { /* dec */
            uint8_t v = (uint8_t)operands[0];
            z_set_var(v, (uint16_t)((int16_t)z_get_var(v) - 1));
            return;
        }
        case 0x07: z_decode_text((uint32_t)operands[0], 0, 0, 1); return; /* print_addr */
        case 0x09: z_obj_remove((uint8_t)operands[0]); return; /* remove_obj */
        case 0x0A: z_print_obj_name((uint8_t)operands[0]); return; /* print_obj */
        case 0x0B: { /* ret */
            if (s_frame_ptr > 0) {
                z_call_frame_t *f = &s_call_stack[--s_frame_ptr];
                s_pc = f->return_pc;
                s_sp = f->stack_ptr;
                if (f->store_var != 0xFF) {
                    z_set_var(f->store_var, operands[0]);
                }
            }
            return;
        }
        case 0x0C: s_pc = (uint32_t)((int32_t)s_pc + (int16_t)operands[0] - 2); return; /* jump */
        case 0x0D: z_decode_text((uint32_t)operands[0] << 1, 0, 0, 1); return; /* print_paddr */
        case 0x0E: z_set_var(z_fetch_byte(), z_get_var((uint8_t)operands[0])); return; /* load */
        case 0x0F: z_set_var(z_fetch_byte(), (uint16_t)~operands[0]); return; /* not */
        }
    }

    /* ----------------------------------------------------------------------
     * 0OP Opcodes
     * ---------------------------------------------------------------------- */
    if (opcode_byte >= 0xB0 && opcode_byte < 0xC0) {
        switch (op_num) {
        case 0x00: /* rtrue */
            if (s_frame_ptr > 0) {
                z_call_frame_t *f = &s_call_stack[--s_frame_ptr];
                s_pc = f->return_pc;
                s_sp = f->stack_ptr;
                if (f->store_var != 0xFF) z_set_var(f->store_var, 1);
            }
            return;
        case 0x01: /* rfalse */
            if (s_frame_ptr > 0) {
                z_call_frame_t *f = &s_call_stack[--s_frame_ptr];
                s_pc = f->return_pc;
                s_sp = f->stack_ptr;
                if (f->store_var != 0xFF) z_set_var(f->store_var, 0);
            }
            return;
        case 0x02: s_pc = z_decode_text(s_pc, 0, 0, 1); return; /* print */
        case 0x03: { /* print_ret */
            s_pc = z_decode_text(s_pc, 0, 0, 1);
            z_putc('\n');
            if (s_frame_ptr > 0) {
                z_call_frame_t *f = &s_call_stack[--s_frame_ptr];
                s_pc = f->return_pc;
                s_sp = f->stack_ptr;
                if (f->store_var != 0xFF) z_set_var(f->store_var, 1);
            }
            return;
        }
        case 0x04: return; /* nop */
        case 0x05: z_do_branch(z_save_game()); return; /* save */
        case 0x06: z_do_branch(z_restore_game()); return; /* restore */
        case 0x07: s_pc = z_read_word(ZH_PC); s_sp = 0; s_frame_ptr = 0; return; /* restart */
        case 0x08: { /* ret_popped */
            uint16_t v = (s_sp > 0) ? s_stack[--s_sp] : 0;
            if (s_frame_ptr > 0) {
                z_call_frame_t *f = &s_call_stack[--s_frame_ptr];
                s_pc = f->return_pc;
                s_sp = f->stack_ptr;
                if (f->store_var != 0xFF) z_set_var(f->store_var, v);
            }
            return;
        }
        case 0x09: if (s_sp > 0) s_sp--; return; /* pop */
        case 0x0A: s_running = 0; return; /* quit */
        case 0x0B: z_putc('\n'); return; /* new_line */
        case 0x0C: z_update_status_line(); return; /* show_status */
        case 0x0D: z_do_branch(1); return; /* verify */
        }
    }

    /* ----------------------------------------------------------------------
     * VAR Opcodes
     * ---------------------------------------------------------------------- */
    switch (op_num) {
    case 0x00: { /* call */
        uint8_t store_var = z_fetch_byte();
        z_call_routine(operands[0], &operands[1], (uint8_t)(op_count - 1), store_var);
        return;
    }
    case 0x01: z_write_word((uint16_t)(operands[0] + (operands[1] << 1)), operands[2]); return; /* storew */
    case 0x02: z_write_byte((uint16_t)(operands[0] + operands[1]), (uint8_t)(operands[2] & 0xFF)); return; /* storeb */
    case 0x03: z_obj_put_prop((uint8_t)operands[0], (uint8_t)operands[1], operands[2]); return; /* put_prop */
    case 0x04: { /* sread */
        uint16_t text_buf = operands[0];
        uint16_t parse_buf = operands[1];
        uint8_t max_chars = z_read_byte(text_buf);
        char in_line[128];
        int in_len = 0;

        z_flush_word();
        z_update_status_line();

        while (1) {
            uint16_t key = os_getc();
            char c = (char)(key & 0xFF);

            if (c == '\r' || c == '\n') {
                os_putc('\n');
                s_cur_col = 0;
                s_cur_row++;
                s_rows_printed = 0;
                break;
            } else if (c == '\b' || c == 127) {
                if (in_len > 0) {
                    in_len--;
                    os_puts("\b \b");
                    if (s_cur_col > 0) s_cur_col--;
                }
            } else if (c >= 32 && c <= 126) {
                if (in_len + 1 < (int)max_chars && in_len < 120) {
                    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
                    in_line[in_len++] = c;
                    os_putc(c);
                    s_cur_col++;
                }
            } else if (c == 3) { /* Ctrl+C */
                s_running = 0;
                return;
            }
        }
        in_line[in_len] = '\0';

        /* Copy into Z-Machine text buffer starting at offset 1 */
        for (i = 0; i < in_len; i++) {
            z_write_byte(text_buf + 1 + i, (uint8_t)in_line[i]);
        }
        z_write_byte(text_buf + 1 + in_len, 0);

        /* Tokenize input line into parse_buf */
        if (parse_buf != 0) {
            z_tokenise(text_buf, parse_buf);
        }
        return;
    }
    case 0x05: z_putc((char)(operands[0] & 0xFF)); return; /* print_char */
    case 0x06: z_print_num((int16_t)operands[0]); return; /* print_num */
    case 0x07: { /* random */
        int16_t r = (int16_t)operands[0];
        if (r <= 0) {
            s_random_seed = (uint16_t)(-r);
            z_set_var(z_fetch_byte(), 0);
        } else {
            s_random_seed = (uint16_t)(s_random_seed * 25173 + 13849);
            z_set_var(z_fetch_byte(), (uint16_t)((s_random_seed % (uint16_t)r) + 1));
        }
        return;
    }
    case 0x08: { /* push */
        if (s_sp < Z_STACK_SIZE) s_stack[s_sp++] = operands[0];
        return;
    }
    case 0x09: { /* pull */
        uint8_t v = (uint8_t)operands[0];
        uint16_t val = (s_sp > 0) ? s_stack[--s_sp] : 0;
        z_set_var(v, val);
        return;
    }
    }
}

/* --------------------------------------------------------------------------
 * Main Entry Point (app_main)
 * -------------------------------------------------------------------------- */

int app_main(void) {
    const char *args = os_get_args();
    char filepath[64];
    int flen = 0;
    long fsize;
    uint32_t offset;
    uint16_t vtype = os_video_type();

    filepath[0] = '\0';

    if (args) {
        while (*args == ' ') args++;
    }

    if (args && *args != '\0') {
        while (*args && *args != ' ' && flen < 63) {
            filepath[flen++] = *args++;
        }
        filepath[flen] = '\0';
    }

    /* Default to /GAMES/ZORK1.Z3 if no argument specified */
    if (filepath[0] == '\0') {
        const char *def = "GAMES/ZORK1.Z3";
        while (def[flen]) {
            filepath[flen] = def[flen];
            flen++;
        }
        filepath[flen] = '\0';
    }

    fsize = os_file_size(filepath);
    if (fsize < 0) {
        /* Try /ZORK1.Z3 in root */
        const char *alt = "ZORK1.Z3";
        flen = 0;
        while (alt[flen]) { filepath[flen] = alt[flen]; flen++; }
        filepath[flen] = '\0';
        fsize = os_file_size(filepath);
    }

    if (fsize < 0) {
        os_puts("zcode: Story file not found (tried GAMES/ZORK1.Z3)\n");
        os_puts("Usage: zcode [story_file.z3]\n");
        return 1;
    }

    if (fsize > 131072L) {
        os_puts("zcode: Story file exceeds 128 KB maximum for Z-Machine Version 3\n");
        return 1;
    }

    /* Allocate conventional RAM for story file (8192 paragraphs = 128 KB) */
    s_story_seg = os_mem_alloc(8192);
    if (s_story_seg == 0) {
        os_puts("zcode: Insufficient conventional memory to load story file\n");
        return 1;
    }

    /* Load entire story file into allocated conventional RAM */
    s_story_len = (uint32_t)fsize;
    offset = 0;
    while (offset < s_story_len) {
        uint16_t chunk = 16384;
        uint16_t seg = s_story_seg + (uint16_t)(offset >> 4);
        if (offset + chunk > s_story_len) {
            chunk = (uint16_t)(s_story_len - offset);
        }
        if (os_file_read_offset(filepath, (void __far *)MK_FP(seg, 0), chunk, offset) == 0) {
            os_puts("zcode: Error reading story file from disk\n");
            os_mem_free(s_story_seg);
            return 1;
        }
        offset += chunk;
    }

    /* Check Z-Machine Version */
    if (z_read_byte(ZH_VERSION) != 3) {
        os_puts("zcode: Only Z-Machine Version 3 (.z3) story files are currently supported\n");
        os_mem_free(s_story_seg);
        return 1;
    }

    /* Determine Video VRAM segment (MDA: 0xB000, CGA/EGA/VGA: 0xB800) */
    s_vram_seg = (vtype == VIDEO_TYPE_MDA) ? 0xB000 : 0xB800;

    /* Initialize Story State */
    s_dynamic_size = z_read_word(ZH_STATICMEM);
    s_pc = z_read_word(ZH_PC);
    s_sp = 0;
    s_frame_ptr = 0;
    s_running = 1;
    s_cur_col = 0;
    s_cur_row = 1;
    s_rows_printed = 0;
    s_word_len = 0;

    /* Clear screen and draw initial status line */
    os_puts("\033[2J"); /* In case ANSI, or standard cls via BIOS: */
    {
        uint16_t __far *vram = (uint16_t __far *)MK_FP(s_vram_seg, 0);
        int i;
        for (i = 0; i < 80 * 25; i++) {
            vram[i] = ((uint16_t)0x07 << 8) | ' ';
        }
    }
    z_update_status_line();

    /* Main Execution Loop */
    while (s_running) {
        z_execute_instruction();
    }

    z_flush_word();
    os_puts("\n[Session ended]\n");

    /* Free allocated story memory */
    os_mem_free(s_story_seg);
    return 0;
}
