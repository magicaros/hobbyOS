/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Sound player and IMF (Id Music Format) player utility (/BIN/PLAY.BIN).
 */

#include "hobbyos.h"

#define FP_HERE(ptr) ((void __far *)(((unsigned long)get_ds() << 16) | (uint16_t)(ptr)))

uint8_t inb(uint16_t port);
#pragma aux inb = \
    "in al, dx" \
    parm [dx] value [al];

void outb(uint16_t port, uint8_t val);
#pragma aux outb = \
    "out dx, al" \
    parm [dx] [al];

uint16_t get_pit_counter(void);
#pragma aux get_pit_counter = \
    "pushf" \
    "cli" \
    "mov al, 00h" \
    "out 43h, al" \
    "in al, 40h" \
    "mov ah, al" \
    "in al, 40h" \
    "xchg al, ah" \
    "popf" \
    value [ax];

static uint16_t s_pit_last = 0;
static uint16_t s_pit_accum = 0;

static void pit_timer_start(void) {
    /* Set Timer 0 to Mode 2 (Rate generator), binary, divisor 65536 (0).
     * Mode 2 counts down continuously by 1 on every 1.193182 MHz clock cycle
     * from 65535 down to 1, pulsing OUT0 at 18.2 Hz for BIOS tick compatibility.
     */
    outb(0x43, 0x34);
    outb(0x40, 0x00);
    outb(0x40, 0x00);
    s_pit_last = get_pit_counter();
    s_pit_accum = 0;
}

static void pit_timer_stop(void) {
    /* Restore Timer 0 to standard BIOS Mode 3 (Square wave), divisor 65536 */
    outb(0x43, 0x36);
    outb(0x40, 0x00);
    outb(0x40, 0x00);
}

static int wait_imf_ticks_abortable(uint16_t ticks, uint16_t counts_per_tick) {
    while (ticks > 0) {
        uint16_t cur = get_pit_counter();
        /* Timer 0 counts down, so elapsed = last - cur */
        uint16_t elapsed = s_pit_last - cur;
        s_pit_last = cur;
        s_pit_accum += elapsed;

        while (s_pit_accum >= counts_per_tick && ticks > 0) {
            s_pit_accum -= counts_per_tick;
            ticks--;
        }

        if (os_kbhit()) {
            return 1; /* Aborted by user */
        }
    }
    return 0;
}

static void print_uint16(uint16_t val) {
    char buf[8];
    int i = 0;
    if (val == 0) {
        os_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    while (i > 0) {
        os_putc(buf[--i]);
    }
}

static void print_hex16(uint16_t val) {
    static const char hex[] = "0123456789ABCDEF";
    os_puts("0x");
    os_putc(hex[(val >> 12) & 0x0F]);
    os_putc(hex[(val >> 8) & 0x0F]);
    os_putc(hex[(val >> 4) & 0x0F]);
    os_putc(hex[val & 0x0F]);
}

static void show_info(void) {
    uint16_t port = 0;
    uint16_t channels = 0;
    int dev = os_sound_info(&port, &channels);

    os_puts("hobbyOS Unified Sound Subsystem:\n");
    os_puts("  Device:    ");
    os_puts(os_sound_device_name(dev));
    os_putc('\n');

    os_puts("  Base Port: ");
    if (port) {
        print_hex16(port);
    } else {
        os_puts("N/A");
    }
    os_putc('\n');

    os_puts("  Channels:  ");
    print_uint16(channels);
    os_putc('\n');

    os_puts("  Status:    ");
    if (dev == SOUND_DEV_OPL3) {
        os_puts("Yamaha OPL3 Stereo FM Active\n");
    } else if (dev == SOUND_DEV_OPL2) {
        os_puts("Yamaha OPL2 Mono FM Active\n");
    } else if (dev == SOUND_DEV_SPEAKER) {
        os_puts("PC Speaker PIT Square-Wave Active\n");
    } else {
        os_puts("Sound hardware muted\n");
    }
}

static void play_test(void) {
    /* 4-note retro arpeggio: C4 (261 Hz), E4 (329 Hz), G4 (392 Hz), C5 (523 Hz) */
    os_puts("Playing test melody... (press any key to stop)\n");

    os_sound_tone(261, 200, 0);
    if (os_kbhit()) { os_sound_stop(); return; }

    os_sound_tone(329, 200, 0);
    if (os_kbhit()) { os_sound_stop(); return; }

    os_sound_tone(392, 200, 0);
    if (os_kbhit()) { os_sound_stop(); return; }

    os_sound_tone(523, 400, 0);
    os_sound_stop();
    os_puts("Done.\n");
}

#define IMF_CHUNK_BYTES 512

static int str_contains_icase(const char *str, const char *sub) {
    while (*str) {
        const char *s = str;
        const char *p = sub;
        while (*s && *p) {
            char cs = *s;
            char cp = *p;
            if (cs >= 'A' && cs <= 'Z') cs += ('a' - 'A');
            if (cp >= 'A' && cp <= 'Z') cp += ('a' - 'A');
            if (cs != cp) break;
            s++;
            p++;
        }
        if (*p == '\0') return 1;
        str++;
    }
    return 0;
}

static void play_imf_file(const char *path, uint16_t req_rate) {
    static uint8_t chunk[IMF_CHUNK_BYTES];
    uint32_t file_offset = 0;
    int32_t fsize;
    int bytes_read;
    uint16_t rec_idx;
    uint16_t start_offset = 0;
    uint16_t port, channels;
    int dev;
    uint16_t rate = req_rate;
    uint16_t counts_per_tick;

    fsize = os_file_size(path);
    if (fsize <= 0) {
        os_puts("play: File not found or empty: ");
        os_puts(path);
        os_putc('\n');
        return;
    }

    /* Auto-detect playback rate if not specified */
    if (rate == 0) {
        if (str_contains_icase(path, "wonderin") ||
            str_contains_icase(path, "wolf") ||
            str_contains_icase(path, "700") ||
            str_contains_icase(path, ".wlf")) {
            rate = 700;
        } else {
            rate = 560;
        }
    }

    /* 1,193,182 Hz PIT clock:
     * 560 Hz -> 2131 PIT counts per tick
     * 700 Hz -> 1705 PIT counts per tick
     */
    if (rate == 700) {
        counts_per_tick = 1705;
    } else {
        counts_per_tick = 2131;
    }

    dev = os_sound_info(&port, &channels);
    os_puts("Playing ");
    os_puts(path);
    os_puts(" (");
    print_uint16(rate);
    os_puts(" Hz) on ");
    os_puts(os_sound_device_name(dev));
    os_puts("... (press any key to stop)\n");

    /* Check if Type-1 IMF (2-byte length prefix) */
    bytes_read = os_file_read_offset(path, FP_HERE(chunk), 4, 0);
    if (bytes_read == 4) {
        uint16_t pfx = (uint16_t)chunk[0] | ((uint16_t)chunk[1] << 8);
        if (pfx != 0 && pfx == (uint16_t)(fsize - 2)) {
            start_offset = 2; /* Skip 2-byte Type-1 header */
        }
    }

    file_offset = start_offset;
    pit_timer_start();

    /* Standard AdLib/OPL initialization */
    os_sound_opl_write(0x01, 0x20); /* Enable waveform control */
    os_sound_opl_write(0x08, 0x00); /* Disable split point / speech mode */
    os_sound_opl_write(0xBD, 0x00); /* Tremolo/vibrato depth 0, rhythm mode off */

    while (file_offset < (uint32_t)fsize) {
        bytes_read = os_file_read_offset(path, FP_HERE(chunk), IMF_CHUNK_BYTES, file_offset);
        if (bytes_read <= 0) break;

        rec_idx = 0;
        while ((int)rec_idx + 4 <= bytes_read) {
            uint8_t  reg   = chunk[rec_idx + 0];
            uint8_t  val   = chunk[rec_idx + 1];
            uint16_t delay = (uint16_t)chunk[rec_idx + 2] | ((uint16_t)chunk[rec_idx + 3] << 8);

            os_sound_opl_write((uint16_t)reg, val);

            if (delay > 0) {
                if (wait_imf_ticks_abortable(delay, counts_per_tick)) {
                    os_getc();
                    pit_timer_stop();
                    os_sound_stop();
                    os_puts("\nPlayback stopped by user.\n");
                    return;
                }
            }

            rec_idx += 4;
        }

        file_offset += (uint32_t)rec_idx;
    }

    pit_timer_stop();
    os_sound_stop();
    os_puts("\nPlayback finished.\n");
}

#define WAV_CHUNK_BYTES 4096

static void play_wav_file(const char *path) {
    static uint8_t chunk[WAV_CHUNK_BYTES];
    int32_t fsize;
    int bytes_read;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;
    uint16_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint16_t format = 0;
    uint16_t p = 0, ch = 0, sb_base = 0;
    uint8_t sb_type = 0;
    uint32_t cur_offset = 0;

    fsize = os_file_size(path);
    if (fsize <= 44) {
        os_puts("play: File not found or invalid WAV: ");
        os_puts(path);
        os_putc('\n');
        return;
    }

    /* Check Sound Blaster availability */
    os_sound_info_ex(&p, &ch, &sb_type, &sb_base);
    if (sb_type == SB_TYPE_NONE || sb_base == 0) {
        os_puts("play: Sound Blaster DSP required for WAV PCM playback (none detected)\n");
        return;
    }

    /* Read first 44 bytes to parse RIFF header */
    bytes_read = os_file_read_offset(path, FP_HERE(chunk), 44, 0);
    if (bytes_read < 44) {
        os_puts("play: Error reading WAV header\n");
        return;
    }

    /* Verify "RIFF" and "WAVE" */
    if (chunk[0] != 'R' || chunk[1] != 'I' || chunk[2] != 'F' || chunk[3] != 'F' ||
        chunk[8] != 'W' || chunk[9] != 'A' || chunk[10] != 'V' || chunk[11] != 'E') {
        os_puts("play: Not a valid RIFF/WAVE file\n");
        return;
    }

    /* Parse fmt subchunk */
    if (chunk[12] == 'f' && chunk[13] == 'm' && chunk[14] == 't' && chunk[15] == ' ') {
        format = (uint16_t)chunk[20] | ((uint16_t)chunk[21] << 8);
        channels = (uint16_t)chunk[22] | ((uint16_t)chunk[23] << 8);
        sample_rate = (uint16_t)chunk[24] | ((uint16_t)chunk[25] << 8);
        bits = (uint16_t)chunk[34] | ((uint16_t)chunk[35] << 8);
    } else {
        os_puts("play: 'fmt ' subchunk not found at expected offset\n");
        return;
    }

    if (format != 1) {
        os_puts("play: Only uncompressed PCM WAV supported\n");
        return;
    }
    if (channels != 1) {
        os_puts("play: Only mono WAV supported\n");
        return;
    }
    if (bits != 8) {
        os_puts("play: Only 8-bit unsigned PCM WAV supported\n");
        return;
    }

    /* Find "data" chunk header */
    if (chunk[36] == 'd' && chunk[37] == 'a' && chunk[38] == 't' && chunk[39] == 'a') {
        data_size = (uint32_t)chunk[40] | ((uint32_t)chunk[41] << 8) |
                    ((uint32_t)chunk[42] << 16) | ((uint32_t)chunk[43] << 24);
        data_offset = 44;
    } else {
        data_offset = 44;
        data_size = (uint32_t)(fsize - 44);
    }

    if (data_offset + data_size > (uint32_t)fsize) {
        data_size = (uint32_t)(fsize - data_offset);
    }

    os_puts("Playing ");
    os_puts(path);
    os_puts(" (8-bit Mono PCM, ");
    print_uint16(sample_rate);
    os_puts(" Hz, ");
    print_uint16((uint16_t)data_size);
    os_puts(" bytes) on ");
    os_puts(os_sound_sb_name(sb_type));
    os_puts("... (press any key to stop)\n");

    cur_offset = data_offset;
    while (cur_offset < data_offset + data_size) {
        uint16_t to_read = WAV_CHUNK_BYTES;
        uint32_t remaining = (data_offset + data_size) - cur_offset;
        if ((uint32_t)to_read > remaining) {
            to_read = (uint16_t)remaining;
        }

        bytes_read = os_file_read_offset(path, FP_HERE(chunk), to_read, cur_offset);
        if (bytes_read <= 0) break;

        /* Start PCM DMA transfer */
        if (os_sound_pcm_play(sample_rate, (uint16_t)bytes_read, FP_HERE(chunk)) < 0) {
            os_puts("play: PCM playback failed\n");
            break;
        }

        /* Wait for DMA chunk to complete playing */
        while (os_sound_pcm_status()) {
            if (os_kbhit()) {
                os_getc();
                os_sound_pcm_stop();
                os_puts("\nPlayback stopped by user.\n");
                return;
            }
        }

        cur_offset += (uint32_t)bytes_read;
    }

    os_sound_pcm_stop();
    os_puts("Playback finished.\n");
}

static void start_bg_imf(const char *path, uint16_t req_rate) {
    int32_t fsize = os_file_size(path);
    uint16_t rate = req_rate;
    uint16_t seg;
    void __far *buf;
    int bytes_read;
    const char *fname;

    if (fsize <= 0 || fsize > 32768) {
        os_puts("play: File not found or exceeds 32 KB limit for background: ");
        os_puts(path);
        os_putc('\n');
        return;
    }

    if (rate == 0) {
        if (str_contains_icase(path, "wonderin") ||
            str_contains_icase(path, "wolf") ||
            str_contains_icase(path, "700") ||
            str_contains_icase(path, ".wlf")) {
            rate = 700;
        } else {
            rate = 560;
        }
    }

    seg = os_mem_alloc((uint16_t)((fsize + 15) >> 4));
    if (!seg) {
        os_puts("play: Not enough conventional memory to load background track\n");
        return;
    }
    buf = (void __far *)MK_FP(seg, 0);

    bytes_read = os_file_read_offset(path, buf, (uint16_t)fsize, 0);
    if (bytes_read <= 0) {
        os_mem_free(seg);
        os_puts("play: Error reading file\n");
        return;
    }

    /* Extract basename */
    fname = path;
    {
        const char *p = path;
        while (*p) {
            if (*p == '/' || *p == '\\' || *p == ':') fname = p + 1;
            p++;
        }
    }

    if (os_sound_bg_play(rate, 1, (uint16_t)bytes_read, buf, fname) == 0) {
        os_puts("Started background playback of ");
        os_puts(fname);
        os_puts(" (");
        print_uint16(rate);
        os_puts(" Hz, looping).\n");
    } else {
        os_puts("play: Failed to start background playback\n");
    }

    os_mem_free(seg);
}

static void show_bg_status(void) {
    os_sound_bg_status_t st;
    if (os_sound_bg_status(FP_HERE(&st))) {
        os_puts("Background Music: Playing '");
        os_puts(st.name);
        os_puts("' (");
        print_uint16(st.rate_hz);
        os_puts(" Hz, ");
        print_uint16((uint16_t)st.current_pos);
        os_puts(" / ");
        print_uint16((uint16_t)st.total_bytes);
        os_puts(" bytes)\n");
    } else {
        os_puts("Background Music: Idle (no track active)\n");
    }
}

int app_main(void) {
    const char *args = os_get_args();
    char path_buf[64];
    uint16_t rate = 0;
    int is_bg = 0;
    int i = 0;

    if (args) {
        while (*args == ' ') args++;
    }

    if (!args || *args == '\0') {
        show_info();
        os_puts("\nUsage: play <file.imf|file.wav> [/bg] [/560|/700] | /stop | /status | /info | /test\n");
        return 0;
    }

    if (str_contains_icase(args, "/stop") || str_contains_icase(args, "-stop")) {
        os_sound_bg_stop();
        os_puts("Background music stopped.\n");
        return 0;
    }

    if (str_contains_icase(args, "/status") || str_contains_icase(args, "-status")) {
        show_bg_status();
        return 0;
    }

    if (str_contains_icase(args, "/bg") || str_contains_icase(args, "-bg")) {
        is_bg = 1;
    }

    if (args[0] == '/' || args[0] == '-') {
        char opt = args[1];
        if (opt == 'i' || opt == 'I') {
            show_info();
            return 0;
        } else if (opt == 't' || opt == 'T') {
            play_test();
            return 0;
        }
    }

    /* Extract file path */
    while (*args && *args != ' ' && i < 63) {
        path_buf[i++] = *args++;
    }
    path_buf[i] = '\0';

    if (str_contains_icase(path_buf, ".wav")) {
        play_wav_file(path_buf);
        return 0;
    }

    /* Check for optional rate flag for IMF */
    while (*args == ' ') args++;
    if (*args != '\0') {
        if (str_contains_icase(args, "700")) {
            rate = 700;
        } else if (str_contains_icase(args, "560")) {
            rate = 560;
        }
    }

    if (is_bg) {
        start_bg_imf(path_buf, rate);
        return 0;
    }

    play_imf_file(path_buf, rate);
    return 0;
}
