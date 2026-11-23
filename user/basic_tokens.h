/* Copyright (C) 2026 Benoit Colson */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Auto-generated from PC-BASIC / GW-BASIC token specifications */

#ifndef HOBBYOS_BASIC_TOKENS_H
#define HOBBYOS_BASIC_TOKENS_H

#include "hobbyos.h"

/* Special / Literal tokens */
#define TOK_LIT_OCT          0x0B
#define TOK_LIT_HEX          0x0C
#define TOK_LIT_UINT_PROC    0x0D
#define TOK_LIT_UINT         0x0E
#define TOK_LIT_BYTE         0x0F
#define TOK_LIT_INT          0x1C
#define TOK_LIT_SINGLE       0x1D
#define TOK_LIT_DOUBLE       0x1F

/* Number literals C_0..C_10 (0x11..0x1B) */
#define TOK_C_0          0x11
#define TOK_C_1          0x12
#define TOK_C_2          0x13
#define TOK_C_3          0x14
#define TOK_C_4          0x15
#define TOK_C_5          0x16
#define TOK_C_6          0x17
#define TOK_C_7          0x18
#define TOK_C_8          0x19
#define TOK_C_9          0x1A
#define TOK_C_10         0x1B

/* Single-byte keyword tokens */
#define TOK_END          0x81
#define TOK_FOR          0x82
#define TOK_NEXT         0x83
#define TOK_DATA         0x84
#define TOK_INPUT        0x85
#define TOK_DIM          0x86
#define TOK_READ         0x87
#define TOK_LET          0x88
#define TOK_GOTO         0x89
#define TOK_RUN          0x8A
#define TOK_IF           0x8B
#define TOK_RESTORE      0x8C
#define TOK_GOSUB        0x8D
#define TOK_RETURN       0x8E
#define TOK_REM          0x8F
#define TOK_STOP         0x90
#define TOK_PRINT        0x91
#define TOK_CLEAR        0x92
#define TOK_LIST         0x93
#define TOK_NEW          0x94
#define TOK_ON           0x95
#define TOK_WAIT         0x96
#define TOK_DEF          0x97
#define TOK_POKE         0x98
#define TOK_CONT         0x99
#define TOK_OUT          0x9C
#define TOK_LPRINT       0x9D
#define TOK_LLIST        0x9E
#define TOK_WIDTH        0xA0
#define TOK_ELSE         0xA1
#define TOK_TRON         0xA2
#define TOK_TROFF        0xA3
#define TOK_SWAP         0xA4
#define TOK_ERASE        0xA5
#define TOK_EDIT         0xA6
#define TOK_ERROR        0xA7
#define TOK_RESUME       0xA8
#define TOK_DELETE       0xA9
#define TOK_AUTO         0xAA
#define TOK_RENUM        0xAB
#define TOK_DEFSTR       0xAC
#define TOK_DEFINT       0xAD
#define TOK_DEFSNG       0xAE
#define TOK_DEFDBL       0xAF
#define TOK_LINE         0xB0
#define TOK_WHILE        0xB1
#define TOK_WEND         0xB2
#define TOK_CALL         0xB3
#define TOK_WRITE        0xB7
#define TOK_OPTION       0xB8
#define TOK_RANDOMIZE    0xB9
#define TOK_OPEN         0xBA
#define TOK_CLOSE        0xBB
#define TOK_LOAD         0xBC
#define TOK_MERGE        0xBD
#define TOK_SAVE         0xBE
#define TOK_COLOR        0xBF
#define TOK_CLS          0xC0
#define TOK_MOTOR        0xC1
#define TOK_BSAVE        0xC2
#define TOK_BLOAD        0xC3
#define TOK_SOUND        0xC4
#define TOK_BEEP         0xC5
#define TOK_PSET         0xC6
#define TOK_PRESET       0xC7
#define TOK_SCREEN       0xC8
#define TOK_KEY          0xC9
#define TOK_LOCATE       0xCA
#define TOK_TO           0xCC
#define TOK_THEN         0xCD
#define TOK_TAB          0xCE
#define TOK_STEP         0xCF
#define TOK_USR          0xD0
#define TOK_FN           0xD1
#define TOK_SPC          0xD2
#define TOK_NOT          0xD3
#define TOK_ERL          0xD4
#define TOK_ERR          0xD5
#define TOK_STRING       0xD6
#define TOK_USING        0xD7
#define TOK_INSTR        0xD8
#define TOK_SQUOTE       0xD9
#define TOK_VARPTR       0xDA
#define TOK_CSRLIN       0xDB
#define TOK_POINT        0xDC
#define TOK_OFF          0xDD
#define TOK_INKEY        0xDE
#define TOK_GT           0xE6
#define TOK_EQ           0xE7
#define TOK_LT           0xE8
#define TOK_PLUS         0xE9
#define TOK_MINUS        0xEA
#define TOK_TIMES        0xEB
#define TOK_DIV          0xEC
#define TOK_CARET        0xED
#define TOK_AND          0xEE
#define TOK_OR           0xEF
#define TOK_XOR          0xF0
#define TOK_EQV          0xF1
#define TOK_IMP          0xF2
#define TOK_MOD          0xF3
#define TOK_INTDIV       0xF4

/* Multi-byte keyword tokens (0xFD, 0xFE, 0xFF prefixes) */
#define TOK_CVI          0xFD81
#define TOK_CVS          0xFD82
#define TOK_CVD          0xFD83
#define TOK_MKI          0xFD84
#define TOK_MKS          0xFD85
#define TOK_MKD          0xFD86
#define TOK_EXTERR       0xFD8B
#define TOK_FILES        0xFE81
#define TOK_FIELD        0xFE82
#define TOK_SYSTEM       0xFE83
#define TOK_NAME         0xFE84
#define TOK_LSET         0xFE85
#define TOK_RSET         0xFE86
#define TOK_KILL         0xFE87
#define TOK_PUT          0xFE88
#define TOK_GET          0xFE89
#define TOK_RESET        0xFE8A
#define TOK_COMMON       0xFE8B
#define TOK_CHAIN        0xFE8C
#define TOK_DATE         0xFE8D
#define TOK_TIME         0xFE8E
#define TOK_PAINT        0xFE8F
#define TOK_COM          0xFE90
#define TOK_CIRCLE       0xFE91
#define TOK_DRAW         0xFE92
#define TOK_PLAY         0xFE93
#define TOK_TIMER        0xFE94
#define TOK_ERDEV        0xFE95
#define TOK_IOCTL        0xFE96
#define TOK_CHDIR        0xFE97
#define TOK_MKDIR        0xFE98
#define TOK_RMDIR        0xFE99
#define TOK_SHELL        0xFE9A
#define TOK_ENVIRON      0xFE9B
#define TOK_VIEW         0xFE9C
#define TOK_WINDOW       0xFE9D
#define TOK_PMAP         0xFE9E
#define TOK_PALETTE      0xFE9F
#define TOK_LCOPY        0xFEA0
#define TOK_CALLS        0xFEA1
#define TOK_PCOPY        0xFEA5
#define TOK_LOCK         0xFEA7
#define TOK_UNLOCK       0xFEA8
#define TOK_LEFT         0xFF81
#define TOK_RIGHT        0xFF82
#define TOK_MID          0xFF83
#define TOK_SGN          0xFF84
#define TOK_INT          0xFF85
#define TOK_ABS          0xFF86
#define TOK_SQR          0xFF87
#define TOK_RND          0xFF88
#define TOK_SIN          0xFF89
#define TOK_LOG          0xFF8A
#define TOK_EXP          0xFF8B
#define TOK_COS          0xFF8C
#define TOK_TAN          0xFF8D
#define TOK_ATN          0xFF8E
#define TOK_FRE          0xFF8F
#define TOK_INP          0xFF90
#define TOK_POS          0xFF91
#define TOK_LEN          0xFF92
#define TOK_STR          0xFF93
#define TOK_VAL          0xFF94
#define TOK_ASC          0xFF95
#define TOK_CHR          0xFF96
#define TOK_PEEK         0xFF97
#define TOK_SPACE        0xFF98
#define TOK_OCT          0xFF99
#define TOK_HEX          0xFF9A
#define TOK_LPOS         0xFF9B
#define TOK_CINT         0xFF9C
#define TOK_CSNG         0xFF9D
#define TOK_CDBL         0xFF9E
#define TOK_FIX          0xFF9F
#define TOK_PEN          0xFFA0
#define TOK_STICK        0xFFA1
#define TOK_STRIG        0xFFA2
#define TOK_EOF          0xFFA3
#define TOK_LOC          0xFFA4
#define TOK_LOF          0xFFA5

/* Token-to-Keyword Table */
typedef struct {
    const char *kw;
    uint16_t tok;
    uint8_t len;
} basic_kw_entry_t;

static const basic_kw_entry_t g_basic_keywords[] = {
    { "RANDOMIZE", 0x00B9, 9 },
    { "STRING$", 0x00D6, 7 },
    { "RESTORE", 0x008C, 7 },
    { "PALETTE", 0xFE9F, 7 },
    { "ENVIRON", 0xFE9B, 7 },
    { "WINDOW", 0xFE9D, 6 },
    { "VARPTR", 0x00DA, 6 },
    { "UNLOCK", 0xFEA8, 6 },
    { "SYSTEM", 0xFE83, 6 },
    { "SPACE$", 0xFF98, 6 },
    { "SCREEN", 0x00C8, 6 },
    { "RIGHT$", 0xFF82, 6 },
    { "RETURN", 0x008E, 6 },
    { "RESUME", 0x00A8, 6 },
    { "PRESET", 0x00C7, 6 },
    { "OPTION", 0x00B8, 6 },
    { "LPRINT", 0x009D, 6 },
    { "LOCATE", 0x00CA, 6 },
    { "INKEY$", 0x00DE, 6 },
    { "EXTERR", 0xFD8B, 6 },
    { "DELETE", 0x00A9, 6 },
    { "DEFSTR", 0x00AC, 6 },
    { "DEFSNG", 0x00AE, 6 },
    { "DEFINT", 0x00AD, 6 },
    { "DEFDBL", 0x00AF, 6 },
    { "CSRLIN", 0x00DB, 6 },
    { "COMMON", 0xFE8B, 6 },
    { "CIRCLE", 0xFE91, 6 },
    { "WRITE", 0x00B7, 5 },
    { "WIDTH", 0x00A0, 5 },
    { "WHILE", 0x00B1, 5 },
    { "USING", 0x00D7, 5 },
    { "TROFF", 0x00A3, 5 },
    { "TIMER", 0xFE94, 5 },
    { "TIME$", 0xFE8E, 5 },
    { "STRIG", 0xFFA2, 5 },
    { "STICK", 0xFFA1, 5 },
    { "SOUND", 0x00C4, 5 },
    { "SHELL", 0xFE9A, 5 },
    { "RMDIR", 0xFE99, 5 },
    { "RESET", 0xFE8A, 5 },
    { "RENUM", 0x00AB, 5 },
    { "PRINT", 0x0091, 5 },
    { "POINT", 0x00DC, 5 },
    { "PCOPY", 0xFEA5, 5 },
    { "PAINT", 0xFE8F, 5 },
    { "MOTOR", 0x00C1, 5 },
    { "MKDIR", 0xFE98, 5 },
    { "MERGE", 0x00BD, 5 },
    { "LLIST", 0x009E, 5 },
    { "LEFT$", 0xFF81, 5 },
    { "LCOPY", 0xFEA0, 5 },
    { "IOCTL", 0xFE96, 5 },
    { "INSTR", 0x00D8, 5 },
    { "INPUT", 0x0085, 5 },
    { "GOSUB", 0x008D, 5 },
    { "FILES", 0xFE81, 5 },
    { "FIELD", 0xFE82, 5 },
    { "ERROR", 0x00A7, 5 },
    { "ERDEV", 0xFE95, 5 },
    { "ERASE", 0x00A5, 5 },
    { "DATE$", 0xFE8D, 5 },
    { "COLOR", 0x00BF, 5 },
    { "CLOSE", 0x00BB, 5 },
    { "CLEAR", 0x0092, 5 },
    { "CHDIR", 0xFE97, 5 },
    { "CHAIN", 0xFE8C, 5 },
    { "CALLS", 0xFEA1, 5 },
    { "BSAVE", 0x00C2, 5 },
    { "BLOAD", 0x00C3, 5 },
    { "WEND", 0x00B2, 4 },
    { "WAIT", 0x0096, 4 },
    { "VIEW", 0xFE9C, 4 },
    { "TRON", 0x00A2, 4 },
    { "THEN", 0x00CD, 4 },
    { "TAB(", 0x00CE, 4 },
    { "SWAP", 0x00A4, 4 },
    { "STR$", 0xFF93, 4 },
    { "STOP", 0x0090, 4 },
    { "STEP", 0x00CF, 4 },
    { "SPC(", 0x00D2, 4 },
    { "SAVE", 0x00BE, 4 },
    { "RSET", 0xFE86, 4 },
    { "READ", 0x0087, 4 },
    { "PSET", 0x00C6, 4 },
    { "POKE", 0x0098, 4 },
    { "PMAP", 0xFE9E, 4 },
    { "PLAY", 0xFE93, 4 },
    { "PEEK", 0xFF97, 4 },
    { "OPEN", 0x00BA, 4 },
    { "OCT$", 0xFF99, 4 },
    { "NEXT", 0x0083, 4 },
    { "NAME", 0xFE84, 4 },
    { "MKS$", 0xFD85, 4 },
    { "MKI$", 0xFD84, 4 },
    { "MKD$", 0xFD86, 4 },
    { "MID$", 0xFF83, 4 },
    { "LSET", 0xFE85, 4 },
    { "LPOS", 0xFF9B, 4 },
    { "LOCK", 0xFEA7, 4 },
    { "LOAD", 0x00BC, 4 },
    { "LIST", 0x0093, 4 },
    { "LINE", 0x00B0, 4 },
    { "KILL", 0xFE87, 4 },
    { "HEX$", 0xFF9A, 4 },
    { "GOTO", 0x0089, 4 },
    { "ELSE", 0x00A1, 4 },
    { "EDIT", 0x00A6, 4 },
    { "DRAW", 0xFE92, 4 },
    { "DATA", 0x0084, 4 },
    { "CSNG", 0xFF9D, 4 },
    { "CONT", 0x0099, 4 },
    { "CINT", 0xFF9C, 4 },
    { "CHR$", 0xFF96, 4 },
    { "CDBL", 0xFF9E, 4 },
    { "CALL", 0x00B3, 4 },
    { "BEEP", 0x00C5, 4 },
    { "AUTO", 0x00AA, 4 },
    { "XOR", 0x00F0, 3 },
    { "VAL", 0xFF94, 3 },
    { "USR", 0x00D0, 3 },
    { "TAN", 0xFF8D, 3 },
    { "SQR", 0xFF87, 3 },
    { "SIN", 0xFF89, 3 },
    { "SGN", 0xFF84, 3 },
    { "RUN", 0x008A, 3 },
    { "RND", 0xFF88, 3 },
    { "REM", 0x008F, 3 },
    { "PUT", 0xFE88, 3 },
    { "POS", 0xFF91, 3 },
    { "PEN", 0xFFA0, 3 },
    { "OUT", 0x009C, 3 },
    { "OFF", 0x00DD, 3 },
    { "NOT", 0x00D3, 3 },
    { "NEW", 0x0094, 3 },
    { "MOD", 0x00F3, 3 },
    { "LOG", 0xFF8A, 3 },
    { "LOF", 0xFFA5, 3 },
    { "LOC", 0xFFA4, 3 },
    { "LET", 0x0088, 3 },
    { "LEN", 0xFF92, 3 },
    { "KEY", 0x00C9, 3 },
    { "INT", 0xFF85, 3 },
    { "INP", 0xFF90, 3 },
    { "IMP", 0x00F2, 3 },
    { "GET", 0xFE89, 3 },
    { "FRE", 0xFF8F, 3 },
    { "FOR", 0x0082, 3 },
    { "FIX", 0xFF9F, 3 },
    { "EXP", 0xFF8B, 3 },
    { "ERR", 0x00D5, 3 },
    { "ERL", 0x00D4, 3 },
    { "EQV", 0x00F1, 3 },
    { "EOF", 0xFFA3, 3 },
    { "END", 0x0081, 3 },
    { "DIM", 0x0086, 3 },
    { "DEF", 0x0097, 3 },
    { "CVS", 0xFD82, 3 },
    { "CVI", 0xFD81, 3 },
    { "CVD", 0xFD83, 3 },
    { "COS", 0xFF8C, 3 },
    { "COM", 0xFE90, 3 },
    { "CLS", 0x00C0, 3 },
    { "ATN", 0xFF8E, 3 },
    { "ASC", 0xFF95, 3 },
    { "AND", 0x00EE, 3 },
    { "ABS", 0xFF86, 3 },
    { "TO", 0x00CC, 2 },
    { "OR", 0x00EF, 2 },
    { "ON", 0x0095, 2 },
    { "IF", 0x008B, 2 },
    { "FN", 0x00D1, 2 },
    { "^", 0x00ED, 1 },
    { "\\", 0x00F4, 1 },
    { "?", 0x0091, 1 },
    { ">", 0x00E6, 1 },
    { "=", 0x00E7, 1 },
    { "<", 0x00E8, 1 },
    { "/", 0x00EC, 1 },
    { "-", 0x00EA, 1 },
    { "+", 0x00E9, 1 },
    { "*", 0x00EB, 1 },
    { "'", 0x00D9, 1 },
    { 0, 0, 0 }
};

#define BASIC_NUM_KEYWORDS (sizeof(g_basic_keywords) / sizeof(g_basic_keywords[0]) - 1)

static const char *basic_token_to_name(uint16_t tok) {
    switch (tok) {
    case 0x0081: return "END";
    case 0x0082: return "FOR";
    case 0x0083: return "NEXT";
    case 0x0084: return "DATA";
    case 0x0085: return "INPUT";
    case 0x0086: return "DIM";
    case 0x0087: return "READ";
    case 0x0088: return "LET";
    case 0x0089: return "GOTO";
    case 0x008A: return "RUN";
    case 0x008B: return "IF";
    case 0x008C: return "RESTORE";
    case 0x008D: return "GOSUB";
    case 0x008E: return "RETURN";
    case 0x008F: return "REM";
    case 0x0090: return "STOP";
    case 0x0091: return "PRINT";
    case 0x0092: return "CLEAR";
    case 0x0093: return "LIST";
    case 0x0094: return "NEW";
    case 0x0095: return "ON";
    case 0x0096: return "WAIT";
    case 0x0097: return "DEF";
    case 0x0098: return "POKE";
    case 0x0099: return "CONT";
    case 0x009C: return "OUT";
    case 0x009D: return "LPRINT";
    case 0x009E: return "LLIST";
    case 0x00A0: return "WIDTH";
    case 0x00A1: return "ELSE";
    case 0x00A2: return "TRON";
    case 0x00A3: return "TROFF";
    case 0x00A4: return "SWAP";
    case 0x00A5: return "ERASE";
    case 0x00A6: return "EDIT";
    case 0x00A7: return "ERROR";
    case 0x00A8: return "RESUME";
    case 0x00A9: return "DELETE";
    case 0x00AA: return "AUTO";
    case 0x00AB: return "RENUM";
    case 0x00AC: return "DEFSTR";
    case 0x00AD: return "DEFINT";
    case 0x00AE: return "DEFSNG";
    case 0x00AF: return "DEFDBL";
    case 0x00B0: return "LINE";
    case 0x00B1: return "WHILE";
    case 0x00B2: return "WEND";
    case 0x00B3: return "CALL";
    case 0x00B7: return "WRITE";
    case 0x00B8: return "OPTION";
    case 0x00B9: return "RANDOMIZE";
    case 0x00BA: return "OPEN";
    case 0x00BB: return "CLOSE";
    case 0x00BC: return "LOAD";
    case 0x00BD: return "MERGE";
    case 0x00BE: return "SAVE";
    case 0x00BF: return "COLOR";
    case 0x00C0: return "CLS";
    case 0x00C1: return "MOTOR";
    case 0x00C2: return "BSAVE";
    case 0x00C3: return "BLOAD";
    case 0x00C4: return "SOUND";
    case 0x00C5: return "BEEP";
    case 0x00C6: return "PSET";
    case 0x00C7: return "PRESET";
    case 0x00C8: return "SCREEN";
    case 0x00C9: return "KEY";
    case 0x00CA: return "LOCATE";
    case 0x00CC: return "TO";
    case 0x00CD: return "THEN";
    case 0x00CE: return "TAB(";
    case 0x00CF: return "STEP";
    case 0x00D0: return "USR";
    case 0x00D1: return "FN";
    case 0x00D2: return "SPC(";
    case 0x00D3: return "NOT";
    case 0x00D4: return "ERL";
    case 0x00D5: return "ERR";
    case 0x00D6: return "STRING$";
    case 0x00D7: return "USING";
    case 0x00D8: return "INSTR";
    case 0x00D9: return "'";
    case 0x00DA: return "VARPTR";
    case 0x00DB: return "CSRLIN";
    case 0x00DC: return "POINT";
    case 0x00DD: return "OFF";
    case 0x00DE: return "INKEY$";
    case 0x00E6: return ">";
    case 0x00E7: return "=";
    case 0x00E8: return "<";
    case 0x00E9: return "+";
    case 0x00EA: return "-";
    case 0x00EB: return "*";
    case 0x00EC: return "/";
    case 0x00ED: return "^";
    case 0x00EE: return "AND";
    case 0x00EF: return "OR";
    case 0x00F0: return "XOR";
    case 0x00F1: return "EQV";
    case 0x00F2: return "IMP";
    case 0x00F3: return "MOD";
    case 0x00F4: return "\\";
    case 0xFD81: return "CVI";
    case 0xFD82: return "CVS";
    case 0xFD83: return "CVD";
    case 0xFD84: return "MKI$";
    case 0xFD85: return "MKS$";
    case 0xFD86: return "MKD$";
    case 0xFD8B: return "EXTERR";
    case 0xFE81: return "FILES";
    case 0xFE82: return "FIELD";
    case 0xFE83: return "SYSTEM";
    case 0xFE84: return "NAME";
    case 0xFE85: return "LSET";
    case 0xFE86: return "RSET";
    case 0xFE87: return "KILL";
    case 0xFE88: return "PUT";
    case 0xFE89: return "GET";
    case 0xFE8A: return "RESET";
    case 0xFE8B: return "COMMON";
    case 0xFE8C: return "CHAIN";
    case 0xFE8D: return "DATE$";
    case 0xFE8E: return "TIME$";
    case 0xFE8F: return "PAINT";
    case 0xFE90: return "COM";
    case 0xFE91: return "CIRCLE";
    case 0xFE92: return "DRAW";
    case 0xFE93: return "PLAY";
    case 0xFE94: return "TIMER";
    case 0xFE95: return "ERDEV";
    case 0xFE96: return "IOCTL";
    case 0xFE97: return "CHDIR";
    case 0xFE98: return "MKDIR";
    case 0xFE99: return "RMDIR";
    case 0xFE9A: return "SHELL";
    case 0xFE9B: return "ENVIRON";
    case 0xFE9C: return "VIEW";
    case 0xFE9D: return "WINDOW";
    case 0xFE9E: return "PMAP";
    case 0xFE9F: return "PALETTE";
    case 0xFEA0: return "LCOPY";
    case 0xFEA1: return "CALLS";
    case 0xFEA5: return "PCOPY";
    case 0xFEA7: return "LOCK";
    case 0xFEA8: return "UNLOCK";
    case 0xFF81: return "LEFT$";
    case 0xFF82: return "RIGHT$";
    case 0xFF83: return "MID$";
    case 0xFF84: return "SGN";
    case 0xFF85: return "INT";
    case 0xFF86: return "ABS";
    case 0xFF87: return "SQR";
    case 0xFF88: return "RND";
    case 0xFF89: return "SIN";
    case 0xFF8A: return "LOG";
    case 0xFF8B: return "EXP";
    case 0xFF8C: return "COS";
    case 0xFF8D: return "TAN";
    case 0xFF8E: return "ATN";
    case 0xFF8F: return "FRE";
    case 0xFF90: return "INP";
    case 0xFF91: return "POS";
    case 0xFF92: return "LEN";
    case 0xFF93: return "STR$";
    case 0xFF94: return "VAL";
    case 0xFF95: return "ASC";
    case 0xFF96: return "CHR$";
    case 0xFF97: return "PEEK";
    case 0xFF98: return "SPACE$";
    case 0xFF99: return "OCT$";
    case 0xFF9A: return "HEX$";
    case 0xFF9B: return "LPOS";
    case 0xFF9C: return "CINT";
    case 0xFF9D: return "CSNG";
    case 0xFF9E: return "CDBL";
    case 0xFF9F: return "FIX";
    case 0xFFA0: return "PEN";
    case 0xFFA1: return "STICK";
    case 0xFFA2: return "STRIG";
    case 0xFFA3: return "EOF";
    case 0xFFA4: return "LOC";
    case 0xFFA5: return "LOF";
    default: return 0;
    }
}

#endif /* HOBBYOS_BASIC_TOKENS_H */
