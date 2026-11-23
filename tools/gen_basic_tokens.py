#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generate user/basic_tokens.h from PC-BASIC token definitions.

import os
import sys

# Add /tmp/pcbasic to path
sys.path.insert(0, '/tmp/pcbasic')
from pcbasic.basic.base import tokens

output_path = os.path.join(os.path.dirname(__file__), '..', 'user', 'basic_tokens.h')

# Collect all keywords from KEYWORDS
# KEYWORDS: token_bytes -> keyword_bytes
entries = []
for tok_bytes, kw_bytes in tokens.KEYWORDS.items():
    kw_str = kw_bytes.decode('latin1')
    if len(tok_bytes) == 1:
        tok_val = tok_bytes[0]
    elif len(tok_bytes) == 2:
        tok_val = (tok_bytes[0] << 8) | tok_bytes[1]
    else:
        continue
    entries.append((kw_str, tok_val))

# Also add special token aliases
# '?' -> PRINT (0x91)
entries.append(('?', 0x91))

# Sort entries by keyword length descending (so longest match first in tokenizer)
entries_by_len = sorted(entries, key=lambda x: (len(x[0]), x[0]), reverse=True)

# Sort entries by token value for lister
entries_by_tok = sorted([e for e in entries if e[0] != '?'], key=lambda x: x[1])

with open(output_path, 'w', encoding='ascii') as f:
    f.write('/* Copyright (C) 2026 Benoit Colson */\n')
    f.write('/* SPDX-License-Identifier: GPL-3.0-or-later */\n')
    f.write('/* Auto-generated from PC-BASIC / GW-BASIC token specifications */\n\n')
    f.write('#ifndef HOBBYOS_BASIC_TOKENS_H\n')
    f.write('#define HOBBYOS_BASIC_TOKENS_H\n\n')
    f.write('#include "hobbyos.h"\n\n')

    # Define tokens constants
    f.write('/* Special / Literal tokens */\n')
    f.write('#define TOK_LIT_OCT          0x0B\n')
    f.write('#define TOK_LIT_HEX          0x0C\n')
    f.write('#define TOK_LIT_UINT_PROC    0x0D\n')
    f.write('#define TOK_LIT_UINT         0x0E\n')
    f.write('#define TOK_LIT_BYTE         0x0F\n')
    f.write('#define TOK_LIT_INT          0x1C\n')
    f.write('#define TOK_LIT_SINGLE       0x1D\n')
    f.write('#define TOK_LIT_DOUBLE       0x1F\n\n')

    f.write('/* Number literals C_0..C_10 (0x11..0x1B) */\n')
    for i in range(11):
        f.write(f'#define TOK_C_{i:<2d}         0x{0x11 + i:02X}\n')
    f.write('\n')

    # Keyword tokens
    f.write('/* Single-byte keyword tokens */\n')
    for kw_str, tok_val in entries_by_tok:
        if tok_val <= 0xFF:
            safe_name = kw_str.rstrip('$').replace('(', '').replace('+', 'PLUS').replace('-', 'MINUS')
            safe_name = safe_name.replace('*', 'TIMES').replace('/', 'DIV').replace('^', 'CARET')
            safe_name = safe_name.replace('=', 'EQ').replace('>', 'GT').replace('<', 'LT')
            safe_name = safe_name.replace('\\', 'INTDIV').replace("'", 'SQUOTE')
            f.write(f'#define TOK_{safe_name:<12s} 0x{tok_val:02X}\n')

    f.write('\n/* Multi-byte keyword tokens (0xFD, 0xFE, 0xFF prefixes) */\n')
    for kw_str, tok_val in entries_by_tok:
        if tok_val > 0xFF:
            safe_name = kw_str.rstrip('$').replace('(', '')
            f.write(f'#define TOK_{safe_name:<12s} 0x{tok_val:04X}\n')

    f.write('\n/* Token-to-Keyword Table */\n')
    f.write('typedef struct {\n')
    f.write('    const char *kw;\n')
    f.write('    uint16_t tok;\n')
    f.write('    uint8_t len;\n')
    f.write('} basic_kw_entry_t;\n\n')

    f.write('static const basic_kw_entry_t g_basic_keywords[] = {\n')
    for kw_str, tok_val in entries_by_len:
        escaped = kw_str.replace('\\', '\\\\').replace('"', '\\"')
        f.write(f'    {{ "{escaped}", 0x{tok_val:04X}, {len(kw_str)} }},\n')
    f.write('    { 0, 0, 0 }\n')
    f.write('};\n\n')

    f.write('#define BASIC_NUM_KEYWORDS (sizeof(g_basic_keywords) / sizeof(g_basic_keywords[0]) - 1)\n\n')

    f.write('static const char *basic_token_to_name(uint16_t tok) {\n')
    f.write('    switch (tok) {\n')
    for kw_str, tok_val in entries_by_tok:
        escaped = kw_str.replace('\\', '\\\\').replace('"', '\\"')
        f.write(f'    case 0x{tok_val:04X}: return "{escaped}";\n')
    f.write('    default: return 0;\n')
    f.write('    }\n')
    f.write('}\n\n')

    f.write('#endif /* HOBBYOS_BASIC_TOKENS_H */\n')

print(f'Generated {output_path} with {len(entries)} tokens')
