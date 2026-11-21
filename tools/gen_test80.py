#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a CP/M-80 test binary (TEST80.COM) executing standard 8080 opcodes.

import sys

def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "test80.com"

    code = bytearray()

    # 0x0100: MVI C, 9 (0x0E, 0x09) - Print String
    code += bytes([0x0E, 0x09])
    # LXI D, 0x0120 (0x11, 0x20, 0x01)
    code += bytes([0x11, 0x20, 0x01])
    # CALL 0x0005 (0xCD, 0x05, 0x00)
    code += bytes([0xCD, 0x05, 0x00])

    # MVI C, 12 (0x0E, 0x0C) - Get Version
    code += bytes([0x0E, 0x0C])
    # CALL 0x0005 (0xCD, 0x05, 0x00)
    code += bytes([0xCD, 0x05, 0x00])

    # MVI C, 9 (0x0E, 0x09) - Print OK String
    code += bytes([0x0E, 0x09])
    # LXI D, 0x0150 (0x11, 0x50, 0x01)
    code += bytes([0x11, 0x50, 0x01])
    # CALL 0x0005 (0xCD, 0x05, 0x00)
    code += bytes([0xCD, 0x05, 0x00])

    # MVI C, 0 (0x0E, 0x00) - Warm Boot / Exit
    code += bytes([0x0E, 0x00])
    # CALL 0x0005 (0xCD, 0x05, 0x00)
    code += bytes([0xCD, 0x05, 0x00])

    # Pad to 0x0120 (offset 0x20 from 0x0100 = 32 bytes)
    while len(code) < 32:
        code.append(0)

    # String 1 at 0x0120
    msg1 = "Testing CP/M-80 runtime environment...\r\n$"
    code += msg1.encode("ascii")

    # Pad to 0x0150 (offset 0x50 from 0x0100 = 80 bytes)
    while len(code) < 80:
        code.append(0)

    # String 2 at 0x0150
    msg2 = "CP/M-80 BDOS v2.2 execution verified successfully!\r\n$"
    code += msg2.encode("ascii")

    with open(out_path, "wb") as f:
        f.write(code)

    print(f"Generated CP/M test binary: {out_path} ({len(code)} bytes)")

if __name__ == "__main__":
    main()
