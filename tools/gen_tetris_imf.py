#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a standard IMF (Id Music Format) Type-0 song file.
# Tune: Korobeiniki (19th-century Russian Folk Song / Tetris Theme - Public Domain)

import struct
import sys

def note_to_freq(note_name):
    if not note_name or note_name == 'REST':
        return 0.0
    notes = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
    name = note_name[:-1]
    octave = int(note_name[-1])
    semitone = notes.index(name)
    midi = 12 + octave * 12 + semitone
    return 440.0 * (2.0 ** ((midi - 69) / 12.0))

def freq_to_fnum_block(freq):
    if freq <= 0:
        return 0, 0
    for block in range(8):
        fnum = int((freq * (1 << (20 - block))) / 49716.0 + 0.5)
        if fnum < 1024:
            return fnum, block
    return 1023, 7

class IMFBuilder:
    def __init__(self, tick_rate=560):
        self.tick_rate = tick_rate # 560 Hz
        self.events = []

    def write_reg(self, time_ticks, reg, val):
        self.events.append((time_ticks, reg, val))

    def export_type0(self):
        self.events.sort(key=lambda e: e[0])
        records = bytearray()
        for i, (ev_time, reg, val) in enumerate(self.events):
            if i + 1 < len(self.events):
                next_time = self.events[i + 1][0]
                delay = int(next_time - ev_time)
                if delay < 0: delay = 0
                if delay > 65535: delay = 65535
            else:
                delay = 0
            records += struct.pack('<BBH', reg, val, delay)
        return bytes(records)

def build_tetris_theme(imf_path):
    builder = IMFBuilder(tick_rate=560)
    # Fast tempo ~144 BPM (0.416s per beat = ~233 ticks at 560 Hz)
    beat_ticks = int(560 * 0.40)

    mod_ops = [0x00, 0x01, 0x02]
    car_ops = [0x03, 0x04, 0x05]

    t = 0
    # Enable waveform control
    builder.write_reg(t, 0x01, 0x20)

    # Lead (Bright Chiptune Lead)
    m0, c0 = mod_ops[0], car_ops[0]
    builder.write_reg(t, 0x20 + m0, 0x21) # Mult=1, Sustain
    builder.write_reg(t, 0x40 + m0, 0x10) # Modulator level
    builder.write_reg(t, 0x60 + m0, 0xF2) # Fast attack, decay
    builder.write_reg(t, 0x80 + m0, 0x62)
    builder.write_reg(t, 0xE0 + m0, 0x00)
    builder.write_reg(t, 0x20 + c0, 0x21)
    builder.write_reg(t, 0x40 + c0, 0x00) # Full carrier volume
    builder.write_reg(t, 0x60 + c0, 0xF3)
    builder.write_reg(t, 0x80 + c0, 0x63)
    builder.write_reg(t, 0xE0 + c0, 0x00)
    builder.write_reg(t, 0xC0 + 0,  0x30) # Stereo center

    # Bass (Punchy FM Bass)
    m1, c1 = mod_ops[1], car_ops[1]
    builder.write_reg(t, 0x20 + m1, 0x01)
    builder.write_reg(t, 0x40 + m1, 0x12)
    builder.write_reg(t, 0x60 + m1, 0xF4)
    builder.write_reg(t, 0x80 + m1, 0x74)
    builder.write_reg(t, 0xE0 + m1, 0x00)
    builder.write_reg(t, 0x20 + c1, 0x01)
    builder.write_reg(t, 0x40 + c1, 0x04)
    builder.write_reg(t, 0x60 + c1, 0xF5)
    builder.write_reg(t, 0x80 + c1, 0x65)
    builder.write_reg(t, 0xE0 + c1, 0x00)
    builder.write_reg(t, 0xC0 + 1,  0x30)

    # Musical score: (melody note, bass note, duration_in_beats)
    score = [
        # Part 1
        ('E5', 'A2', 1.0), ('B4', 'E3', 0.5), ('C5', 'A3', 0.5),
        ('D5', 'D3', 1.0), ('C5', 'A3', 0.5), ('B4', 'E3', 0.5),
        ('A4', 'A2', 1.0), ('A4', 'E3', 0.5), ('C5', 'A3', 0.5),
        ('E5', 'C3', 1.0), ('D5', 'G3', 0.5), ('C5', 'C3', 0.5),
        ('B4', 'E2', 1.5), ('C5', 'B2', 0.5),
        ('D5', 'E3', 1.0), ('E5', 'G#3', 1.0),
        ('C5', 'A2', 1.0), ('A4', 'E3', 1.0),
        ('A4', 'A2', 1.5), ('REST', 'REST', 0.5),

        # Part 2
        ('D5', 'D3', 1.0), ('F5', 'A3', 0.5), ('A5', 'D4', 0.5),
        ('G5', 'G3', 1.0), ('F5', 'B3', 0.5), ('E5', 'G3', 0.5),
        ('C5', 'C3', 1.5), ('E5', 'G3', 0.5),
        ('D5', 'F3', 0.5), ('C5', 'D3', 0.5), ('B4', 'E3', 1.0),
        ('B4', 'E2', 0.5), ('C5', 'G#2', 0.5), ('D5', 'B2', 1.0),
        ('E5', 'E3', 1.0),
        ('C5', 'A2', 1.0), ('A4', 'E3', 1.0),
        ('A4', 'A2', 1.5), ('REST', 'REST', 0.5),
    ]

    cur_tick = int(beat_ticks * 0.25)

    def note_on(tick, ch, note):
        f = note_to_freq(note)
        fnum, blk = freq_to_fnum_block(f)
        builder.write_reg(tick, 0xA0 + ch, fnum & 0xFF)
        builder.write_reg(tick, 0xB0 + ch, 0x20 | (blk << 2) | ((fnum >> 8) & 0x03))

    def note_off(tick, ch):
        builder.write_reg(tick, 0xB0 + ch, 0x00)

    for mel, bass, dur in score:
        dur_ticks = int(dur * beat_ticks)
        gate_ticks = int(dur_ticks * 0.88)

        if mel != 'REST':
            note_on(cur_tick, 0, mel)
            note_off(cur_tick + gate_ticks, 0)
        if bass != 'REST':
            note_on(cur_tick, 1, bass)
            note_off(cur_tick + gate_ticks, 1)

        cur_tick += dur_ticks

    # Silence all channels at the end before loop
    note_off(cur_tick, 0)
    note_off(cur_tick, 1)

    data = builder.export_type0()
    with open(imf_path, 'wb') as f:
        f.write(data)
    print(f"Generated {imf_path}: {len(data)} bytes ({len(builder.events)} events)")

if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'build/tetris.imf'
    build_tetris_theme(out_path)
