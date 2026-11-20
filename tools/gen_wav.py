#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a standard 8-bit mono uncompressed PCM .WAV audio file.
# Synthesizes a retro two-tone chime sound effect (11025 Hz).

import struct
import math
import sys

def build_chime_wav(wav_path):
    sample_rate = 11025
    duration = 0.55
    num_samples = int(sample_rate * duration)
    samples = bytearray()

    for i in range(num_samples):
        t = i / sample_rate
        if t < 0.16:
            freq = 523.25 # C5
            env = min(1.0, t / 0.015) * (1.0 - t / 0.22)
        else:
            freq = 783.99 # G5
            t2 = t - 0.16
            env = min(1.0, t2 / 0.015) * math.exp(-t2 * 4.5)

        # Sine wave with gentle 2nd harmonic
        val = 0.82 * math.sin(2.0 * math.pi * freq * t) + 0.18 * math.sin(4.0 * math.pi * freq * t)
        val = val * max(0.0, env)
        # 8-bit unsigned PCM: 128 is center, 0-255 range
        byte_val = int(128 + 120 * val)
        byte_val = max(0, min(255, byte_val))
        samples.append(byte_val)

    data_size = len(samples)
    riff_size = 36 + data_size

    header = struct.pack('<4sI4s4sIHHIIHH4sI',
        b'RIFF', riff_size, b'WAVE',
        b'fmt ', 16, 1, 1, sample_rate, sample_rate, 1, 8,
        b'data', data_size
    )
    with open(wav_path, 'wb') as f:
        f.write(header + samples)
    print(f"Generated {wav_path}: {len(header) + len(samples)} bytes ({sample_rate} Hz 8-bit PCM)")

if __name__ == '__main__':
    out_path = sys.argv[1] if len(sys.argv) > 1 else 'build/chime.wav'
    build_chime_wav(out_path)
