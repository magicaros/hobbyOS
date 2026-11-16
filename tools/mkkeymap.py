#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""
Generates 392-byte binary .MAP keyboard layout files (os_keymap_t) for hobbyOS:
  - US.MAP (US QWERTY)
  - FR.MAP (French AZERTY)
  - BE.MAP (Belgian AZERTY)
"""

import os
import sys

# CP437 character codes for French/Belgian symbols
CP437_SECTION = 0x15  # §
CP437_E_ACUTE = 0x82  # é
CP437_A_GRAVE = 0x85  # à
CP437_C_CEDIL = 0x87  # ç
CP437_E_GRAVE = 0x8A  # è
CP437_U_GRAVE = 0x97  # ù
CP437_POUND   = 0x9C  # £
CP437_MICRO   = 0xE6  # µ
CP437_DEGREE  = 0xF8  # °
CP437_SQUARE  = 0xFD  # ²


def ch(val) -> int:
    if isinstance(val, int):
        return val & 0xFF
    if isinstance(val, str) and len(val) == 1:
        return ord(val) & 0xFF
    return 0


def base_common_tables():
    normal = [0] * 128
    shift = [0] * 128
    altgr = [0] * 128

    # Control / whitespace / keypad keys common across US, FR, BE
    for tbl in (normal, shift):
        tbl[0x01] = 0x1B        # Esc
        tbl[0x0E] = ord("\b")   # Backspace
        tbl[0x0F] = ord("\t")   # Tab
        tbl[0x1C] = ord("\r")   # Enter
        tbl[0x37] = ord("*")    # Keypad *
        tbl[0x39] = ord(" ")    # Space
        tbl[0x4A] = ord("-")    # Keypad -
        tbl[0x4E] = ord("+")    # Keypad +

    # Shifted / NumLock keypad digits (0x47..0x53)
    shift[0x47] = ord("7")
    shift[0x48] = ord("8")
    shift[0x49] = ord("9")
    shift[0x4B] = ord("4")
    shift[0x4C] = ord("5")
    shift[0x4D] = ord("6")
    shift[0x4F] = ord("1")
    shift[0x50] = ord("2")
    shift[0x51] = ord("3")
    shift[0x52] = ord("0")
    shift[0x53] = ord(".")

    return normal, shift, altgr


def build_us_map() -> bytes:
    normal, shift, altgr = base_common_tables()

    # Row 0: 1..0 - =
    row0_n = "1234567890-="
    row0_s = "!@#$%^&*()_+"
    for i, sc in enumerate(range(0x02, 0x0E)):
        normal[sc] = ord(row0_n[i])
        shift[sc] = ord(row0_s[i])

    # Row 1: q..p [ ]
    row1_n = "qwertyuiop[]"
    row1_s = "QWERTYUIOP{}"
    for i, sc in enumerate(range(0x10, 0x1C)):
        normal[sc] = ord(row1_n[i])
        shift[sc] = ord(row1_s[i])

    # Row 2: a..l ; ' `
    row2_n = "asdfghjkl;'`"
    row2_s = 'ASDFGHJKL:"~'
    for i, sc in enumerate(range(0x1E, 0x2A)):
        normal[sc] = ord(row2_n[i])
        shift[sc] = ord(row2_s[i])

    # Row 3: \ z..m , . /
    normal[0x2B] = ord("\\")
    shift[0x2B] = ord("|")
    row3_n = "zxcvbnm,./"
    row3_s = "ZXCVBNM<>?"
    for i, sc in enumerate(range(0x2C, 0x36)):
        normal[sc] = ord(row3_n[i])
        shift[sc] = ord(row3_s[i])

    # ISO 102nd key (0x56)
    normal[0x56] = ord("\\")
    shift[0x56] = ord("|")

    return pack_keymap("US", normal, shift, altgr)


def build_fr_map() -> bytes:
    normal, shift, altgr = base_common_tables()

    # Row 0 (0x02..0x0D): French AZERTY number row
    row0 = [
        (0x02, "&",          "1",          0),
        (0x03, CP437_E_ACUTE, "2",         "~"),
        (0x04, '"',          "3",          "#"),
        (0x05, "'",          "4",          "{"),
        (0x06, "(",          "5",          "["),
        (0x07, "-",          "6",          "|"),
        (0x08, CP437_E_GRAVE, "7",         "`"),
        (0x09, "_",          "8",          "\\"),
        (0x0A, CP437_C_CEDIL, "9",         "^"),
        (0x0B, CP437_A_GRAVE, "0",         "@"),
        (0x0C, ")",          CP437_DEGREE, "]"),
        (0x0D, "=",          "+",          "}"),
    ]
    for sc, n, s, ag in row0:
        normal[sc], shift[sc], altgr[sc] = ch(n), ch(s), ch(ag)

    # Row 1 (0x10..0x1B): a z e r t y u i o p ^ $
    row1_letters = "azertyuiop"
    for i, c in enumerate(row1_letters):
        normal[0x10 + i] = ord(c)
        shift[0x10 + i] = ord(c.upper())
    normal[0x1A], shift[0x1A], altgr[0x1A] = ord("^"), ord('"'), ord("[")
    normal[0x1B], shift[0x1B], altgr[0x1B] = ord("$"), CP437_POUND, ord("]")

    # Row 2 (0x1E..0x29): q s d f g h j k l m ù ²
    row2_letters = "qsdfghjklm"
    for i, c in enumerate(row2_letters):
        normal[0x1E + i] = ord(c)
        shift[0x1E + i] = ord(c.upper())
    normal[0x28], shift[0x28], altgr[0x28] = CP437_U_GRAVE, ord("%"), ord("`")
    normal[0x29], shift[0x29], altgr[0x29] = CP437_SQUARE, ord("~"), ord("~")

    # Key 0x2B (* / µ)
    normal[0x2B], shift[0x2B], altgr[0x2B] = ord("*"), CP437_MICRO, ord("\\")

    # Row 3 (0x2C..0x35): w x c v b n , ; : !
    row3_letters = "wxcvbn"
    for i, c in enumerate(row3_letters):
        normal[0x2C + i] = ord(c)
        shift[0x2C + i] = ord(c.upper())
    normal[0x32], shift[0x32] = ord(","), ord("?")
    normal[0x33], shift[0x33] = ord(";"), ord(".")
    normal[0x34], shift[0x34] = ord(":"), ord("/")
    normal[0x35], shift[0x35], altgr[0x35] = ord("!"), CP437_SECTION, ord("\\")

    # ISO 102nd key (0x56): < > \
    normal[0x56], shift[0x56], altgr[0x56] = ord("<"), ord(">"), ord("\\")

    return pack_keymap("FR", normal, shift, altgr)


def build_be_map() -> bytes:
    normal, shift, altgr = base_common_tables()

    # Row 0 (0x02..0x0D): Belgian AZERTY number row
    row0 = [
        (0x02, "&",           "1",          "|"),
        (0x03, CP437_E_ACUTE, "2",          "@"),
        (0x04, '"',           "3",          "#"),
        (0x05, "'",           "4",          "{"),
        (0x06, "(",           "5",          "["),
        (0x07, CP437_SECTION, "6",          "^"),
        (0x08, CP437_E_GRAVE, "7",          "`"),
        (0x09, "!",           "8",          "\\"),
        (0x0A, CP437_C_CEDIL, "9",          "{"),
        (0x0B, CP437_A_GRAVE, "0",          "}"),
        (0x0C, ")",           CP437_DEGREE, "]"),
        (0x0D, "-",           "_",          "~"),
    ]
    for sc, n, s, ag in row0:
        normal[sc], shift[sc], altgr[sc] = ch(n), ch(s), ch(ag)

    # Row 1 (0x10..0x1B): a z e r t y u i o p ^ $
    row1_letters = "azertyuiop"
    for i, c in enumerate(row1_letters):
        normal[0x10 + i] = ord(c)
        shift[0x10 + i] = ord(c.upper())
    normal[0x1A], shift[0x1A], altgr[0x1A] = ord("^"), ord('"'), ord("[")
    normal[0x1B], shift[0x1B], altgr[0x1B] = ord("$"), ord("*"), ord("]")

    # Row 2 (0x1E..0x29): q s d f g h j k l m ù ²
    row2_letters = "qsdfghjklm"
    for i, c in enumerate(row2_letters):
        normal[0x1E + i] = ord(c)
        shift[0x1E + i] = ord(c.upper())
    normal[0x28], shift[0x28], altgr[0x28] = CP437_U_GRAVE, ord("%"), ord("'")
    normal[0x29], shift[0x29], altgr[0x29] = CP437_SQUARE, ord("~"), ord("\\")

    # Key 0x2B (µ / £ / `)
    normal[0x2B], shift[0x2B], altgr[0x2B] = CP437_MICRO, CP437_POUND, ord("`")

    # Row 3 (0x2C..0x35): w x c v b n , ; : =
    row3_letters = "wxcvbn"
    for i, c in enumerate(row3_letters):
        normal[0x2C + i] = ord(c)
        shift[0x2C + i] = ord(c.upper())
    normal[0x32], shift[0x32] = ord(","), ord("?")
    normal[0x33], shift[0x33] = ord(";"), ord(".")
    normal[0x34], shift[0x34] = ord(":"), ord("/")
    normal[0x35], shift[0x35], altgr[0x35] = ord("="), ord("+"), ord("~")

    # ISO 102nd key (0x56): < > \
    normal[0x56], shift[0x56], altgr[0x56] = ord("<"), ord(">"), ord("\\")

    return pack_keymap("BE", normal, shift, altgr)


def pack_keymap(name: str, normal: list[int], shift: list[int], altgr: list[int]) -> bytes:
    name_bytes = name.encode("ascii")[:7].ljust(8, b"\x00")
    data = name_bytes + bytes(normal[:128]) + bytes(shift[:128]) + bytes(altgr[:128])
    assert len(data) == 392
    return data


def main() -> None:
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "build"
    os.makedirs(out_dir, exist_ok=True)

    maps = {
        "us.map": build_us_map(),
        "fr.map": build_fr_map(),
        "be.map": build_be_map(),
    }
    for fname, blob in maps.items():
        with open(os.path.join(out_dir, fname), "wb") as f:
            f.write(blob)


if __name__ == "__main__":
    main()
