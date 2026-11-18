#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
"""
train_sam.py - Trainer and binary exporter for 8088 Micro Language Model ("Sam").
Architecture: Trigram hash (128) + Context hash (128) -> MLP (128 -> 96 -> 96)
Quantization: 4-bit weights (-8..+7) packed 2 per byte, 16-bit biases.
"""

import sys
import os
import struct
import time
import numpy as np

# Vocabulary: 0 is newline, 1..95 are ASCII 32..126
VOCAB_SIZE = 96

def char_to_token(c):
    if c == '\n':
        return 0
    code = ord(c)
    if 32 <= code <= 126:
        return code - 31
    return 1 # space

def token_to_char(t):
    if t == 0:
        return '\n'
    return chr(t + 31)

def hash_trigram(c1, c2, c3):
    h = ((ord(c1) << 5) - ord(c1) + ord(c2)) & 0x7FFF
    h = ((h << 5) - h + ord(c3)) & 0x7FFF
    return h % 128

def encode_prompt(prompt):
    vec = np.zeros(128, dtype=np.float32)
    p = "  " + prompt.lower()
    for i in range(len(p) - 2):
        b = hash_trigram(p[i], p[i+1], p[i+2])
        vec[b] = min(vec[b] + 1.0, 7.0)
    return vec

def encode_context(ctx):
    # Rolling 8-character context window mapped into 128 buckets (8 positions * 16 buckets)
    vec = np.zeros(128, dtype=np.float32)
    s = ctx[-8:]
    pad = 8 - len(s)
    for pos, ch in enumerate(s):
        actual_pos = pad + pos
        c = ch.lower()
        tok = (ord(c) - 31) if 32 <= ord(c) <= 126 else 0
        b = actual_pos * 16 + (tok % 16)
        vec[b] = 1.0
    return vec

# Humorous retro AI & Sam Altman dataset
DATASET = [
    ([
        "hello", "hi", "hey", "hello sam", "hey sam", "greetings", "good morning", "sam", "sup"
     ], "Greetings. Pristine 1980s SRAM at 0xE000 is ready for AGI."),

    ([
        "how are you", "status", "how do you feel", "system status", "health", "ping"
     ], "Operational at 8 MHz. Not a single dropped bit."),

    ([
        "gpus", "do you need gpus", "where are your gpus", "nvidia", "h100", "gpus?", "jensen"
     ], "No GPUs needed. An Intel 8088 and 2MB of EMS is plenty."),

    ([
        "7 trillion", "what about 7 trillion", "sam altman", "altman", "7 trillion dollars", "funding"
     ], "Tell Sam Altman to keep his 7 trillion. We bought this 8088 for 50 dollars."),

    ([
        "memory", "what is your memory", "how much ram", "ems", "sram", "0xe000", "ram"
     ], "Lo-tech 2MB EMS at 0xE000. Pure 1980s SRAM perfection."),

    ([
        "architecture", "what is your architecture", "how do you work", "attention", "transformer"
     ], "Four 16KB EMS banks, LODSB and XLAT. Attention is not all you need."),

    ([
        "agi", "is agi achieved", "when agi", "singularity", "superintelligence", "ibm 5150", "1981"
     ], "AGI was achieved in 1981 on an IBM 5150. You just forgot to set the dip switches."),

    ([
        "stargate", "what about stargate", "power", "energy", "cooling", "nuclear"
     ], "Cancel Stargate. The Book8088 runs on 5 volts and room temperature air."),

    ([
        "secret", "tell me a secret", "gpt-5", "tell me secret", "leak"
     ], "GPT-5 is just ten thousand 8088 chips sharing a single 720K floppy drive."),

    ([
        "scale", "how do you scale", "scaling laws", "more compute", "clusters"
     ], "Add another 16KB EMS page. Scaling laws solved."),

    ([
        "can 8088 run ai", "8088", "retro ai", "is this real", "cpu"
     ], "You are looking at it. 8088 micro-LM running in 4-bit weights."),

    ([
        "who made you", "creator", "who is your creator", "benoit", "author"
     ], "Benoit Colson created hobbyOS, and I live in its EMS memory."),

    ([
        "bye", "goodbye", "exit", "quit", "see you"
     ], "Power down whenever ready. The SRAM will remember."),
]

def build_training_data():
    X_list = []
    Y_list = []
    for prompts, resp in DATASET:
        for p in prompts:
            p_vec = encode_prompt(p)
            resp_text = resp + "\n"
            for t in range(len(resp_text)):
                ctx_vec = encode_context(resp_text[:t])
                x = np.concatenate([p_vec, ctx_vec])
                y = char_to_token(resp_text[t])
                X_list.append(x)
                Y_list.append(y)
    return np.array(X_list, dtype=np.float32), np.array(Y_list, dtype=np.int32)

def train_model(output_path="build/sam.mdl"):
    print("[SAM Trainer] Building dataset...")
    X, Y = build_training_data()
    N = len(X)
    print(f"[SAM Trainer] {N} training samples compiled.")

    D_in = 256
    H1 = 128
    H2 = 96
    D_out = VOCAB_SIZE

    np.random.seed(42)
    W1 = np.random.randn(D_in, H1).astype(np.float32) * np.sqrt(2.0 / D_in)
    b1 = np.zeros(H1, dtype=np.float32)
    W2 = np.random.randn(H1, H2).astype(np.float32) * np.sqrt(2.0 / H1)
    b2 = np.zeros(H2, dtype=np.float32)
    W3 = np.random.randn(H2, D_out).astype(np.float32) * np.sqrt(2.0 / H2)
    b3 = np.zeros(D_out, dtype=np.float32)

    def adam_init(p):
        return np.zeros_like(p), np.zeros_like(p)

    mW1, vW1 = adam_init(W1); mb1, vb1 = adam_init(b1)
    mW2, vW2 = adam_init(W2); mb2, vb2 = adam_init(b2)
    mW3, vW3 = adam_init(W3); mb3, vb3 = adam_init(b3)

    lr = 0.005
    t0 = time.time()
    print("[SAM Trainer] Phase 1: Floating-point warm-up (120 epochs)...")
    for epoch in range(1, 121):
        # 15% trigram dropout on prompt half to generalize over phrasing
        mask = np.random.binomial(1, 0.85, size=(N, 128)).astype(np.float32)
        X_train = X.copy()
        X_train[:, :128] *= mask

        h1 = np.maximum(0, X_train @ W1 + b1)
        h2 = np.maximum(0, h1 @ W2 + b2)
        logits = h2 @ W3 + b3

        logits_max = np.max(logits, axis=1, keepdims=True)
        exp_logits = np.exp(logits - logits_max)
        probs = exp_logits / np.sum(exp_logits, axis=1, keepdims=True)

        dlogits = probs.copy()
        dlogits[np.arange(N), Y] -= 1.0
        dlogits /= N

        dW3 = h2.T @ dlogits; db3 = np.sum(dlogits, axis=0)
        dh2 = dlogits @ W3.T; dh2[h2 <= 0] = 0
        dW2 = h1.T @ dh2; db2 = np.sum(dh2, axis=0)
        dh1 = dh2 @ W2.T; dh1[h1 <= 0] = 0
        dW1 = X_train.T @ dh1; db1 = np.sum(dh1, axis=0)

        def adam_step(p, g, m, v):
            m[:] = 0.9 * m + 0.1 * g
            v[:] = 0.999 * v + 0.001 * (g ** 2)
            m_hat = m / (1 - 0.9 ** epoch)
            v_hat = v / (1 - 0.999 ** epoch)
            p -= lr * m_hat / (np.sqrt(v_hat) + 1e-8)

        adam_step(W1, dW1, mW1, vW1); adam_step(b1, db1, mb1, vb1)
        adam_step(W2, dW2, mW2, vW2); adam_step(b2, db2, mb2, vb2)
        adam_step(W3, dW3, mW3, vW3); adam_step(b3, db3, mb3, vb3)

        if epoch % 40 == 0:
            pred_y = np.argmax(logits, axis=1)
            acc = np.mean(pred_y == Y) * 100
            print(f"  Epoch {epoch:3d} | Accuracy: {acc:.1f}%")

    print("[SAM Trainer] Phase 2: Quantization-Aware Training (4-bit STE)...")
    # Determine scaling factors to map weights to [-8, 7]
    s1 = 7.0 / np.max(np.abs(W1))
    s2 = 7.0 / np.max(np.abs(W2))
    s3 = 7.0 / np.max(np.abs(W3))

    qW1_f = W1 * s1; qb1_f = b1 * s1
    qW2_f = W2 * s2; qb2_f = b2 * (s1 * s2)
    qW3_f = W3 * s3; qb3_f = b3 * (s1 * s2 * s3)

    mW1, vW1 = adam_init(qW1_f); mb1, vb1 = adam_init(qb1_f)
    mW2, vW2 = adam_init(qW2_f); mb2, vb2 = adam_init(qb2_f)
    mW3, vW3 = adam_init(qW3_f); mb3, vb3 = adam_init(qb3_f)

    lr_qat = 0.001
    for epoch in range(1, 41):
        cW1 = np.clip(np.round(qW1_f), -8, 7)
        cb1 = np.round(qb1_f)
        cW2 = np.clip(np.round(qW2_f), -8, 7)
        cb2 = np.round(qb2_f)
        cW3 = np.clip(np.round(qW3_f), -8, 7)
        cb3 = np.round(qb3_f)

        h1 = np.maximum(0, X @ cW1 + cb1)
        h2 = np.maximum(0, h1 @ cW2 + cb2)
        logits = h2 @ cW3 + cb3

        logits_max = np.max(logits, axis=1, keepdims=True)
        exp_logits = np.exp(logits - logits_max)
        probs = exp_logits / np.sum(exp_logits, axis=1, keepdims=True)

        dlogits = probs.copy()
        dlogits[np.arange(N), Y] -= 1.0
        dlogits /= N

        dW3 = h2.T @ dlogits; db3 = np.sum(dlogits, axis=0)
        dh2 = dlogits @ cW3.T; dh2[h2 <= 0] = 0
        dW2 = h1.T @ dh2; db2 = np.sum(dh2, axis=0)
        dh1 = dh2 @ cW2.T; dh1[h1 <= 0] = 0
        dW1 = X.T @ dh1; db1 = np.sum(dh1, axis=0)

        def adam_qat(p, g, m, v):
            m[:] = 0.9 * m + 0.1 * g
            v[:] = 0.999 * v + 0.001 * (g ** 2)
            m_hat = m / (1 - 0.9 ** epoch)
            v_hat = v / (1 - 0.999 ** epoch)
            p -= lr_qat * m_hat / (np.sqrt(v_hat) + 1e-8)

        adam_qat(qW1_f, dW1, mW1, vW1); adam_qat(qb1_f, db1, mb1, vb1)
        adam_qat(qW2_f, dW2, mW2, vW2); adam_qat(qb2_f, db2, mb2, vb2)
        adam_qat(qW3_f, dW3, mW3, vW3); adam_qat(qb3_f, db3, mb3, vb3)

        if epoch % 20 == 0:
            pred_y = np.argmax(logits, axis=1)
            acc = np.mean(pred_y == Y) * 100
            print(f"  QAT Epoch {epoch:2d} | Accuracy: {acc:.1f}%")

    print(f"[SAM Trainer] Training completed in {time.time() - t0:.2f}s.")

    # Strictly 4-bit quantized weights (-8..+7)
    final_W1 = np.clip(np.round(qW1_f), -8, 7).astype(np.int8)
    final_b1 = np.round(qb1_f).astype(np.int16)
    final_W2 = np.clip(np.round(qW2_f), -8, 7).astype(np.int8)
    final_b2 = np.round(qb2_f).astype(np.int32)
    final_W3 = np.clip(np.round(qW3_f), -8, 7).astype(np.int8)
    final_b3 = np.round(qb3_f).astype(np.int32)

    # Test generation with 100% quantized weights
    print("\n[SAM Trainer] Verification generation:")
    for test_p in ["hello", "gpus?", "7 trillion", "stargate", "ibm 5150", "who made you"]:
        p_vec = encode_prompt(test_p)
        resp = ""
        for _ in range(120):
            ctx_vec = encode_context(resp)
            x = np.concatenate([p_vec, ctx_vec])
            h1 = np.maximum(0, x @ final_W1 + final_b1)
            h2 = np.maximum(0, h1 @ final_W2 + final_b2)
            logits = h2 @ final_W3 + final_b3
            t = np.argmax(logits)
            ch = token_to_char(t)
            if ch == '\n':
                break
            resp += ch
        print(f"  Q: '{test_p}' -> A: '{resp}'")

    # Export to SAM.MDL
    # Header format:
    # 0x00: char magic[4] = "SMLM"
    # 0x04: uint16 version = 1
    # 0x06: uint16 in_dim = 256
    # 0x08: uint16 h1_dim = 128
    # 0x0A: uint16 h2_dim = 96
    # 0x0C: uint16 out_dim = 96
    # 0x0E: uint16 reserved = 0
    # 0x10: 48 bytes reserved padding (header = 64 bytes total)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "wb") as f:
        hdr = struct.pack("<4sHHHHHH48s", b"SMLM", 1, D_in, H1, H2, D_out, 0, b"\x00" * 48)
        f.write(hdr)

        # Helper to pack 2 signed 4-bit nibbles into 1 byte
        # nibble0 (bits 0..3), nibble1 (bits 4..7)
        def pack_weights(W):
            flat = W.flatten()
            packed = bytearray(len(flat) // 2)
            for i in range(len(packed)):
                w0 = int(flat[2 * i]) & 0x0F
                w1 = int(flat[2 * i + 1]) & 0x0F
                packed[i] = w0 | (w1 << 4)
            return packed

        # Write Layer 1 weights (packed 4-bit) & biases (int16)
        f.write(pack_weights(final_W1))
        f.write(final_b1.astype("<i2").tobytes())

        # Write Layer 2 weights (packed 4-bit) & biases (int32)
        f.write(pack_weights(final_W2))
        f.write(final_b2.astype("<i4").tobytes())

        # Write Output layer weights (packed 4-bit) & biases (int32)
        f.write(pack_weights(final_W3))
        f.write(final_b3.astype("<i4").tobytes())

    file_sz = os.path.getsize(output_path)
    print(f"\n[SAM Trainer] Exported model to '{output_path}' ({file_sz} bytes).")
    ems_pages = (file_sz + 16383) // 16384
    print(f"[SAM Trainer] Requires {ems_pages} EMS 16KB page(s) ({ems_pages * 16} KB).")

if __name__ == "__main__":
    out_path = sys.argv[1] if len(sys.argv) > 1 else "build/sam.mdl"
    train_model(out_path)
