/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mouse driver and INT 0x33 subsystem for hobbyOS.
 * Supports PS/2 auxiliary pointing devices and Serial mice (Microsoft 2/3-button
 * and Mouse Systems 3-button protocols) across COM1..COM4 with auto-probing.
 */

#include "kernel.h"

static uint8_t  s_detected       = 0;
static uint8_t  s_mouse_type     = MOUSE_TYPE_NONE;
static uint8_t  s_buttons        = 0;
static uint16_t s_port           = 0;
static uint8_t  s_irq            = 0;

static uint16_t s_x              = 320;
static uint16_t s_y              = 100;
static uint16_t s_min_x          = 0;
static uint16_t s_max_x          = 639;
static uint16_t s_min_y          = 0;
static uint16_t s_max_y          = 199;
static uint16_t s_btn_status     = 0;
static int16_t  s_cursor_visible = -1; /* < 0: hidden, 0: visible */
static int16_t  s_mickeys_x      = 0;
static int16_t  s_mickeys_y      = 0;

/* Text-mode cursor state */
static uint16_t s_last_cell_idx  = 0xFFFF;
static uint16_t s_last_cell_val  = 0;

/* Packet parser state */
static uint8_t  s_pkt_buf[8];
static uint8_t  s_pkt_idx        = 0;
static uint8_t  s_active_proto   = 0; /* 1: MS 3-byte, 2: Mouse Systems 5-byte, 3: PS/2 3-byte */

static void mouse_erase_cursor(void) {
    if (s_last_cell_idx != 0xFFFF) {
        con_set_cell(s_last_cell_idx, s_last_cell_val);
        s_last_cell_idx = 0xFFFF;
    }
}

static void mouse_render_cursor(void) {
    uint16_t col = s_x >> 3; /* 640 -> 80 */
    uint16_t row = s_y >> 3; /* 200 -> 25 */
    uint16_t idx;
    uint16_t val;
    uint16_t inv_val;

    if (col >= 80) col = 79;
    if (row >= 25) row = 24;
    idx = row * 80 + col;

    if (idx == s_last_cell_idx) {
        return;
    }

    mouse_erase_cursor();

    /* Read cell from console shadow buffer */
    val = con_get_cell(idx);
    s_last_cell_idx = idx;
    s_last_cell_val = val;

    /* Invert cell attribute (XOR attribute byte with 0x77) */
    inv_val = (val & 0x00FF) | (((val >> 8) ^ 0x77) << 8);
    con_set_cell(idx, inv_val);
}

static void update_coords(int16_t dx, int16_t dy, uint16_t btns) {
    int32_t nx = (int32_t)s_x + (int32_t)dx;
    int32_t ny = (int32_t)s_y + (int32_t)dy;

    if (nx < (int32_t)s_min_x) nx = (int32_t)s_min_x;
    if (nx > (int32_t)s_max_x) nx = (int32_t)s_max_x;
    if (ny < (int32_t)s_min_y) ny = (int32_t)s_min_y;
    if (ny > (int32_t)s_max_y) ny = (int32_t)s_max_y;

    s_x = (uint16_t)nx;
    s_y = (uint16_t)ny;
    s_btn_status = btns;
    s_mickeys_x += dx;
    s_mickeys_y += dy;

    if (s_cursor_visible >= 0) {
        mouse_render_cursor();
    }
}

/* Dual serial packet parser (Microsoft & Mouse Systems) */
static void parse_serial_byte(uint8_t b) {
    if (s_pkt_idx == 0) {
        if ((b & 0x40) != 0) {
            /* Microsoft protocol sync header: Bit 6 is always 1 */
            s_active_proto = 1;
            s_pkt_buf[0] = b;
            s_pkt_idx = 1;
            return;
        } else if ((b & 0xF8) == 0x80) {
            /* Mouse Systems protocol sync header: 1000 0xxx */
            s_active_proto = 2;
            s_pkt_buf[0] = b;
            s_pkt_idx = 1;
            return;
        }
        return; /* Out of sync byte, ignore */
    }

    s_pkt_buf[s_pkt_idx++] = b;

    if (s_active_proto == 1 && s_pkt_idx == 3) {
        /* Microsoft 3-byte packet complete */
        uint8_t b0 = s_pkt_buf[0];
        uint8_t b1 = s_pkt_buf[1];
        uint8_t b2 = s_pkt_buf[2];
        int8_t  dx = (int8_t)(((b0 & 0x03) << 6) | (b1 & 0x3F));
        int8_t  dy = (int8_t)(((b0 & 0x0C) << 4) | (b2 & 0x3F));
        uint16_t btns = 0;

        if (b0 & 0x20) btns |= 0x01; /* Left button */
        if (b0 & 0x10) btns |= 0x02; /* Right button */

        s_pkt_idx = 0;
        update_coords((int16_t)dx, (int16_t)dy, btns);
    } else if (s_active_proto == 2 && s_pkt_idx == 5) {
        /* Mouse Systems 5-byte packet complete */
        uint8_t b0  = s_pkt_buf[0];
        int8_t  dx1 = (int8_t)s_pkt_buf[1];
        int8_t  dy1 = -(int8_t)s_pkt_buf[2]; /* Inverted Y in Mouse Systems */
        int8_t  dx2 = (int8_t)s_pkt_buf[3];
        int8_t  dy2 = -(int8_t)s_pkt_buf[4]; /* Inverted Y in Mouse Systems */
        int16_t dx  = (int16_t)dx1 + (int16_t)dx2;
        int16_t dy  = (int16_t)dy1 + (int16_t)dy2;
        uint16_t btns = 0;

        if ((b0 & 0x04) == 0) btns |= 0x01; /* Left button (active low) */
        if ((b0 & 0x01) == 0) btns |= 0x02; /* Right button (active low) */
        if ((b0 & 0x02) == 0) btns |= 0x04; /* Middle button (active low) */

        s_pkt_idx = 0;
        update_coords(dx, dy, btns);
    } else if (s_pkt_idx >= 5) {
        s_pkt_idx = 0;
    }
}

/* PS/2 packet parser */
static void parse_ps2_byte(uint8_t b) {
    if (s_pkt_idx == 0) {
        if ((b & 0x08) == 0) {
            return; /* Bit 3 must be 1 in first byte of PS/2 packet */
        }
    }
    s_pkt_buf[s_pkt_idx++] = b;
    if (s_pkt_idx == 3) {
        uint8_t  b0 = s_pkt_buf[0];
        int16_t  dx = (int16_t)s_pkt_buf[1];
        int16_t  dy = (int16_t)s_pkt_buf[2];
        uint16_t btns = 0;

        if (b0 & 0x10) dx |= (int16_t)0xFF00; /* Sign extend X */
        if (b0 & 0x20) dy |= (int16_t)0xFF00; /* Sign extend Y */
        dy = -dy; /* PS/2 Y is positive upwards */

        if (b0 & 0x01) btns |= 0x01; /* Left */
        if (b0 & 0x02) btns |= 0x02; /* Right */
        if (b0 & 0x04) btns |= 0x04; /* Middle */

        s_pkt_idx = 0;
        update_coords(dx, dy, btns);
    }
}

void mouse_irq_handler(void) {
    if (!s_detected) {
        return;
    }

    if (s_mouse_type == MOUSE_TYPE_PS2) {
        while (inb(0x64) & 0x01) {
            uint8_t b = inb(0x60);
            parse_ps2_byte(b);
        }
    } else if (s_port != 0) {
        while (inb(s_port + 5) & 0x01) {
            uint8_t b = inb(s_port + 0);
            parse_serial_byte(b);
        }
    }
}

void mouse_dispatch(syscall_regs_t __far *regs) {
    switch (regs->ax) {
    case 0x0000: /* Reset & Detect */
        if (s_detected) {
            regs->ax = 0xFFFF;
            regs->bx = s_buttons;
            mouse_erase_cursor();
            s_x = 320;
            s_y = 100;
            s_min_x = 0;
            s_max_x = 639;
            s_min_y = 0;
            s_max_y = 199;
            s_cursor_visible = -1;
            s_mickeys_x = 0;
            s_mickeys_y = 0;
        } else {
            regs->ax = 0x0000;
            regs->bx = 0;
        }
        break;

    case 0x0001: /* Show Cursor */
        s_cursor_visible++;
        if (s_cursor_visible == 0) {
            mouse_render_cursor();
        }
        break;

    case 0x0002: /* Hide Cursor */
        if (s_cursor_visible == 0) {
            mouse_erase_cursor();
        }
        s_cursor_visible--;
        break;

    case 0x0003: /* Get Status */
        regs->bx = s_btn_status;
        regs->cx = s_x;
        regs->dx = s_y;
        break;

    case 0x0004: /* Set Position */
        s_x = regs->cx;
        s_y = regs->dx;
        if (s_x < s_min_x) s_x = s_min_x;
        if (s_x > s_max_x) s_x = s_max_x;
        if (s_y < s_min_y) s_y = s_min_y;
        if (s_y > s_max_y) s_y = s_max_y;
        if (s_cursor_visible >= 0) {
            mouse_render_cursor();
        }
        break;

    case 0x0007: /* Set Horizontal Limits */
        s_min_x = regs->cx;
        s_max_x = regs->dx;
        if (s_x < s_min_x) s_x = s_min_x;
        if (s_x > s_max_x) s_x = s_max_x;
        break;

    case 0x0008: /* Set Vertical Limits */
        s_min_y = regs->cx;
        s_max_y = regs->dx;
        if (s_y < s_min_y) s_y = s_min_y;
        if (s_y > s_max_y) s_y = s_max_y;
        break;

    case 0x000B: /* Get Motion Counters (mickeys) */
        regs->cx = (uint16_t)s_mickeys_x;
        regs->dx = (uint16_t)s_mickeys_y;
        s_mickeys_x = 0;
        s_mickeys_y = 0;
        break;

    default:
        break;
    }
}

void mouse_handle_ps2_packet(uint16_t status, int16_t raw_x, int16_t raw_y) {
    int16_t dx, dy;
    uint16_t btns = 0;

    if (!s_detected) {
        return;
    }

    /* PS/2 status byte:
     * Bit 0: Left button
     * Bit 1: Right button
     * Bit 2: Middle button
     * Bit 4: X sign bit (1 = negative)
     * Bit 5: Y sign bit (1 = negative)
     */
    if (status & 0x10) {
        dx = (int16_t)(raw_x | (int16_t)0xFF00);
    } else {
        dx = (int16_t)(raw_x & 0x00FF);
    }

    if (status & 0x20) {
        dy = (int16_t)(raw_y | (int16_t)0xFF00);
    } else {
        dy = (int16_t)(raw_y & 0x00FF);
    }

    /* Screen coordinates: Y increases downward, PS/2 reports upward */
    dy = -dy;

    if (status & 0x01) btns |= 0x01; /* Left */
    if (status & 0x02) btns |= 0x02; /* Right */
    if (status & 0x04) btns |= 0x04; /* Middle */

    update_coords(dx, dy, btns);
}

static int probe_ps2(void) {
    uint8_t dev_id = 0;

    /* PS/2 pointing devices and BIOS INT 15h (C2xx) require at least an 80286 */
    if (cpu_get_type() < CPU_TYPE_80286) {
        return 0;
    }

    /* Step 1: Set BIOS device handler callback (INT 15h, AX=0xC207) */
    if (bios_ps2_set_handler() != 0) {
        return 0; /* BIOS does not support PS/2 pointing device */
    }

    /* Step 2: Initialize pointing device (packet size = 3 bytes, INT 15h, AX=0xC205) */
    if (bios_ps2_init(3) != 0) {
        bios_ps2_reset_handler();
        return 0;
    }

    /* Step 3: Query device ID (optional, INT 15h, AX=0xC204) */
    if (bios_ps2_get_id(&dev_id) != 0) {
        dev_id = 0;
    }

    /* Step 4: Enable pointing device interface (INT 15h, AX=0xC200) */
    if (bios_ps2_enable(1) != 0) {
        bios_ps2_reset_handler();
        return 0;
    }

    s_detected   = 1;
    s_mouse_type = MOUSE_TYPE_PS2;
    s_buttons    = (dev_id == 0x03) ? 3 : 2;
    s_port       = 0x60;
    s_irq        = 12;
    mouse_install_vectors(0); /* Installs INT 0x33 vector */
    return 1;
}

static int probe_serial(void) {
    volatile uint16_t __far *bda_com   = (volatile uint16_t __far *)MK_FP(0x0040, 0x0000);
    volatile uint16_t __far *bda_ticks = (volatile uint16_t __far *)MK_FP(0x0040, 0x006C);
    uint16_t ports[4];
    uint8_t  irqs[4];
    uint8_t  num_ports = 0;
    uint8_t  i;

    /* Read discovered COM ports from BIOS Data Area */
    for (i = 0; i < 4; i++) {
        uint16_t p = bda_com[i];
        if (p != 0) {
            ports[num_ports] = p;
            irqs[num_ports] = (i == 0 || i == 2) ? 4 : 3;
            num_ports++;
        }
    }

    /* Fallback to standard ISA ports if BDA table is empty */
    if (num_ports == 0) {
        ports[0] = 0x3F8; irqs[0] = 4;
        ports[1] = 0x2F8; irqs[1] = 3;
        ports[2] = 0x3E8; irqs[2] = 4;
        ports[3] = 0x2E8; irqs[3] = 3;
        num_ports = 4;
    }

    for (i = 0; i < num_ports; i++) {
        uint16_t p = ports[i];
        uint8_t  irq = irqs[i];
        uint8_t  old_ier;
        uint8_t  old_lcr;
        uint8_t  old_div_lo = 0x0C;
        uint8_t  old_div_hi = 0x00;
        uint8_t  old_mcr;
        uint16_t timeout;
        uint16_t start_tick;
        uint8_t  ms_id = 0;

        /* Verify UART presence via IER bit pattern */
        old_ier = inb(p + 1);
        outb(p + 1, 0x00);
        if ((inb(p + 1) & 0xFF) != 0x00) continue;
        outb(p + 1, 0x0F);
        if ((inb(p + 1) & 0xFF) != 0x0F) continue;
        outb(p + 1, old_ier);

        /* Save original UART divisor and line control */
        old_lcr = inb(p + 3);
        outb(p + 3, old_lcr | 0x80); /* Enable DLAB to read divisor */
        old_div_lo = inb(p + 0);
        old_div_hi = inb(p + 1);
        outb(p + 3, old_lcr & ~0x80);
        old_mcr = inb(p + 4);

        /* Configure 1200 baud, 8N1 */
        outb(p + 1, 0x00); /* Disable interrupts */
        outb(p + 3, 0x80); /* Enable DLAB */
        outb(p + 0, 0x60); /* 1200 baud divisor (115200 / 1200 = 96 = 0x0060) */
        outb(p + 1, 0x00);
        outb(p + 3, 0x03); /* 8N1 */

        /* Flush receive buffer */
        for (timeout = 0; timeout < 100; timeout++) {
            if (inb(p + 5) & 0x01) {
                inb(p + 0);
            }
        }

        /* Toggle DTR/RTS to reset connected mouse (power-down pulse ~100ms) */
        outb(p + 4, 0x00); /* DTR=0, RTS=0 */
        start_tick = *bda_ticks;
        for (timeout = 0; timeout < 20000; timeout++) {
            if ((uint16_t)(*bda_ticks - start_tick) >= 2) break;
            inb(0x61);
        }

        /* Power-up mouse: DTR=1, RTS=1, OUT2=1 (enables IRQ line to PIC) */
        outb(p + 4, 0x0B);

        /* Wait up to ~220ms (4 timer ticks) for Microsoft mouse identification byte 'M' */
        start_tick = *bda_ticks;
        for (timeout = 0; timeout < 40000; timeout++) {
            if (inb(p + 5) & 0x01) {
                ms_id = inb(p + 0);
                break;
            }
            if ((uint16_t)(*bda_ticks - start_tick) >= 4) {
                break;
            }
            inb(0x61);
        }

        if (ms_id == 'M') {
            /* Microsoft 2/3-button serial mouse detected */
            uint8_t btn = 2;
            start_tick = *bda_ticks;
            for (timeout = 0; timeout < 20000; timeout++) {
                if (inb(p + 5) & 0x01) {
                    uint8_t id2 = inb(p + 0);
                    if (id2 == '3' || id2 == 'B') btn = 3;
                    break;
                }
                if ((uint16_t)(*bda_ticks - start_tick) >= 2) {
                    break;
                }
                inb(0x61);
            }
            s_detected   = 1;
            s_mouse_type = MOUSE_TYPE_SERIAL_MS;
            s_buttons    = btn;
            s_port       = p;
            s_irq        = irq;
            outb(p + 1, 0x01); /* Enable Received Data Available IRQ */
            mouse_install_vectors(irq);
            if (p == COM1_BASE) {
                serial_disable(); /* Disable console text mirroring to mouse port */
            }
            return 1;
        }

        /* No mouse detected on this port - restore original UART configuration */
        outb(p + 1, 0x00);
        outb(p + 3, 0x80); /* Enable DLAB */
        outb(p + 0, old_div_lo);
        outb(p + 1, old_div_hi);
        outb(p + 3, old_lcr);
        outb(p + 4, old_mcr);
        outb(p + 1, old_ier);
    }
    return 0;
}

int mouse_init(void) {
    s_detected       = 0;
    s_mouse_type     = MOUSE_TYPE_NONE;
    s_buttons        = 0;
    s_port           = 0;
    s_irq            = 0;
    s_cursor_visible = -1;
    s_last_cell_idx  = 0xFFFF;
    s_pkt_idx        = 0;

    /* Probe PS/2 first, then serial ports COM1..COM4 */
    if (probe_ps2() || probe_serial()) {
        return 1;
    }

    /* Even if mouse not found, install dummy INT 0x33 vector */
    mouse_install_vectors(0);
    return 0;
}

uint8_t mouse_is_detected(void) {
    return s_detected;
}

uint8_t mouse_get_type(void) {
    return s_mouse_type;
}

uint8_t mouse_get_buttons(void) {
    return s_buttons;
}

uint16_t mouse_get_port(void) {
    return s_port;
}

uint8_t mouse_get_irq(void) {
    return s_irq;
}

const char *mouse_get_type_name(void) {
    switch (s_mouse_type) {
    case MOUSE_TYPE_PS2:              return "PS/2 pointing device";
    case MOUSE_TYPE_SERIAL_MS:        return "Microsoft serial mouse";
    case MOUSE_TYPE_SERIAL_MSSYSTEMS: return "Mouse Systems 3-button serial mouse";
    default:                          return "Not detected";
    }
}
