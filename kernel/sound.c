/*
 * Copyright (C) 2026 Benoit Colson
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Unified Sound Subsystem for hobbyOS:
 * - Yamaha YMF262 (OPL3) & YM3812 (OPL2) FM Synthesizer at 0x388..0x38B
 * - Universal PC Speaker fallback (PIT Timer 2 / Port 0x61) with real-time
 *   OPL frequency interception (automatic AdLib-to-PC-Speaker emulation).
 */

#include "kernel.h"

#define OPL_PORT_BASE     0x0388
#define OPL_PORT_STATUS   0x0388
#define OPL_PORT_REG      0x0388
#define OPL_PORT_DATA     0x0389
#define OPL_PORT_REG2     0x038A
#define OPL_PORT_DATA2    0x038B

static uint8_t  s_sound_dev       = SOUND_DEV_NONE;
static uint16_t s_sound_port      = 0;
static uint16_t s_sound_channels  = 0;

/* PC Speaker fallback tracking across 9 OPL channels */
static uint8_t  s_opl_fnum_lo[9];
static uint8_t  s_opl_key_on[9];
static uint16_t s_opl_freq[9];
static int8_t   s_active_spk_ch   = -1;

/* AdLib standard hardware delays:
 * 6 status port reads after register select (~3.3 us)
 * 35 status port reads after data write (~23 us)
 */
static void opl_delay_reg(void) {
    uint8_t i;
    for (i = 0; i < 6; i++) {
        inb(OPL_PORT_STATUS);
    }
}

static void opl_delay_data(void) {
    uint8_t i;
    for (i = 0; i < 35; i++) {
        inb(OPL_PORT_STATUS);
    }
}

static void opl_raw_write(uint16_t reg, uint8_t val) {
    if (reg & 0x0100) {
        /* Bank 1 (OPL3 second set of 9 channels) */
        outb(OPL_PORT_REG2, (uint8_t)(reg & 0xFF));
        opl_delay_reg();
        outb(OPL_PORT_DATA2, val);
        opl_delay_data();
    } else {
        /* Bank 0 (standard OPL2 / first 9 channels) */
        outb(OPL_PORT_REG, (uint8_t)(reg & 0xFF));
        opl_delay_reg();
        outb(OPL_PORT_DATA, val);
        opl_delay_data();
    }
}

/* Divides PIT clock 1,193,180 (0x001234DC) by freq without compiler 32-bit div helper */
static uint16_t calc_pit_divisor(uint16_t freq) {
    uint16_t q;
    if (freq < 19) return 0xFFFF;
    __asm {
        mov dx, 0x0012
        mov ax, 0x34DC
        mov bx, freq
        div bx
        mov q, ax
    }
    return q;
}

static void speaker_tone(uint16_t freq_hz) {
    if (freq_hz == 0) {
        /* Mute speaker: disable timer 2 gate and speaker data */
        outb(0x61, (uint8_t)(inb(0x61) & ~0x03));
    } else {
        uint16_t div = calc_pit_divisor(freq_hz);
        outb(0x43, 0xB6); /* Timer 2, Mode 3 (square wave), LSB then MSB, binary */
        outb(0x42, (uint8_t)(div & 0xFF));
        outb(0x42, (uint8_t)(div >> 8));
        /* Enable timer 2 gate and speaker data */
        outb(0x61, (uint8_t)(inb(0x61) | 0x03));
    }
}

/* Converts OPL F-number (0..1023) and block (0..7) to frequency in Hz without 32-bit division */
static uint16_t opl_to_freq(uint16_t fnum, uint8_t blk) {
    uint16_t freq;
    uint8_t shift = (uint8_t)(20 - (blk & 0x07));
    __asm {
        mov ax, fnum
        mov bx, 49716
        mul bx          ; DX:AX = fnum * 49716
        mov cl, shift
        cmp cl, 16
        jae .shift_high
        ; shift < 16: (DX << (16 - shift)) | (AX >> shift)
        mov ch, 16
        sub ch, cl
        mov bx, dx
        xchg cl, ch
        shl bx, cl
        xchg cl, ch
        shr ax, cl
        or ax, bx
        mov freq, ax
        jmp .done
    .shift_high:
        ; shift >= 16: DX >> (shift - 16)
        sub cl, 16
        shr dx, cl
        mov freq, dx
    .done:
    }
    return freq;
}

/* Converts frequency in Hz to OPL F-number and block */
static void freq_to_opl(uint16_t freq, uint16_t *out_fnum, uint8_t *out_blk) {
    uint8_t blk;
    for (blk = 0; blk < 8; blk++) {
        uint16_t fnum;
        uint8_t shift = (uint8_t)(20 - blk);
        __asm {
            mov ax, freq
            xor dx, dx
            mov cl, shift
            cmp cl, 16
            jae .hi_shift
            ; shift < 16: DX:AX = freq << shift
            mov ch, 16
            sub ch, cl
            mov dx, ax
            xchg cl, ch
            shr dx, cl
            xchg cl, ch
            shl ax, cl
            jmp .do_div
        .hi_shift:
            ; shift >= 16: DX:AX = (freq << (shift - 16)) : 0
            sub cl, 16
            shl ax, cl
            mov dx, ax
            xor ax, ax
        .do_div:
            mov bx, 49716
            div bx
            mov fnum, ax
        }
        if (fnum < 1024) {
            *out_fnum = fnum;
            *out_blk = blk;
            return;
        }
    }
    *out_fnum = 1023;
    *out_blk = 7;
}

static int probe_opl(void) {
    uint8_t stat1, stat2;
    uint16_t i;

    /* Step 1: Reset Timer 1 and Timer 2 (reg 0x04 = 0x60) */
    outb(OPL_PORT_REG, 0x04);
    opl_delay_reg();
    outb(OPL_PORT_DATA, 0x60);
    opl_delay_data();

    /* Step 2: Reset IRQ flag (reg 0x04 = 0x80) */
    outb(OPL_PORT_REG, 0x04);
    opl_delay_reg();
    outb(OPL_PORT_DATA, 0x80);
    opl_delay_data();

    /* Step 3: Read status register (should have timer bits clear) */
    stat1 = inb(OPL_PORT_STATUS) & 0xE0;

    /* Step 4: Write 0xFF to Timer 1 count register (reg 0x02) */
    outb(OPL_PORT_REG, 0x02);
    opl_delay_reg();
    outb(OPL_PORT_DATA, 0xFF);
    opl_delay_data();

    /* Step 5: Start Timer 1 (unmask bit 6, enable bit 0 -> reg 0x04 = 0x21) */
    outb(OPL_PORT_REG, 0x04);
    opl_delay_reg();
    outb(OPL_PORT_DATA, 0x21);

    /* Wait ~100 us for Timer 1 to overflow (ticks every 80 us at 3.58 MHz) */
    for (i = 0; i < 200; i++) {
        inb(OPL_PORT_STATUS);
    }

    /* Step 6: Read status register (bit 7 IRQ and bit 6 Timer 1 should be set: 0xC0) */
    stat2 = inb(OPL_PORT_STATUS) & 0xE0;

    /* Step 7: Reset timers to clean up */
    outb(OPL_PORT_REG, 0x04);
    opl_delay_reg();
    outb(OPL_PORT_DATA, 0x60);
    opl_delay_data();

    if (stat1 == 0x00 && stat2 == 0xC0) {
        /* OPL FM chip confirmed present! Now check if OPL3 (YMF262) */
        uint8_t stat_opl3 = inb(OPL_PORT_STATUS) & 0x06;
        if (stat_opl3 == 0x00) {
            /* Enable OPL3 extensions (write 1 to Bank 1 register 0x05) */
            outb(OPL_PORT_REG2, 0x05);
            opl_delay_reg();
            outb(OPL_PORT_DATA2, 0x01);
            opl_delay_data();
            return SOUND_DEV_OPL3;
        }
        return SOUND_DEV_OPL2;
    }

    return SOUND_DEV_NONE;
}

static void init_opl_channel(uint8_t ch) {
    uint8_t mod_op = (ch < 3) ? ch : ((ch < 6) ? (ch + 5) : (ch + 10));
    uint8_t car_op = mod_op + 3;

    /* Standard mellow brass / piano instrument */
    opl_raw_write(0x20 + mod_op, 0x21); /* Multiplier=1, Sustain */
    opl_raw_write(0x40 + mod_op, 0x14); /* Modulation level */
    opl_raw_write(0x60 + mod_op, 0xF3); /* Attack 15, Decay 3 */
    opl_raw_write(0x80 + mod_op, 0x53); /* Sustain 5, Release 3 */
    opl_raw_write(0xE0 + mod_op, 0x00); /* Sine wave */

    opl_raw_write(0x20 + car_op, 0x21);
    opl_raw_write(0x40 + car_op, 0x00); /* Carrier full volume */
    opl_raw_write(0x60 + car_op, 0xF4); /* Attack 15, Decay 4 */
    opl_raw_write(0x80 + car_op, 0x44); /* Sustain 4, Release 4 */
    opl_raw_write(0xE0 + car_op, 0x00);

    opl_raw_write(0xC0 + ch, 0x30);     /* Stereo Center on OPL3, FM mode */
}

static uint16_t s_sb_base       = 0;
static uint8_t  s_sb_type       = SB_TYPE_NONE;
static uint8_t  s_sb_dsp_major  = 0;
static uint8_t  s_sb_dsp_minor  = 0;

static int sb_dsp_reset(uint16_t base) {
    uint16_t i;
    outb(base + 0x06, 1);
    for (i = 0; i < 50; i++) inb(0x61);
    outb(base + 0x06, 0);

    for (i = 0; i < 1000; i++) {
        if (inb(base + 0x0E) & 0x80) {
            if (inb(base + 0x0A) == 0xAA) {
                return 1;
            }
        }
    }
    return 0;
}

static int sb_dsp_write(uint16_t base, uint8_t val) {
    uint16_t i;
    for (i = 0; i < 5000; i++) {
        if ((inb(base + 0x0C) & 0x80) == 0) {
            outb(base + 0x0C, val);
            return 1;
        }
    }
    return 0;
}

static int sb_dsp_read(uint16_t base, uint8_t *val) {
    uint16_t i;
    for (i = 0; i < 5000; i++) {
        if (inb(base + 0x0E) & 0x80) {
            *val = inb(base + 0x0A);
            return 1;
        }
    }
    return 0;
}

static void probe_sb(void) {
    static const uint16_t bases[2] = { 0x0220, 0x0240 };
    uint8_t b;

    s_sb_base = 0;
    s_sb_type = SB_TYPE_NONE;
    s_sb_dsp_major = 0;
    s_sb_dsp_minor = 0;

    for (b = 0; b < 2; b++) {
        uint16_t base = bases[b];
        if (sb_dsp_reset(base)) {
            s_sb_base = base;
            if (sb_dsp_write(base, 0xE1)) {
                uint8_t maj = 0, min = 0;
                if (sb_dsp_read(base, &maj) && sb_dsp_read(base, &min)) {
                    s_sb_dsp_major = maj;
                    s_sb_dsp_minor = min;
                    if (maj >= 4) {
                        s_sb_type = SB_TYPE_16;
                    } else if (maj == 3) {
                        s_sb_type = SB_TYPE_PRO;
                    } else if (maj == 2) {
                        s_sb_type = SB_TYPE_SB20;
                    } else {
                        s_sb_type = SB_TYPE_SB10;
                    }
                    return;
                }
            }
            s_sb_type = SB_TYPE_SB10;
            return;
        }
    }
}

static void sb_mixer_write(uint8_t reg, uint8_t val) {
    if (!s_sb_base) return;
    outb(s_sb_base + 0x04, reg);
    outb(s_sb_base + 0x05, val);
}

static uint8_t sb_mixer_read(uint8_t reg) {
    if (!s_sb_base) return 0;
    outb(s_sb_base + 0x04, reg);
    return inb(s_sb_base + 0x05);
}

uint8_t sound_get_sb_type(void) {
    return s_sb_type;
}

uint16_t sound_get_sb_base(void) {
    return s_sb_base;
}

const char *sound_get_sb_name(void) {
    switch (s_sb_type) {
    case SB_TYPE_16:  return "Sound Blaster 16 (CT1745 Mixer)";
    case SB_TYPE_PRO: return "Sound Blaster Pro (CT1345 Mixer)";
    case SB_TYPE_SB20: return "Sound Blaster 2.0";
    case SB_TYPE_SB10: return "Sound Blaster 1.0/1.5";
    default:          return "None";
    }
}

int16_t sound_get_volume(uint8_t channel) {
    if (s_sb_type < SB_TYPE_PRO) return -1;

    if (s_sb_type == SB_TYPE_16) {
        uint8_t reg = 0;
        uint8_t raw;
        uint16_t pct;
        if (channel == SOUND_VOL_MASTER) reg = 0x30;
        else if (channel == SOUND_VOL_FM) reg = 0x34;
        else if (channel == SOUND_VOL_PCM) reg = 0x32;
        else return -1;

        raw = (sb_mixer_read(reg) >> 3) & 0x1F;
        pct = ((uint16_t)raw * 100 + 15) / 31;
        return (int16_t)pct;
    } else {
        uint8_t reg = 0;
        uint8_t raw;
        uint16_t pct;
        if (channel == SOUND_VOL_MASTER) reg = 0x22;
        else if (channel == SOUND_VOL_FM) reg = 0x26;
        else if (channel == SOUND_VOL_PCM) reg = 0x04;
        else return -1;

        raw = (sb_mixer_read(reg) >> 5) & 0x07;
        pct = ((uint16_t)raw * 100 + 3) / 7;
        return (int16_t)pct;
    }
}

int16_t sound_set_volume(uint8_t channel, uint8_t pct) {
    if (s_sb_type < SB_TYPE_PRO) return -1;
    if (pct > 100) pct = 100;

    if (s_sb_type == SB_TYPE_16) {
        uint8_t step = (uint8_t)(((uint16_t)pct * 31 + 50) / 100);
        uint8_t reg_l = 0, reg_r = 0;
        uint8_t val;
        if (step > 31) step = 31;
        val = (uint8_t)(step << 3);

        if (channel == SOUND_VOL_MASTER) {
            reg_l = 0x30; reg_r = 0x31;
        } else if (channel == SOUND_VOL_FM) {
            reg_l = 0x34; reg_r = 0x35;
        } else if (channel == SOUND_VOL_PCM) {
            reg_l = 0x32; reg_r = 0x33;
        } else {
            return -1;
        }

        sb_mixer_write(reg_l, val);
        sb_mixer_write(reg_r, val);
        return 0;
    } else {
        uint8_t step = (uint8_t)(((uint16_t)pct * 7 + 50) / 100);
        uint8_t reg = 0;
        uint8_t val;
        if (step > 7) step = 7;
        val = (uint8_t)((step << 5) | (step << 1));

        if (channel == SOUND_VOL_MASTER) reg = 0x22;
        else if (channel == SOUND_VOL_FM) reg = 0x26;
        else if (channel == SOUND_VOL_PCM) reg = 0x04;
        else return -1;

        sb_mixer_write(reg, val);
        return 0;
    }
}

int sound_init(void) {
    int opl_type;
    uint8_t i;

    s_sound_dev      = SOUND_DEV_NONE;
    s_sound_port     = 0;
    s_sound_channels = 0;
    s_active_spk_ch  = -1;
    for (i = 0; i < 9; i++) {
        s_opl_fnum_lo[i] = 0;
        s_opl_key_on[i]  = 0;
        s_opl_freq[i]    = 0;
    }

    /* Ensure speaker starts in silent state */
    speaker_tone(0);

    /* Probe for Sound Blaster DSP & Mixer */
    probe_sb();
    if (s_sb_type >= SB_TYPE_PRO) {
        sound_set_volume(SOUND_VOL_MASTER, 80);
        sound_set_volume(SOUND_VOL_FM, 80);
        sound_set_volume(SOUND_VOL_PCM, 80);
    }

    /* Probe for Yamaha OPL2 / OPL3 at 0x388 */
    opl_type = probe_opl();
    if (opl_type == SOUND_DEV_OPL3) {
        s_sound_dev      = SOUND_DEV_OPL3;
        s_sound_port     = OPL_PORT_BASE;
        s_sound_channels = 18;
        /* Enable waveform control on OPL */
        opl_raw_write(0x01, 0x20);
        sound_stop();
        init_opl_channel(0);
        return 1;
    } else if (opl_type == SOUND_DEV_OPL2) {
        s_sound_dev      = SOUND_DEV_OPL2;
        s_sound_port     = OPL_PORT_BASE;
        s_sound_channels = 9;
        opl_raw_write(0x01, 0x20);
        sound_stop();
        init_opl_channel(0);
        return 1;
    }

    /* Fallback to universal PC Speaker */
    s_sound_dev      = SOUND_DEV_SPEAKER;
    s_sound_port     = 0x42;
    s_sound_channels = 1;
    return 1;
}

uint8_t sound_get_device(void) {
    return s_sound_dev;
}

const char *sound_get_device_name(void) {
    switch (s_sound_dev) {
    case SOUND_DEV_OPL3:    return "Yamaha YMF262 (OPL3) at 0x388 (18 channels stereo)";
    case SOUND_DEV_OPL2:    return "Yamaha YM3812 (OPL2) at 0x388 (9 channels mono)";
    case SOUND_DEV_SPEAKER: return "PC Speaker fallback (PIT Timer 2 / Port 0x61)";
    default:                return "None / Muted";
    }
}

uint16_t sound_get_port(void) {
    return s_sound_port;
}

uint16_t sound_get_channels(void) {
    return s_sound_channels;
}

#define PCM_DMA_BUF_SIZE 4096
static uint8_t  s_pcm_dma_buf[PCM_DMA_BUF_SIZE];
static uint8_t  s_pcm_playing = 0;

static uint8_t calc_time_constant(uint16_t rate) {
    uint16_t div_val;
    if (rate < 4000) rate = 4000;
    if (rate > 22050) rate = 22050;
    __asm {
        mov dx, 0x000F
        mov ax, 0x4240
        mov bx, rate
        div bx
        mov div_val, ax
    }
    if (div_val >= 256) return 0;
    return (uint8_t)(256 - div_val);
}

static void dma1_setup(uint32_t phys_addr, uint16_t length) {
    uint16_t count = length - 1;
    uint8_t page = (uint8_t)(phys_addr >> 16);
    uint16_t offset = (uint16_t)(phys_addr & 0xFFFF);

    outb(0x0A, 0x05); /* Mask channel 1 */
    outb(0x0C, 0x00); /* Clear flip-flop */
    outb(0x0B, 0x49); /* Mode: Single cycle, Read transfer (mem->DAC), Ch 1 */

    outb(0x02, (uint8_t)(offset & 0xFF));
    outb(0x02, (uint8_t)(offset >> 8));

    outb(0x83, page); /* Page register for Channel 1 */

    outb(0x03, (uint8_t)(count & 0xFF));
    outb(0x03, (uint8_t)(count >> 8));

    outb(0x0A, 0x01); /* Unmask channel 1 */
}

static int dma1_is_done(void) {
    /* Read DMA status port 0x08: bit 1 is TC (Terminal Count) for Channel 1 */
    return (inb(0x08) & 0x02) != 0;
}

int16_t sound_pcm_play(uint16_t rate_hz, uint16_t len, const uint8_t __far *samples) {
    uint16_t i;
    uint8_t tc;
    uint32_t phys;

    if (s_sb_type == SB_TYPE_NONE || !s_sb_base) return -1;
    if (len == 0 || len > PCM_DMA_BUF_SIZE) return -2;

    /* Copy samples into kernel DMA buffer */
    for (i = 0; i < len; i++) {
        s_pcm_dma_buf[i] = samples[i];
    }

    tc = calc_time_constant(rate_hz);

    /* Setup DMA Channel 1 */
    phys = ((uint32_t)KERNEL_SEG << 4) + (uint16_t)(void *)s_pcm_dma_buf;
    dma1_setup(phys, len);

    /* Enable speaker */
    sb_dsp_write(s_sb_base, 0xD1);

    /* Set sample rate time constant */
    sb_dsp_write(s_sb_base, 0x40);
    sb_dsp_write(s_sb_base, tc);

    /* Start single-cycle 8-bit DMA output */
    sb_dsp_write(s_sb_base, 0x14);
    sb_dsp_write(s_sb_base, (uint8_t)((len - 1) & 0xFF));
    sb_dsp_write(s_sb_base, (uint8_t)((len - 1) >> 8));

    s_pcm_playing = 1;
    return 0;
}

int16_t sound_pcm_status(void) {
    if (!s_pcm_playing) return 0;
    if (dma1_is_done()) {
        /* Acknowledge 8-bit DSP interrupt by reading port 0x22E */
        if (s_sb_base) {
            inb(s_sb_base + 0x0E);
        }
        s_pcm_playing = 0;
        return 0;
    }
    return 1;
}

void sound_pcm_stop(void) {
    if (s_sb_base && s_pcm_playing) {
        /* Pause 8-bit DMA */
        sb_dsp_write(s_sb_base, 0xD0);
        /* Mask DMA channel 1 */
        outb(0x0A, 0x05);
        inb(s_sb_base + 0x0E);
    }
    s_pcm_playing = 0;
}

/* Background Music Playback state */
static uint16_t s_bg_seg          = 0;
static uint32_t s_bg_len          = 0;
static uint32_t s_bg_offset       = 0;
static uint16_t s_bg_start_offset = 0;
static uint16_t s_bg_delay_left   = 0;
static uint16_t s_bg_rate         = 0;
static uint8_t  s_bg_active       = 0;
static uint8_t  s_bg_loop         = 1;
static char     s_bg_name[16]     = "";

void sound_bg_tick(void) {
    if (!s_bg_active || !s_bg_seg) return;

    if (s_bg_delay_left > 0) {
        s_bg_delay_left--;
        if (s_bg_delay_left > 0) return;
    }

    /* Process all records with delay == 0 */
    while (s_bg_delay_left == 0 && s_bg_offset + 4 <= s_bg_len) {
        uint8_t __far *p = (uint8_t __far *)MK_FP(s_bg_seg, (uint16_t)s_bg_offset);
        uint8_t  reg   = p[0];
        uint8_t  val   = p[1];
        uint16_t delay = (uint16_t)p[2] | ((uint16_t)p[3] << 8);

        sound_opl_write((uint16_t)reg, val);
        s_bg_offset += 4;

        if (delay > 0) {
            s_bg_delay_left = delay;
            break;
        }
    }

    if (s_bg_offset + 4 > s_bg_len) {
        if (s_bg_loop) {
            s_bg_offset = s_bg_start_offset;
            s_bg_delay_left = 0;
        } else {
            sound_bg_stop();
        }
    }
}

int16_t sound_bg_play(uint16_t rate_hz, uint8_t loop, uint16_t len, const uint8_t __far *data, const char __far *name) {
    uint16_t paras;
    uint16_t seg;
    uint16_t i;
    uint16_t divisor;

    if (len == 0 || !data) return -1;
    if (rate_hz != 700) rate_hz = 560;

    /* Stop any active background playback */
    sound_bg_stop();

    /* Allocate kernel MCB buffer */
    paras = (len + 15) >> 4;
    seg = mem_alloc(paras, KERNEL_SEG);
    if (!seg) return -1;

    /* Copy IMF data into buffer */
    for (i = 0; i < len; i++) {
        uint8_t __far *dst = (uint8_t __far *)MK_FP(seg, i);
        *dst = data[i];
    }

    /* Check for Type-1 IMF header (2-byte length prefix) */
    s_bg_start_offset = 0;
    if (len >= 4) {
        uint8_t __far *p = (uint8_t __far *)MK_FP(seg, 0);
        uint16_t pfx = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        if (pfx != 0 && pfx == (uint16_t)(len - 2)) {
            s_bg_start_offset = 2;
        }
    }

    s_bg_seg        = seg;
    s_bg_len        = len;
    s_bg_offset     = s_bg_start_offset;
    s_bg_delay_left = 0;
    s_bg_rate       = rate_hz;
    s_bg_loop       = loop;
    s_bg_active     = 1;

    /* Copy track name */
    for (i = 0; i < 15 && name && name[i] != '\0'; i++) {
        s_bg_name[i] = name[i];
    }
    s_bg_name[i] = '\0';

    /* Standard AdLib/OPL initialization */
    sound_opl_write(0x01, 0x20); /* Enable waveform control */
    sound_opl_write(0x08, 0x00); /* Disable speech mode */
    sound_opl_write(0xBD, 0x00); /* Disable rhythm mode */

    /* Divisor for 8253 PIT: 1,193,180 / rate */
    divisor = (rate_hz == 700) ? 1705 : 2131;
    sound_timer_install(divisor);

    return 0;
}

void sound_bg_stop(void) {
    if (s_bg_active) {
        s_bg_active = 0;
        sound_timer_restore();
        speaker_tone(0);
        s_active_spk_ch = -1;
        if (s_sound_dev == SOUND_DEV_OPL2 || s_sound_dev == SOUND_DEV_OPL3) {
            uint8_t ch;
            for (ch = 0; ch < 9; ch++) {
                opl_raw_write(0xB0 + ch, 0x00);
            }
            if (s_sound_dev == SOUND_DEV_OPL3) {
                for (ch = 0; ch < 9; ch++) {
                    opl_raw_write(0x01B0 + ch, 0x00);
                }
            }
        }
    }
    if (s_bg_seg) {
        mem_free(s_bg_seg);
        s_bg_seg = 0;
    }
    s_bg_len = 0;
    s_bg_offset = 0;
    s_bg_delay_left = 0;
    s_bg_name[0] = '\0';
}

int16_t sound_bg_get_status(os_sound_bg_status_t __far *st) {
    uint8_t i;
    if (!st) return (int16_t)s_bg_active;
    st->active = s_bg_active;
    st->loop = s_bg_loop;
    st->rate_hz = s_bg_rate;
    st->current_pos = s_bg_offset;
    st->total_bytes = s_bg_len;
    for (i = 0; i < 15; i++) {
        st->name[i] = s_bg_name[i];
    }
    st->name[15] = '\0';
    return (int16_t)s_bg_active;
}

void sound_stop(void) {
    uint8_t ch;
    sound_pcm_stop();

    /* If background music is actively playing, preserve OPL channels */
    if (s_bg_active) return;

    speaker_tone(0);
    s_active_spk_ch = -1;
    for (ch = 0; ch < 9; ch++) {
        s_opl_key_on[ch] = 0;
    }

    if (s_sound_dev == SOUND_DEV_OPL2 || s_sound_dev == SOUND_DEV_OPL3) {
        /* Turn off key-on on all Bank 0 channels */
        for (ch = 0; ch < 9; ch++) {
            opl_raw_write(0xB0 + ch, 0x00);
        }
        if (s_sound_dev == SOUND_DEV_OPL3) {
            /* Turn off key-on on all Bank 1 channels */
            for (ch = 0; ch < 9; ch++) {
                opl_raw_write(0x01B0 + ch, 0x00);
            }
        }
    }
}

void sound_opl_write(uint16_t reg, uint8_t val) {
    if (s_sound_dev == SOUND_DEV_OPL2 || s_sound_dev == SOUND_DEV_OPL3) {
        if (s_sound_dev == SOUND_DEV_OPL3) {
            /* If writing to C0..C8 (or 1C0..1C8) on OPL3 and stereo output bits (4-5) are 0,
             * default to Center (both Left and Right enabled, val |= 0x30).
             * Vintage OPL2 software/IMF tracks leave bits 4-5 zero, which silences OPL3!
             */
            uint8_t r8 = (uint8_t)(reg & 0xFF);
            if (r8 >= 0xC0 && r8 <= 0xC8) {
                if ((val & 0x30) == 0) {
                    val |= 0x30;
                }
            }
        }
        opl_raw_write(reg, val);
    } else if (s_sound_dev == SOUND_DEV_SPEAKER) {
        /* AdLib-to-PC-Speaker emulation across all 9 channels */
        if (reg >= 0xA0 && reg <= 0xA8) {
            s_opl_fnum_lo[reg - 0xA0] = val;
        } else if (reg >= 0xB0 && reg <= 0xB8) {
            uint8_t ch = (uint8_t)(reg - 0xB0);
            if (val & 0x20) {
                /* Key-On */
                uint16_t fnum = ((uint16_t)(val & 0x03) << 8) | s_opl_fnum_lo[ch];
                uint8_t blk = (val >> 2) & 0x07;
                uint16_t freq = opl_to_freq(fnum, blk);
                s_opl_freq[ch] = freq;
                s_opl_key_on[ch] = 1;
                s_active_spk_ch = (int8_t)ch;
                speaker_tone(freq);
            } else {
                /* Key-Off */
                s_opl_key_on[ch] = 0;
                if (s_active_spk_ch == (int8_t)ch) {
                    int8_t i;
                    s_active_spk_ch = -1;
                    for (i = 8; i >= 0; i--) {
                        if (s_opl_key_on[i]) {
                            s_active_spk_ch = i;
                            speaker_tone(s_opl_freq[i]);
                            break;
                        }
                    }
                    if (s_active_spk_ch == -1) {
                        speaker_tone(0);
                    }
                }
            }
        }
    }
}

void sound_tone(uint16_t freq_hz, uint16_t duration_ms, uint8_t channel) {
    volatile uint16_t __far *bda_ticks = (volatile uint16_t __far *)MK_FP(0x0040, 0x006C);

    if (freq_hz == 0) {
        sound_stop();
        return;
    }

    if (s_sound_dev == SOUND_DEV_OPL2 || s_sound_dev == SOUND_DEV_OPL3) {
        uint16_t fnum;
        uint8_t blk;
        uint8_t ch = channel % 9;
        uint16_t bank = (channel >= 9 && s_sound_dev == SOUND_DEV_OPL3) ? 0x0100 : 0x0000;

        freq_to_opl(freq_hz, &fnum, &blk);
        opl_raw_write(bank | (0xA0 + ch), (uint8_t)(fnum & 0xFF));
        opl_raw_write(bank | (0xB0 + ch), (uint8_t)(0x20 | (blk << 2) | ((fnum >> 8) & 0x03)));

        if (duration_ms > 0) {
            /* 1 tick = ~55 ms */
            uint16_t ticks = (duration_ms + 27) / 55;
            uint16_t start = *bda_ticks;
            if (ticks == 0) ticks = 1;
            while ((uint16_t)(*bda_ticks - start) < ticks);
            opl_raw_write(bank | (0xB0 + ch), (uint8_t)((blk << 2) | ((fnum >> 8) & 0x03)));
        }
    } else {
        /* PC Speaker fallback */
        speaker_tone(freq_hz);
        if (duration_ms > 0) {
            uint16_t ticks = (duration_ms + 27) / 55;
            uint16_t start = *bda_ticks;
            if (ticks == 0) ticks = 1;
            while ((uint16_t)(*bda_ticks - start) < ticks);
            speaker_tone(0);
        }
    }
}
