#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a standard IMF (Id Music Format) Type-0 song file.
# Tune: Ludwig van Beethoven - Ode to Joy (Symphony No. 9, 1824 - Public Domain)

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
        self.tick_rate = tick_rate # 560 Hz (Commander Keen rate)
        self.events = [] # list of (time_ticks, reg, val)

    def write_reg(self, time_ticks, reg, val):
        self.events.append((time_ticks, reg, val))

    def export_type0(self):
        # Sort events by time
        self.events.sort(key=lambda e: e[0])
        records = bytearray()
        cur_time = 0
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

def build_ode_to_joy(imf_path):
    builder = IMFBuilder(tick_rate=560)
    beat_ticks = int(560 * 0.45) # ~133 BPM (0.45 sec per beat = ~252 ticks)

    # Operator offsets for channels 0, 1, 2
    mod_ops = [0x00, 0x01, 0x02]
    car_ops = [0x03, 0x04, 0x05]

    t = 0
    # Enable waveform control
    builder.write_reg(t, 0x01, 0x20)

    # Initialize Instrument 0 (Lead Melody / Brass)
    m0, c0 = mod_ops[0], car_ops[0]
    builder.write_reg(t, 0x20 + m0, 0x21) # Char: Mult=1, Sustain
    builder.write_reg(t, 0x40 + m0, 0x12) # Total level mod
    builder.write_reg(t, 0x60 + m0, 0xF3) # Attack 15, Decay 3
    builder.write_reg(t, 0x80 + m0, 0x52) # Sustain 5, Release 2
    builder.write_reg(t, 0xE0 + m0, 0x00) # Wave: Sine
    builder.write_reg(t, 0x20 + c0, 0x21) # Char: Mult=1, Sustain
    builder.write_reg(t, 0x40 + c0, 0x00) # Full volume
    builder.write_reg(t, 0x60 + c0, 0xF4) # Attack 15, Decay 4
    builder.write_reg(t, 0x80 + c0, 0x44) # Sustain 4, Release 4
    builder.write_reg(t, 0xE0 + c0, 0x00)
    builder.write_reg(t, 0xC0 + 0,  0x30) # Stereo L+R on OPL3

    # Initialize Instrument 1 (Harmony / Strings)
    m1, c1 = mod_ops[1], car_ops[1]
    builder.write_reg(t, 0x20 + m1, 0x21)
    builder.write_reg(t, 0x40 + m1, 0x18)
    builder.write_reg(t, 0x60 + m1, 0x82)
    builder.write_reg(t, 0x80 + m1, 0x62)
    builder.write_reg(t, 0xE0 + m1, 0x00)
    builder.write_reg(t, 0x20 + c1, 0x21)
    builder.write_reg(t, 0x40 + c1, 0x0A) # Slightly quieter
    builder.write_reg(t, 0x60 + c1, 0x93)
    builder.write_reg(t, 0x80 + c1, 0x53)
    builder.write_reg(t, 0xE0 + c1, 0x00)
    builder.write_reg(t, 0xC0 + 1,  0x30)

    # Initialize Instrument 2 (Bass)
    m2, c2 = mod_ops[2], car_ops[2]
    builder.write_reg(t, 0x20 + m2, 0x01)
    builder.write_reg(t, 0x40 + m2, 0x14)
    builder.write_reg(t, 0x60 + m2, 0xF3)
    builder.write_reg(t, 0x80 + m2, 0x74)
    builder.write_reg(t, 0xE0 + m2, 0x00)
    builder.write_reg(t, 0x20 + c2, 0x01)
    builder.write_reg(t, 0x40 + c2, 0x06)
    builder.write_reg(t, 0x60 + c2, 0xF5)
    builder.write_reg(t, 0x80 + c2, 0x65)
    builder.write_reg(t, 0xE0 + c2, 0x00)
    builder.write_reg(t, 0xC2,       0x30)

    # Musical score: (melody note, harmony note, bass note, duration_in_beats)
    score = [
        # Phrase 1
        ('E4', 'C4', 'C3', 1.0), ('E4', 'C4', 'C3', 1.0), ('F4', 'D4', 'D3', 1.0), ('G4', 'E4', 'E3', 1.0),
        ('G4', 'E4', 'E3', 1.0), ('F4', 'D4', 'D3', 1.0), ('E4', 'C4', 'C3', 1.0), ('D4', 'B3', 'G2', 1.0),
        ('C4', 'G3', 'C3', 1.0), ('C4', 'G3', 'C3', 1.0), ('D4', 'B3', 'G2', 1.0), ('E4', 'C4', 'C3', 1.0),
        ('E4', 'C4', 'C3', 1.5), ('D4', 'B3', 'G2', 0.5), ('D4', 'B3', 'G2', 2.0),

        # Phrase 2
        ('E4', 'C4', 'C3', 1.0), ('E4', 'C4', 'C3', 1.0), ('F4', 'D4', 'D3', 1.0), ('G4', 'E4', 'E3', 1.0),
        ('G4', 'E4', 'E3', 1.0), ('F4', 'D4', 'D3', 1.0), ('E4', 'C4', 'C3', 1.0), ('D4', 'B3', 'G2', 1.0),
        ('C4', 'G3', 'C3', 1.0), ('C4', 'G3', 'C3', 1.0), ('D4', 'B3', 'G2', 1.0), ('E4', 'C4', 'C3', 1.0),
        ('D4', 'B3', 'G2', 1.5), ('C4', 'G3', 'C3', 0.5), ('C4', 'G3', 'C3', 2.0),

        # Phrase 3 (Bridge)
        ('D4', 'B3', 'G2', 1.0), ('D4', 'B3', 'G2', 1.0), ('E4', 'C4', 'C3', 1.0), ('C4', 'G3', 'E3', 1.0),
        ('D4', 'B3', 'G2', 1.0), ('E4', 'C4', 'A2', 0.5), ('F4', 'D4', 'B2', 0.5), ('E4', 'C4', 'C3', 1.0), ('C4', 'G3', 'E3', 1.0),
        ('D4', 'B3', 'G2', 1.0), ('E4', 'C4', 'A2', 0.5), ('F4', 'D4', 'B2', 0.5), ('E4', 'C4', 'C3', 1.0), ('D4', 'B3', 'G2', 1.0),
        ('C4', 'A3', 'F2', 1.0), ('D4', 'B3', 'G2', 1.0), ('G3', 'D3', 'G2', 2.0),

        # Phrase 4 (Finale)
        ('E4', 'C4', 'C3', 1.0), ('E4', 'C4', 'C3', 1.0), ('F4', 'D4', 'D3', 1.0), ('G4', 'E4', 'E3', 1.0),
        ('G4', 'E4', 'E3', 1.0), ('F4', 'D4', 'D3', 1.0), ('E4', 'C4', 'C3', 1.0), ('D4', 'B3', 'G2', 1.0),
        ('C4', 'G3', 'C3', 1.0), ('C4', 'G3', 'C3', 1.0), ('D4', 'B3', 'G2', 1.0), ('E4', 'C4', 'C3', 1.0),
        ('D4', 'B3', 'G2', 1.5), ('C4', 'G3', 'C3', 0.5), ('C4', 'G3', 'C3', 3.0),
    ]

    cur_tick = int(beat_ticks * 0.5)

    def note_on(tick, ch, note):
        f = note_to_freq(note)
        fnum, blk = freq_to_fnum_block(f)
        builder.write_reg(tick, 0xA0 + ch, fnum & 0xFF)
        builder.write_reg(tick, 0xB0 + ch, 0x20 | (blk << 2) | ((fnum >> 8) & 0x03))
        return blk, fnum

    def note_off(tick, ch, blk, fnum):
        builder.write_reg(tick, 0xB0 + ch, (blk << 2) | ((fnum >> 8) & 0x03))

    for m_note, h_note, b_note, duration_beats in score:
        dur_ticks = int(duration_beats * beat_ticks)
        gate_ticks = int(dur_ticks * 0.90) # 90% gate time, 10% articulation gap

        b0, f0 = note_on(cur_tick, 0, m_note)
        b1, f1 = note_on(cur_tick, 1, h_note)
        b2, f2 = note_on(cur_tick, 2, b_note)

        off_tick = cur_tick + gate_ticks
        note_off(off_tick, 0, b0, f0)
        note_off(off_tick, 1, b1, f1)
        note_off(off_tick, 2, b2, f2)

        cur_tick += dur_ticks

    # Final silence
    final_tick = cur_tick + beat_ticks
    for ch in range(3):
        builder.write_reg(final_tick, 0xB0 + ch, 0x00)

    data = builder.export_type0()
    with open(imf_path, 'wb') as f:
        f.write(data)
    print(f"Generated {imf_path}: {len(data)} bytes ({len(builder.events)} events)")

if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'build/ode.imf'
    build_ode_to_joy(out_path)
