; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; User-space C runtime entry point for hobbyOS flat binaries.

[BITS 16]
CPU 8086

extern _app_main

section _TEXT class=CODE

global _ustart
global _cstart_
_ustart:
_cstart_:
    cld
    call _app_main
    mov ah, 0x00        ; SYS_EXIT (AL = return code from app_main)
    int 0x21
.hang:
    hlt
    jmp .hang
