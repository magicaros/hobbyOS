/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Volume and Mixer control utility (/BIN/VOLUME.BIN).
 */

#include "hobbyos.h"

static void print_uint(uint16_t val) {
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

static void print_bar(int pct) {
    int i;
    int filled;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    filled = (pct * 20) / 100;

    os_putc('[');
    for (i = 0; i < 20; i++) {
        if (i < filled) os_putc('#');
        else os_putc('.');
    }
    os_puts("] ");
    print_uint((uint16_t)pct);
    os_puts("%\n");
}

static int parse_int(const char **pstr) {
    int val = 0;
    const char *s = *pstr;
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    *pstr = s;
    return val;
}

static int str_equal_icase(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += ('a' - 'A');
        if (c2 >= 'A' && c2 <= 'Z') c2 += ('a' - 'A');
        if (c1 != c2) return 0;
        s1++;
        s2++;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

static void show_status(void) {
    uint16_t port, channels, sb_base;
    uint8_t sb_type;
    int master, fm, pcm;

    os_sound_info_ex(&port, &channels, &sb_type, &sb_base);

    if (sb_type < SB_TYPE_PRO) {
        os_puts("hobbyOS Volume Control:\n");
        os_puts("  Sound Hardware: ");
        os_puts(os_sound_sb_name(sb_type));
        os_puts("\n  Mixer Hardware: Not present (fixed audio output)\n");
        return;
    }

    master = os_sound_get_vol(SOUND_VOL_MASTER);
    fm     = os_sound_get_vol(SOUND_VOL_FM);
    pcm    = os_sound_get_vol(SOUND_VOL_PCM);

    os_puts("hobbyOS Volume Control (");
    os_puts(os_sound_sb_name(sb_type));
    os_puts("):\n");

    os_puts("  Master: ");
    print_bar(master);

    os_puts("  FM:     ");
    print_bar(fm);

    os_puts("  PCM:    ");
    print_bar(pcm);

    os_puts("\nUsage: volume [master|fm|pcm] <0-100> | volume <0-100>\n");
}

int app_main(void) {
    const char *args = os_get_args();
    char word[32];
    int wlen = 0;
    int val;

    if (args) {
        while (*args == ' ') args++;
    }

    if (!args || *args == '\0') {
        show_status();
        return 0;
    }

    if (args[0] == '/' || args[0] == '-') {
        if (args[1] == '?' || args[1] == 'h' || args[1] == 'H') {
            os_puts("Usage:\n");
            os_puts("  volume                 Show current volume levels\n");
            os_puts("  volume <0-100>         Set Master, FM, and PCM volume\n");
            os_puts("  volume master <0-100>  Set Master volume\n");
            os_puts("  volume fm <0-100>      Set FM / OPL synthesizer volume\n");
            os_puts("  volume pcm <0-100>     Set Voice / PCM digital audio volume\n");
            return 0;
        }
    }

    /* Check if first token is a number */
    if (args[0] >= '0' && args[0] <= '9') {
        val = parse_int(&args);
        if (val < 0 || val > 100) {
            os_puts("volume: Value must be between 0 and 100\n");
            return 1;
        }
        if (os_sound_set_vol(SOUND_VOL_MASTER, (uint8_t)val) < 0) {
            os_puts("volume: Mixer hardware not available\n");
            return 1;
        }
        os_sound_set_vol(SOUND_VOL_FM, (uint8_t)val);
        os_sound_set_vol(SOUND_VOL_PCM, (uint8_t)val);
        val = os_sound_get_vol(SOUND_VOL_MASTER);
        os_puts("Volume set to ");
        print_uint((uint16_t)val);
        os_puts("% (Master, FM, PCM)\n");
        return 0;
    }

    /* Read first token (channel name) */
    while (*args && *args != ' ' && wlen < 31) {
        word[wlen++] = *args++;
    }
    word[wlen] = '\0';

    while (*args == ' ') args++;
    val = parse_int(&args);
    if (val < 0 || val > 100) {
        os_puts("volume: Value must be between 0 and 100\n");
        return 1;
    }

    if (str_equal_icase(word, "master")) {
        if (os_sound_set_vol(SOUND_VOL_MASTER, (uint8_t)val) < 0) {
            os_puts("volume: Mixer hardware not available\n");
            return 1;
        }
        val = os_sound_get_vol(SOUND_VOL_MASTER);
        os_puts("Master volume set to ");
        print_uint((uint16_t)val);
        os_puts("%\n");
    } else if (str_equal_icase(word, "fm")) {
        if (os_sound_set_vol(SOUND_VOL_FM, (uint8_t)val) < 0) {
            os_puts("volume: Mixer hardware not available\n");
            return 1;
        }
        val = os_sound_get_vol(SOUND_VOL_FM);
        os_puts("FM volume set to ");
        print_uint((uint16_t)val);
        os_puts("%\n");
    } else if (str_equal_icase(word, "pcm") || str_equal_icase(word, "voice")) {
        if (os_sound_set_vol(SOUND_VOL_PCM, (uint8_t)val) < 0) {
            os_puts("volume: Mixer hardware not available\n");
            return 1;
        }
        val = os_sound_get_vol(SOUND_VOL_PCM);
        os_puts("PCM volume set to ");
        print_uint((uint16_t)val);
        os_puts("%\n");
    } else {
        os_puts("volume: Unknown channel '");
        os_puts(word);
        os_puts("'. Expected: master, fm, or pcm\n");
        return 1;
    }

    return 0;
}
