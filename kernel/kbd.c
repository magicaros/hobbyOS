/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Custom PC/XT IRQ 1 (INT 0x09) hardware keyboard driver, live scancode
 * state bitmap, and loadable layout translation table (.MAP) support.
 */

#include "kernel.h"

#define KBD_BUF_SIZE 32

/* Scancode constants (PC/XT Set 1) */
#define SC_LCTRL     0x1D
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_LALT      0x38
#define SC_CAPSLOCK  0x3A
#define SC_C         0x2E
#define SC_DEL       0x53
#define SC_NUMLOCK   0x45
#define SC_SCROLLOCK 0x46

static const os_keymap_t default_us_map = {
    { 'U', 'S', '\0', '\0', '\0', '\0', '\0', '\0' },
    /* normal[128]: Unshifted US QWERTY */
    {
        /* 0x00..0x0F */
        0,    0x1B, '1',  '2',  '3',  '4',  '5',  '6',
        '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
        /* 0x10..0x1F */
        'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
        'o',  'p',  '[',  ']',  '\r', 0,    'a',  's',
        /* 0x20..0x2F */
        'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
        '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
        /* 0x30..0x3F */
        'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
        0,    ' ',  0,    0,    0,    0,    0,    0,
        /* 0x40..0x4F */
        0,    0,    0,    0,    0,    0,    0,    0,
        0,    0,    '-',  0,    0,    0,    '+',  0,
        /* 0x50..0x5F */
        0,    0,    0,    0,    0,    0,    '\\', 0,
        0,    0,    0,    0,    0,    0,    0,    0,
        /* 0x60..0x7F */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    /* shift[128]: Shifted US QWERTY (includes keypad digits on 0x47..0x53) */
    {
        /* 0x00..0x0F */
        0,    0x1B, '!',  '@',  '#',  '$',  '%',  '^',
        '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
        /* 0x10..0x1F */
        'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
        'O',  'P',  '{',  '}',  '\r', 0,    'A',  'S',
        /* 0x20..0x2F */
        'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
        '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
        /* 0x30..0x3F */
        'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
        0,    ' ',  0,    0,    0,    0,    0,    0,
        /* 0x40..0x4F */
        0,    0,    0,    0,    0,    0,    0,    '7',
        '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
        /* 0x50..0x5F */
        '2',  '3',  '0',  '.',  0,    0,    '|',  0,
        0,    0,    0,    0,    0,    0,    0,    0,
        /* 0x60..0x7F */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    /* altgr[128]: No AltGr mappings in standard US QWERTY */
    { 0 }
};

static os_keymap_t      active_map;
static volatile uint16_t kbd_buf[KBD_BUF_SIZE];
static volatile uint8_t  kbd_head    = 0;
static volatile uint8_t  kbd_tail    = 0;
static volatile uint8_t  key_down[16];

static uint8_t e0_prefix    = 0;
static uint8_t mod_lshift   = 0;
static uint8_t mod_rshift   = 0;
static uint8_t mod_ctrl     = 0;
static uint8_t mod_alt      = 0;
static uint8_t mod_altgr    = 0;
static uint8_t mod_capslock = 0;
static uint8_t mod_numlock  = 0;

static uint8_t kbd_break_mode   = BREAK_MODE_NORMAL;
static uint8_t kbd_ctrl_c_count = 0;

void kbd_set_break_mode(uint8_t mode) {
    kbd_break_mode   = mode;
    kbd_ctrl_c_count = 0;
}

uint8_t kbd_get_break_mode(void) {
    return kbd_break_mode;
}

static void sync_bda_flags(void) {
    volatile uint8_t __far *bda_flags = (volatile uint8_t __far *)MK_FP(0x0040, 0x0017);
    uint8_t flags = 0;
    if (mod_rshift)             flags |= 0x01;
    if (mod_lshift)             flags |= 0x02;
    if (mod_ctrl)               flags |= 0x04;
    if (mod_alt || mod_altgr)   flags |= 0x08;
    if (mod_numlock)            flags |= 0x20;
    if (mod_capslock)           flags |= 0x40;
    *bda_flags = flags;
}

void kbd_init(void) {
    uint16_t i;
    const uint8_t *src = (const uint8_t *)&default_us_map;
    uint8_t *dst       = (uint8_t *)&active_map;

    for (i = 0; i < sizeof(os_keymap_t); i++) {
        dst[i] = src[i];
    }
    for (i = 0; i < 16; i++) {
        key_down[i] = 0;
    }

    kbd_head     = 0;
    kbd_tail     = 0;
    e0_prefix    = 0;
    mod_lshift   = 0;
    mod_rshift   = 0;
    mod_ctrl     = 0;
    mod_alt      = 0;
    mod_altgr    = 0;
    mod_capslock = 0;
    mod_numlock  = 0;
    kbd_break_mode   = BREAK_MODE_NORMAL;
    kbd_ctrl_c_count = 0;
    sync_bda_flags();

    kbd_install_vector();
}

uint8_t kbd_irq_handler(uint8_t raw) {
    uint8_t sc;
    uint8_t released;
    uint8_t ext;
    uint8_t ascii = 0;
    uint8_t next_head;

    if (raw == 0xE0) {
        e0_prefix = 1;
        return 0;
    }
    if (raw >= 0xE1) {
        e0_prefix = 0;
        return 0;
    }

    sc       = (uint8_t)(raw & 0x7F);
    released = (uint8_t)((raw & 0x80) != 0);
    ext      = e0_prefix;
    e0_prefix = 0;

    /* Ignore the fake Shift codes (E0 2A / E0 36) 101-key keyboards send around grey keys */
    if (ext && (sc == SC_LSHIFT || sc == SC_RSHIFT)) {
        return 0;
    }

    /* Update live 128-key press state bitmap */
    if (released) {
        key_down[sc >> 3] &= (uint8_t)~(1 << (sc & 7));
    } else {
        key_down[sc >> 3] |= (uint8_t)(1 << (sc & 7));
    }

    /* Update modifier states */
    if (sc == SC_LSHIFT) {
        mod_lshift = (uint8_t)!released;
        sync_bda_flags();
        return 0;
    }
    if (sc == SC_RSHIFT) {
        mod_rshift = (uint8_t)!released;
        sync_bda_flags();
        return 0;
    }
    if (sc == SC_LCTRL) {
        mod_ctrl = (uint8_t)!released;
        sync_bda_flags();
        return 0;
    }
    if (sc == SC_LALT) {
        if (ext) {
            mod_altgr = (uint8_t)!released;
        } else {
            mod_alt = (uint8_t)!released;
        }
        sync_bda_flags();
        return 0;
    }
    if (sc == SC_CAPSLOCK) {
        if (!released) {
            mod_capslock ^= 1;
            sync_bda_flags();
        }
        return 0;
    }
    if (sc == SC_NUMLOCK) {
        if (!released) {
            mod_numlock ^= 1;
            sync_bda_flags();
        }
        return 0;
    }
    if (sc == SC_SCROLLOCK) {
        return 0;
    }

    /* Only enqueue character events on key press (make code) */
    if (released) {
        return 0;
    }

    /* 1. Check for CTRL+ALT+DEL warm reboot */
    if (sc == SC_DEL && mod_ctrl && (mod_alt || mod_altgr)) {
        cpu_reboot();
        return 0;
    }

    /* 2. Check for CTRL+C break */
    if (sc == SC_C && mod_ctrl && !(mod_alt || mod_altgr)) {
        if (get_usermode_active() > 1) {
            if (kbd_break_mode == BREAK_MODE_NORMAL) {
                con_puts("^C\n");
                return 1; /* Abort child process */
            } else {
                /* BREAK_MODE_RAW: 1st Ctrl+C delivered to app; 2nd in a row breaks out */
                kbd_ctrl_c_count++;
                if (kbd_ctrl_c_count >= 2) {
                    kbd_ctrl_c_count = 0;
                    con_puts("^C\n");
                    return 1; /* Abort child process */
                }
                ascii = 0x03;
            }
        } else {
            /* Root shell or kernel prompt: queue ASCII 0x03 */
            ascii = 0x03;
        }
    } else {
        /* Any other key resets the consecutive Ctrl+C counter */
        kbd_ctrl_c_count = 0;
    }

    if (ascii == 0) {
        /* 1. AltGr / Alt layer (supports E0 RAlt, Ctrl+Alt, and XT Alt) */
        if ((mod_altgr || mod_alt) && active_map.altgr[sc] != 0) {
            ascii = active_map.altgr[sc];
        }
        /* 2. Ctrl + letter -> ASCII control code 0x01..0x1A */
        else if (mod_ctrl && !(mod_altgr || mod_alt)) {
            uint8_t base = active_map.normal[sc];
            if (base >= 'a' && base <= 'z') {
                ascii = (uint8_t)(base - 'a' + 1);
            } else if (base >= 'A' && base <= 'Z') {
                ascii = (uint8_t)(base - 'A' + 1);
            }
        }
        /* 3. Standard Normal / Shift / CapsLock / NumLock layers */
        else {
            uint8_t shift_active = (uint8_t)(mod_lshift || mod_rshift);
            uint8_t use_shift;

            /* Numpad cluster (0x47..0x53 except '-' 0x4A and '+' 0x4E) */
            if (!ext && sc >= 0x47 && sc <= 0x53 && sc != 0x4A && sc != 0x4E) {
                use_shift = (uint8_t)(shift_active ^ mod_numlock);
            }
            /* Alphabetic keys invert shift when CapsLock is active */
            else if (active_map.normal[sc] >= 'a' && active_map.normal[sc] <= 'z') {
                use_shift = (uint8_t)(shift_active ^ mod_capslock);
            } else {
                use_shift = shift_active;
            }

            ascii = use_shift ? active_map.shift[sc] : active_map.normal[sc];
        }
    }

    next_head = (uint8_t)((kbd_head + 1) & (KBD_BUF_SIZE - 1));
    if (next_head != kbd_tail) {
        kbd_buf[kbd_head] = ((uint16_t)sc << 8) | ascii;
        kbd_head = next_head;
    }
    return 0;
}

int kbd_kbhit(void) {
    return kbd_head != kbd_tail;
}

uint16_t kbd_getc(void) {
    uint16_t key;
    while (kbd_head == kbd_tail) {
        if (ctrl_c_abort_pending && get_usermode_active() > 1) {
            ctrl_c_abort_pending = 0;
            proc_exit_usermode(130);
        }
        cpu_halt();
    }
    if (ctrl_c_abort_pending && get_usermode_active() > 1) {
        ctrl_c_abort_pending = 0;
        proc_exit_usermode(130);
    }
    key = kbd_buf[kbd_tail];
    kbd_tail = (uint8_t)((kbd_tail + 1) & (KBD_BUF_SIZE - 1));
    return key;
}

int kbd_set_map(const os_keymap_t __far *map) {
    uint16_t i;
    const uint8_t __far *src = (const uint8_t __far *)map;
    uint8_t *dst             = (uint8_t *)&active_map;

    if (!map || map->name[0] == '\0') {
        return -1;
    }
    for (i = 0; i < sizeof(os_keymap_t); i++) {
        dst[i] = src[i];
    }
    active_map.name[7] = '\0';
    return 0;
}

int kbd_get_name(char __far *out_name) {
    uint16_t i;
    if (!out_name) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        out_name[i] = active_map.name[i];
    }
    out_name[7] = '\0';
    return 0;
}

int kbd_get_state(uint8_t scancode) {
    uint8_t sc = (uint8_t)(scancode & 0x7F);
    return (key_down[sc >> 3] & (1 << (sc & 7))) ? 1 : 0;
}
