/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HobbyOS Picture Viewer (BIN/VIEW.BIN)
 * Displays CGA (.CGA), VGA (.VGA), and ZSoft PCX (.PCX) pictures in 320x200/640x200 modes.
 */

#include "hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

#define FORMAT_UNKNOWN 0
#define FORMAT_CGA     1
#define FORMAT_VGA     2
#define FORMAT_PCX     3

void outb(uint16_t port, uint8_t val);
#pragma aux outb = \
    "out dx, al" \
    parm [dx] [al] modify [];

static const struct {
    uint8_t palette;
    uint8_t intensity;
} cga_pal_configs[6] = {
    { 1, 1 }, /* 0: Palette 1, High: Cyan, Magenta, White */
    { 0, 1 }, /* 1: Palette 0, High: Light Green, Light Red, Yellow */
    { 2, 1 }, /* 2: Palette 2, High: Light Cyan, Light Red, White */
    { 1, 0 }, /* 3: Palette 1, Normal: Cyan, Magenta, Light Gray */
    { 0, 0 }, /* 4: Palette 0, Normal: Green, Red, Brown */
    { 2, 0 }  /* 5: Palette 2, Normal: Cyan, Red, Light Gray */
};

static uint8_t g_pal_buf[768];

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

static void print_uint32(uint32_t val) {
    char buf[12];
    int i = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0 && i < 11) {
        buf[i++] = (char)('0' + u32_div10(&val));
    }
    while (i > 0) {
        os_putc(buf[--i]);
    }
}

static int str_len(const char *s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

static char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static int str_ends_with_ci(const char *str, const char *suffix) {
    int slen = str_len(str);
    int suflen = str_len(suffix);
    int i;
    if (slen < suflen) return 0;
    for (i = 0; i < suflen; i++) {
        if (to_lower(str[slen - suflen + i]) != to_lower(suffix[i])) return 0;
    }
    return 1;
}

static int str_has_ext(const char *s) {
    int i = str_len(s);
    while (i > 0 && s[i - 1] != '/' && s[i - 1] != ':') {
        if (s[i - 1] == '.') return 1;
        i--;
    }
    return 0;
}

static void set_vga_palette(const uint8_t *pal, int count, int is_8bit) {
    int i;
    outb(0x3C8, 0);
    for (i = 0; i < count * 3; i++) {
        uint8_t c = pal[i];
        if (is_8bit) {
            c >>= 2;
        }
        outb(0x3C9, (uint8_t)(c & 0x3F));
    }
}

static void print_usage(void) {
    os_puts("HobbyOS Picture Viewer (BIN/VIEW.BIN)\n");
    os_puts("Usage: view [options] <image_file>\n\n");
    os_puts("Supported Formats:\n");
    os_puts("  .CGA      CGA 320x200 4-color & 640x200 2-color raw interlaced dumps\n");
    os_puts("  .VGA      VGA Mode 13h 320x200 256-color raw dumps (64,000 / 64,768 bytes)\n");
    os_puts("  .PCX      ZSoft PCX 320x200 256-color RLE compressed pictures\n\n");
    os_puts("Options (CGA mode):\n");
    os_puts("  /P:0|1|2  Start with CGA palette 0 (Green/Red), 1 (Cyan/Magenta), or 2 (Cyan/Red)\n");
    os_puts("  /I:0|1    Start with low (0) or high (1) intensity\n");
    os_puts("  /B:0-15   Set background color (0-15, default 0)\n");
    os_puts("  /6        Display in 640x200 2-color mode (Mode 6)\n\n");
    os_puts("Controls:\n");
    os_puts("  Space / P   Cycle CGA palette (CGA mode only)\n");
    os_puts("  B           Cycle background color (CGA mode only)\n");
    os_puts("  Any key     Exit viewer and restore text mode\n");
}

/* --------------------------------------------------------------------------
 * PCX 256-color Decoder
 * -------------------------------------------------------------------------- */
static int view_pcx(const char *filepath, long fsize) {
    uint8_t hdr[128];
    uint16_t width, height, bpl;
    uint8_t pal_marker;
    uint8_t in_buf[512];
    uint16_t in_pos = 512;
    uint16_t in_len = 512;
    uint32_t cur_offset = 128;
    uint32_t rle_end;
    uint8_t __far *vram;
    uint16_t y;

    if (fsize < 128 + 769) {
        os_puts("view: PCX file too small or corrupted\n");
        return 1;
    }

    /* Read 128-byte PCX Header */
    if (os_file_read_offset(filepath, (void __far *)FP_HERE(hdr), 128, 0) != 128) {
        os_puts("view: Failed to read PCX header\n");
        return 1;
    }

    if (hdr[0] != 0x0A || hdr[1] < 5 || hdr[2] != 1 || hdr[3] != 8) {
        os_puts("view: Unsupported PCX format (requires ZSoft PCX Version 5, 8 bpp, 256 colors)\n");
        return 1;
    }

    width  = (uint16_t)((hdr[8] | (hdr[9] << 8)) - (hdr[4] | (hdr[5] << 8)) + 1);
    height = (uint16_t)((hdr[10] | (hdr[11] << 8)) - (hdr[6] | (hdr[7] << 8)) + 1);
    bpl    = (uint16_t)(hdr[66] | (hdr[67] << 8));

    /* Verify 256-color palette marker byte (0x0C) at (fsize - 769) */
    if (os_file_read_offset(filepath, (void __far *)FP_HERE(&pal_marker), 1, (uint32_t)(fsize - 769)) != 1 ||
        pal_marker != 0x0C) {
        os_puts("view: Missing PCX 256-color palette block\n");
        return 1;
    }

    /* Read 768-byte palette */
    if (os_file_read_offset(filepath, (void __far *)FP_HERE(g_pal_buf), 768, (uint32_t)(fsize - 768)) != 768) {
        os_puts("view: Failed to read PCX palette\n");
        return 1;
    }

    /* Switch to VGA Mode 13h (320x200, 256 colors) */
    if (os_gfx_set_mode(OS_MODE_VGA_320x200_256) != 0) {
        os_puts("view: Failed to set VGA Mode 13h\n");
        return 1;
    }

    /* Set VGA DAC palette */
    set_vga_palette(g_pal_buf, 256, 1);

    /* Decode RLE data directly into physical VGA VRAM (0xA000:0000) */
    vram = (uint8_t __far *)MK_FP(0xA000, 0x0000);
    rle_end = (uint32_t)(fsize - 769);

    for (y = 0; y < height && y < 200; y++) {
        uint16_t row_start = (uint16_t)y * 320;
        uint16_t x = 0;

        while (x < bpl) {
            uint8_t b, count, val;

            if (in_pos >= in_len) {
                uint16_t to_read = 512;
                if (cur_offset + to_read > rle_end) {
                    to_read = (uint16_t)(rle_end - cur_offset);
                }
                if (to_read == 0) break;
                in_len = (uint16_t)os_file_read_offset(filepath, (void __far *)FP_HERE(in_buf), to_read, cur_offset);
                if (in_len == 0 || in_len == 0xFFFF) break;
                cur_offset += in_len;
                in_pos = 0;
            }

            b = in_buf[in_pos++];

            if ((b & 0xC0) == 0xC0) {
                count = (uint8_t)(b & 0x3F);
                if (in_pos >= in_len) {
                    uint16_t to_read = 512;
                    if (cur_offset + to_read > rle_end) {
                        to_read = (uint16_t)(rle_end - cur_offset);
                    }
                    if (to_read == 0) break;
                    in_len = (uint16_t)os_file_read_offset(filepath, (void __far *)FP_HERE(in_buf), to_read, cur_offset);
                    if (in_len == 0 || in_len == 0xFFFF) break;
                    cur_offset += in_len;
                    in_pos = 0;
                }
                val = in_buf[in_pos++];
            } else {
                count = 1;
                val = b;
            }

            while (count > 0 && x < bpl) {
                if (x < 320) {
                    vram[row_start + x] = val;
                }
                x++;
                count--;
            }
        }
    }

    /* Wait for any key to exit */
    while (1) {
        if (os_kbhit()) {
            os_getc();
            break;
        }
    }

    os_gfx_set_mode(OS_MODE_TEXT_80x25);
    return 0;
}

/* --------------------------------------------------------------------------
 * Raw Mode 13h VGA Dump Loader (.VGA)
 * -------------------------------------------------------------------------- */
static int view_vga(const char *filepath, long fsize) {
    uint32_t pix_offset = 0;
    int has_pal = 0;
    int is_8bit = 0;
    uint8_t __far *vram;

    if (fsize >= 64768) {
        /* Standard layout: 64,000 bytes pixels + 768 bytes palette */
        has_pal = 1;
        pix_offset = 0;
        if (os_file_read_offset(filepath, (void __far *)FP_HERE(g_pal_buf), 768, 64000) != 768) {
            has_pal = 0;
        }
    } else if (fsize >= 64007) {
        /* BSAVE format: 7-byte header + 64,000 bytes pixels */
        pix_offset = 7;
    } else if (fsize >= 64000) {
        pix_offset = 0;
    } else {
        os_puts("view: VGA file too small (expected at least 64000 bytes)\n");
        return 1;
    }

    /* Switch to VGA Mode 13h (320x200, 256 colors) */
    if (os_gfx_set_mode(OS_MODE_VGA_320x200_256) != 0) {
        os_puts("view: Failed to set VGA Mode 13h\n");
        return 1;
    }

    /* Program DAC palette if present */
    if (has_pal) {
        int i;
        for (i = 0; i < 768; i++) {
            if (g_pal_buf[i] > 63) {
                is_8bit = 1;
                break;
            }
        }
        set_vga_palette(g_pal_buf, 256, is_8bit);
    }

    /* Transfer 64,000 bytes in two 32,000-byte chunks into VRAM (0xA000:0000) */
    vram = (uint8_t __far *)MK_FP(0xA000, 0x0000);
    os_file_read_offset(filepath, vram, 32000, pix_offset);
    os_file_read_offset(filepath, vram + 32000, 32000, pix_offset + 32000);

    /* Wait for any key to exit */
    while (1) {
        if (os_kbhit()) {
            os_getc();
            break;
        }
    }

    os_gfx_set_mode(OS_MODE_TEXT_80x25);
    return 0;
}

/* --------------------------------------------------------------------------
 * Raw Interlaced CGA Loader (.CGA)
 * -------------------------------------------------------------------------- */
static int view_cga(const char *filepath, long fsize, uint8_t pal_idx, uint8_t bg_color, uint8_t is_mode6, uint8_t num_palettes) {
    uint32_t even_off = 0;
    uint32_t odd_off  = 8192;
    uint8_t mode;
    uint8_t __far *vram_even;
    uint8_t __far *vram_odd;

    if (fsize < 16000) {
        os_puts("view: CGA file too small (minimum 16000 bytes, got ");
        print_uint32((uint32_t)fsize);
        os_puts(" bytes)\n");
        return 1;
    }

    /* Detect CGA format offsets: BSAVE (16391+), 16KB unpadded (16384/16192), or packed (16000) */
    if (fsize >= 16391) {
        uint8_t hdr[7];
        if (os_file_read_offset(filepath, (void __far *)FP_HERE(hdr), 7, 0) == 7 && hdr[0] == 0xFD) {
            even_off = 7;
            odd_off  = 7 + 8192;
        } else {
            even_off = 0;
            odd_off  = 8192;
        }
    } else if (fsize >= 16192) {
        even_off = 0;
        odd_off  = 8192;
    } else {
        even_off = 0;
        odd_off  = 8000;
    }

    mode = is_mode6 ? OS_MODE_CGA_640x200 : OS_MODE_CGA_320x200;
    if (os_gfx_set_mode(mode) != 0) {
        os_puts("view: Failed to set CGA graphics mode\n");
        return 1;
    }

    if (!is_mode6) {
        os_gfx_set_palette(cga_pal_configs[pal_idx].palette,
                           cga_pal_configs[pal_idx].intensity,
                           bg_color);
    }

    /* Stream interleaved scanlines directly into physical CGA VRAM */
    vram_even = (uint8_t __far *)MK_FP(0xB800, 0x0000);
    vram_odd  = (uint8_t __far *)MK_FP(0xB800, 0x2000);

    os_file_read_offset(filepath, vram_even, 8000, even_off);
    os_file_read_offset(filepath, vram_odd,  8000, odd_off);

    /* Interactive viewing loop */
    while (1) {
        if (os_kbhit()) {
            uint16_t key = os_getc();
            char ch = (char)(key & 0xFF);
            if (!is_mode6 && (ch == ' ' || ch == 'p' || ch == 'P')) {
                pal_idx++;
                if (pal_idx >= num_palettes) pal_idx = 0;
                os_gfx_set_palette(cga_pal_configs[pal_idx].palette,
                                   cga_pal_configs[pal_idx].intensity,
                                   bg_color);
            } else if (!is_mode6 && (ch == 'b' || ch == 'B')) {
                bg_color = (uint8_t)((bg_color + 1) & 0x0F);
                os_gfx_set_palette(cga_pal_configs[pal_idx].palette,
                                   cga_pal_configs[pal_idx].intensity,
                                   bg_color);
            } else {
                break;
            }
        }
    }

    os_gfx_set_mode(OS_MODE_TEXT_80x25);
    return 0;
}

/* --------------------------------------------------------------------------
 * Main Entry Point
 * -------------------------------------------------------------------------- */
int app_main(void) {
    const char *args = os_get_args();
    char filepath[64];
    int flen = 0;
    uint8_t is_mode6 = 0;
    uint8_t pal_idx = 0;   /* Default: Palette 1 High */
    uint8_t bg_color = 0;  /* Default: Black */
    long fsize;
    uint16_t vtype = os_video_type();
    uint8_t num_palettes;
    int fmt = FORMAT_UNKNOWN;
    uint8_t magic[4];

    filepath[0] = '\0';

    if (vtype == VIDEO_TYPE_MDA) {
        os_puts("view: MDA adapter detected. Graphics display is not supported on MDA.\n");
        return 1;
    }

    /* On VGA, intensity differences in CGA emulation are omitted; cycle 3 high palettes */
    num_palettes = (vtype >= VIDEO_TYPE_VGA) ? 3 : 6;

    if (args) {
        while (*args == ' ') args++;
    }

    if (!args || *args == '\0') {
        print_usage();
        return 1;
    }

    /* Parse command-line arguments */
    while (*args) {
        while (*args == ' ') args++;
        if (*args == '\0') break;

        if (*args == '/' || *args == '-') {
            args++;
            if (*args == '?' || *args == 'h' || *args == 'H') {
                print_usage();
                return 0;
            } else if (*args == '6') {
                is_mode6 = 1;
                args++;
            } else if (*args == 'm' || *args == 'M') {
                args++;
                if (*args == '6') {
                    is_mode6 = 1;
                    args++;
                }
            } else if (*args == 'p' || *args == 'P') {
                args++;
                if (*args == ':') args++;
                if (*args == '0') {
                    pal_idx = 1; /* Pal 0 High */
                    args++;
                } else if (*args == '1') {
                    pal_idx = 0; /* Pal 1 High */
                    args++;
                } else if (*args == '2') {
                    pal_idx = 2; /* Pal 2 High */
                    args++;
                }
            } else if (*args == 'i' || *args == 'I') {
                args++;
                if (*args == ':') args++;
                if (*args == '0' && num_palettes == 6) {
                    if (pal_idx < 3) pal_idx += 3; /* Low intensity on CGA */
                    args++;
                } else if (*args == '1') {
                    if (pal_idx >= 3) pal_idx -= 3; /* High intensity */
                    args++;
                }
            } else if (*args == 'b' || *args == 'B') {
                uint8_t b = 0;
                args++;
                if (*args == ':') args++;
                while (*args >= '0' && *args <= '9') {
                    b = (uint8_t)(b * 10 + (*args - '0'));
                    args++;
                }
                bg_color = (uint8_t)(b & 0x0F);
            } else {
                while (*args && *args != ' ') args++;
            }
        } else {
            flen = 0;
            while (*args && *args != ' ' && flen < 63) {
                filepath[flen++] = *args++;
            }
            filepath[flen] = '\0';
        }
    }

    if (filepath[0] == '\0') {
        print_usage();
        return 1;
    }

    /* Check file existence; try appending extensions if not found and no extension */
    fsize = os_file_size(filepath);
    if (fsize < 0 && !str_has_ext(filepath) && flen <= 59) {
        /* Try .PCX */
        filepath[flen] = '.'; filepath[flen + 1] = 'P'; filepath[flen + 2] = 'C'; filepath[flen + 3] = 'X'; filepath[flen + 4] = '\0';
        fsize = os_file_size(filepath);
        if (fsize < 0) {
            /* Try .VGA */
            filepath[flen + 1] = 'V'; filepath[flen + 2] = 'G'; filepath[flen + 3] = 'A';
            fsize = os_file_size(filepath);
        }
        if (fsize < 0) {
            /* Try .CGA */
            filepath[flen + 1] = 'C'; filepath[flen + 2] = 'G'; filepath[flen + 3] = 'A';
            fsize = os_file_size(filepath);
        }
    }

    if (fsize < 0) {
        os_puts("view: File not found: ");
        os_puts(filepath);
        os_putc('\n');
        return 1;
    }

    /* Detect file format from extension or magic */
    if (str_ends_with_ci(filepath, ".pcx")) {
        fmt = FORMAT_PCX;
    } else if (str_ends_with_ci(filepath, ".vga")) {
        fmt = FORMAT_VGA;
    } else if (str_ends_with_ci(filepath, ".cga")) {
        fmt = FORMAT_CGA;
    } else {
        /* Inspect header magic */
        if (os_file_read_offset(filepath, (void __far *)FP_HERE(magic), 4, 0) == 4) {
            if (magic[0] == 0x0A && magic[1] >= 5 && magic[2] == 1 && magic[3] == 8) {
                fmt = FORMAT_PCX;
            } else if (fsize == 64768 || fsize == 64000 || fsize == 64007) {
                fmt = FORMAT_VGA;
            } else {
                fmt = FORMAT_CGA;
            }
        }
    }

    /* Hardware capability check for 256-color VGA formats */
    if (fmt != FORMAT_CGA && vtype < VIDEO_TYPE_VGA) {
        os_puts("view: VGA adapter required to display 256-color picture (detected: ");
        os_puts(os_video_name(vtype));
        os_puts(")\n");
        return 1;
    }

    if (fmt == FORMAT_PCX) {
        return view_pcx(filepath, fsize);
    } else if (fmt == FORMAT_VGA) {
        return view_vga(filepath, fsize);
    } else {
        return view_cga(filepath, fsize, pal_idx, bg_color, is_mode6, num_palettes);
    }
}
