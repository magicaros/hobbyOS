/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * user/sam.c - 8088-uLM ("Sam") Retro Language Model Engine
 * 4-bit quantized neural character LM running on Intel 8088 CPU @ 8 MHz.
 * Staged in Lo-tech 2MB EMS memory at 0xE000:0000 with conventional RAM fallback.
 */

#include "hobbyos.h"

#define MK_FP(seg, off) ((void __far *)(((uint32_t)(seg) << 16) | (uint16_t)(off)))

#define MODEL_PATH_DEFAULT "/MODELS/SAM.MDL"
#define MODEL_PATH_ALT     "SAM.MDL"

#define VOCAB_SIZE  96
#define IN_DIM      256
#define H1_DIM      128
#define H2_DIM      96
#define OUT_DIM     96

#define MODEL_TOTAL_SIZE 28224U

/* Signed 4-bit nibble lookup table: maps 0..15 to -8..+7 */
static const int8_t s_nibble[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1
};

/* Neural network activation buffers in conventional user BSS */
static int8_t   s_in_vec[IN_DIM];
static uint8_t  s_active_idx[32];
static uint8_t  s_active_count = 0;
static int16_t  s_h1[H1_DIM];
static int16_t  s_h2[H2_DIM];
static int32_t  s_logits[OUT_DIM];

static int16_t  s_acc1[H1_DIM];
static int32_t  s_acc2[H2_DIM];
static int32_t  s_acc3[OUT_DIM];

/* EMS allocation state */
static int      s_ems_active = 0;
static uint16_t s_ems_page0  = 0xFFFF;
static uint16_t s_ems_page1  = 0xFFFF;
static uint16_t s_conv_seg   = 0;

static const uint8_t __far *s_model = 0;

/* Offsets in SAM.MDL */
#define OFF_HDR      0U
#define OFF_W1       64U
#define OFF_B1       16448U
#define OFF_W2       16704U
#define OFF_B2       22848U
#define OFF_W3       23232U
#define OFF_B3       27840U

static uint8_t str_len(const char *s) {
    uint8_t len = 0;
    while (*s++) len++;
    return len;
}

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static uint8_t char_to_tok(char c) {
    if (c == '\n') return 0;
    if (c >= 32 && c <= 126) return (uint8_t)(c - 31);
    return 1; /* Space */
}

static char tok_to_char(uint8_t t) {
    if (t == 0) return '\n';
    return (char)(t + 31);
}

/* Polynomial rolling trigram hash: ((c1*31 + c2)*31 + c3) mod 128 */
static uint8_t trigram_hash(char c1, char c2, char c3) {
    uint16_t h = (uint16_t)(((uint8_t)c1 << 5) - (uint8_t)c1 + (uint8_t)c2) & 0x7FFF;
    h = (uint16_t)(((h << 5) - h) + (uint8_t)c3) & 0x7FFF;
    return (uint8_t)(h & 127);
}

static void encode_prompt(const char *prompt) {
    uint8_t len = str_len(prompt);
    uint8_t i;
    char c1, c2, c3;

    for (i = 0; i < 128; i++) {
        s_in_vec[i] = 0;
    }

    if (len == 0) return;

    for (i = 0; i < len; i++) {
        c1 = (i >= 2) ? to_lower(prompt[i - 2]) : ' ';
        c2 = (i >= 1) ? to_lower(prompt[i - 1]) : ' ';
        c3 = to_lower(prompt[i]);
        {
            uint8_t b = trigram_hash(c1, c2, c3);
            if (s_in_vec[b] < 7) {
                s_in_vec[b]++;
            }
        }
    }
}

static void encode_context(const char *resp, uint8_t resp_len) {
    uint8_t i;
    uint8_t s_len = (resp_len > 8) ? 8 : resp_len;
    uint8_t pad = 8 - s_len;
    const char *s = resp + (resp_len - s_len);

    for (i = 0; i < 128; i++) {
        s_in_vec[128 + i] = 0;
    }

    /* Set 1-hot bucket only for characters actually present in the context */
    for (i = 0; i < s_len; i++) {
        uint8_t actual_pos = pad + i;
        char c = to_lower(s[i]);
        uint8_t tok = (c >= 32 && c <= 126) ? (uint8_t)(c - 31) : 0;
        uint8_t b = (uint8_t)((actual_pos << 4) + (tok & 15));
        s_in_vec[128 + b] = 1;
    }
}

/*
 * Run one forward inference step:
 * Input: s_in_vec (256)
 * Output: predicted next character
 */
static char forward_step(void) {
    uint16_t i, j, k;
    uint8_t best_tok;
    int32_t max_logit;

    const uint8_t __far *w1_bytes = s_model + OFF_W1;
    const int16_t __far *b1_words = (const int16_t __far *)(s_model + OFF_B1);

    const uint8_t __far *w2_bytes = s_model + OFF_W2;
    const int32_t __far *b2_dwords = (const int32_t __far *)(s_model + OFF_B2);

    const uint8_t __far *w3_bytes = s_model + OFF_W3;
    const int32_t __far *b3_dwords = (const int32_t __far *)(s_model + OFF_B3);

    /* Collect active input indices (typically ~15-20 non-zero entries) */
    s_active_count = 0;
    for (i = 0; i < IN_DIM; i++) {
        if (s_in_vec[i] > 0) {
            s_active_idx[s_active_count++] = (uint8_t)i;
        }
    }

    /* --- Layer 1: 256 -> 128 --- */
    for (j = 0; j < H1_DIM; j++) {
        s_acc1[j] = 0;
    }

    for (k = 0; k < s_active_count; k++) {
        uint8_t in_idx = s_active_idx[k];
        int16_t x = s_in_vec[in_idx];
        const uint8_t __far *row = w1_bytes + ((uint16_t)in_idx * (H1_DIM / 2));
        int16_t *acc = s_acc1;
        int16_t x_times_nib[16];
        uint16_t n;

        for (n = 0; n < 16; n++) {
            x_times_nib[n] = x * (int16_t)s_nibble[n];
        }

        for (j = 0; j < (H1_DIM / 2); j++) {
            uint8_t b = *row++;
            *acc++ += x_times_nib[b & 0x0F];
            *acc++ += x_times_nib[(b >> 4) & 0x0F];
        }
    }

    for (j = 0; j < H1_DIM; j++) {
        int16_t val = s_acc1[j] + b1_words[j];
        s_h1[j] = (val > 0) ? val : 0;
    }

    /* --- Layer 2: 128 -> 96 --- */
    for (j = 0; j < H2_DIM; j++) {
        s_acc2[j] = 0;
    }

    for (i = 0; i < H1_DIM; i++) {
        int16_t x = s_h1[i];
        const uint8_t __far *row;
        int32_t *acc;
        int32_t x_times_nib[16];
        uint16_t n;

        if (!x) continue;

        for (n = 0; n < 16; n++) {
            x_times_nib[n] = (int32_t)(x * (int16_t)s_nibble[n]);
        }

        row = w2_bytes + (i * (H2_DIM / 2));
        acc = s_acc2;
        for (j = 0; j < (H2_DIM / 2); j++) {
            uint8_t b = *row++;
            *acc++ += x_times_nib[b & 0x0F];
            *acc++ += x_times_nib[(b >> 4) & 0x0F];
        }
    }

    for (j = 0; j < H2_DIM; j++) {
        int32_t val = s_acc2[j] + b2_dwords[j];
        s_h2[j] = (val > 0) ? (int16_t)val : 0;
    }

    /* --- Output Layer: 96 -> 96 --- */
    for (j = 0; j < OUT_DIM; j++) {
        s_acc3[j] = 0;
    }

    for (i = 0; i < H2_DIM; i++) {
        int16_t x = s_h2[i];
        const uint8_t __far *row;
        int32_t *acc;
        int32_t x_times_nib[16];
        uint16_t n;

        if (!x) continue;

        for (n = 0; n < 16; n++) {
            x_times_nib[n] = (int32_t)(x * (int16_t)s_nibble[n]);
        }

        row = w3_bytes + (i * (OUT_DIM / 2));
        acc = s_acc3;
        for (j = 0; j < (OUT_DIM / 2); j++) {
            uint8_t b = *row++;
            *acc++ += x_times_nib[b & 0x0F];
            *acc++ += x_times_nib[(b >> 4) & 0x0F];
        }
    }

    best_tok = 0;
    max_logit = s_acc3[0] + b3_dwords[0];

    for (j = 1; j < OUT_DIM; j++) {
        int32_t val = s_acc3[j] + b3_dwords[j];
        if (val > max_logit) {
            max_logit = val;
            best_tok = (uint8_t)j;
        }
    }

    return tok_to_char(best_tok);
}

static void run_prompt(const char *prompt) {
    char resp[128];
    uint8_t resp_len = 0;
    uint8_t step;

    encode_prompt(prompt);

    os_puts("Sam: ");
    for (step = 0; step < 120; step++) {
        char ch;
        encode_context(resp, resp_len);
        ch = forward_step();
        if (ch == '\n') {
            break;
        }
        os_putc(ch);
        if (resp_len + 1 < sizeof(resp)) {
            resp[resp_len++] = ch;
        }
    }
    os_putc('\n');
}

static int load_model(void) {
    uint16_t bytes_read;

    /* Check if Lo-tech EMS memory (2 pages = 32 KB) is available */
    if (os_ems_avail() >= 2) {
        int p0 = os_ems_alloc();
        int p1 = os_ems_alloc();
        if (p0 >= 0 && p1 >= 0) {
            s_ems_page0 = (uint16_t)p0;
            s_ems_page1 = (uint16_t)p1;
            os_ems_map(0, s_ems_page0);
            os_ems_map(1, s_ems_page1);
            s_model = (const uint8_t __far *)MK_FP(0xE000, 0);
            s_ems_active = 1;
            os_puts("[SAM] Pristine 1980s SRAM engaged: 32 KB across 2 EMS pages at 0xE000.\n");
        }
    }

    if (!s_ems_active) {
        /* Fallback: conventional user memory */
        uint16_t paras = (MODEL_TOTAL_SIZE + 15) >> 4;
        s_conv_seg = os_mem_alloc(paras);
        if (s_conv_seg == 0) {
            os_puts("[SAM] Error: Out of memory (cannot allocate model buffer).\n");
            return -1;
        }
        s_model = (const uint8_t __far *)MK_FP(s_conv_seg, 0);
        os_puts("[SAM] No EMS detected. Falling back to conventional RAM (Sam would weep).\n");
    }

    /* Try loading from /MODELS/SAM.MDL, then SAM.MDL */
    bytes_read = os_file_read(MODEL_PATH_DEFAULT, (void __far *)s_model, MODEL_TOTAL_SIZE);
    if (bytes_read != MODEL_TOTAL_SIZE) {
        bytes_read = os_file_read(MODEL_PATH_ALT, (void __far *)s_model, MODEL_TOTAL_SIZE);
    }

    if (bytes_read != MODEL_TOTAL_SIZE) {
        os_puts("[SAM] Error: Failed to load /MODELS/SAM.MDL\n");
        return -1;
    }

    /* Validate magic "SMLM" */
    if (s_model[0] != 'S' || s_model[1] != 'M' || s_model[2] != 'L' || s_model[3] != 'M') {
        os_puts("[SAM] Error: Invalid model magic.\n");
        return -1;
    }

    return 0;
}

static void cleanup_model(void) {
    if (s_ems_active) {
        if (s_ems_page0 != 0xFFFF) os_ems_free(s_ems_page0);
        if (s_ems_page1 != 0xFFFF) os_ems_free(s_ems_page1);
        s_ems_page0 = 0xFFFF;
        s_ems_page1 = 0xFFFF;
        s_ems_active = 0;
    }
    if (s_conv_seg != 0) {
        os_mem_free(s_conv_seg);
        s_conv_seg = 0;
    }
}

static uint16_t read_input_line(char *buf, uint16_t max_len) {
    uint16_t len = 0;
    for (;;) {
        uint16_t key = os_getc();
        uint8_t  u   = (uint8_t)(key & 0xFF);
        char     c   = (char)u;

        if (c == '\r' || c == '\n') {
            os_putc('\n');
            buf[len] = '\0';
            return len;
        } else if (c == '\b' || u == 0x7F) {
            if (len > 0) {
                len--;
                os_putc('\b');
            }
        } else if (u >= 0x20 && u != 0x7F) {
            if (len + 1 < max_len) {
                buf[len++] = c;
                os_putc(c);
            }
        }
    }
}

static int str_equal(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (to_lower(*s1) != to_lower(*s2)) return 0;
        s1++;
        s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

void app_main(void) {
    const char *args = os_get_args();
    char line[128];

    if (load_model() != 0) {
        cleanup_model();
        os_exit(1);
    }

    /* Skip leading whitespace in arguments */
    while (*args == ' ' || *args == '\t') args++;

    if (*args != '\0') {
        /* One-shot execution mode */
        run_prompt(args);
    } else {
        /* Interactive chat mode */
        os_puts("\n============================================================\n");
        os_puts("  SAM (8088-uLM) - Vintage Character-Level Retro AI\n");
        os_puts("  Powered by 4-bit Quantization & Lo-tech EMS SRAM\n");
        os_puts("============================================================\n");
        os_puts("Type a prompt and press Enter. (Type 'quit' or 'exit' to quit)\n\n");

        for (;;) {
            os_puts("SAM> ");
            if (read_input_line(line, sizeof(line)) == 0) {
                continue;
            }
            if (str_equal(line, "quit") || str_equal(line, "exit")) {
                os_puts("Powering down. The SRAM will remember.\n");
                break;
            }
            run_prompt(line);
        }
    }

    cleanup_model();
    os_exit(0);
}
