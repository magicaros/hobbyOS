; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; User-space C runtime entry point for hobbyOS flat binaries.
; Automatically preserves command-line arguments, zeroes _BSS, relocates
; the stack to 4 KB above _BSS, and shrinks the process MCB segment.

[BITS 16]
CPU 8086

extern _app_main

group DGROUP _TEXT CONST CONST2 _DATA _BSS _END

section _TEXT class=CODE

global _ustart
global _cstart_
_ustart:
_cstart_:
    jmp short .entry
    dw 0x4248           ; 'HB' compact binary header signature at CS:0x0002
    dw __user_args      ; near offset of 256-byte args buffer at CS:0x0004
.entry:
    cld

    ; 1. Copy 256-byte command-line arguments from CS:0xFF00 into __user_args
    mov si, 0xFF00
    mov di, __user_args
    mov cx, 128
    rep movsw

    ; 2. Zero the C program's _BSS section (__bss_zero_start .. __bss_end)
    mov di, __bss_zero_start
    mov cx, __bss_end
    sub cx, di
    jbe .bss_cleared
    xor al, al
    rep stosb
.bss_cleared:

    ; 3. Relocate stack to 4 KB (0x1000 bytes) above __bss_end (capped at 0xFEFE)
    mov ax, __bss_end
    add ax, 0x1000
    jc  .cap_sp
    cmp ax, 0xFEFE
    jbe .set_sp
.cap_sp:
    mov ax, 0xFEFE
.set_sp:
    and ax, 0xFFFE
    mov sp, ax

    ; 4. Shrink process MCB allocation to actual stack ceiling via SYS_MEM_RESIZE (0x33)
    cmp ax, 0xFEFE
    jae .call_main
    add ax, 17          ; round (sp + 2) up to 16-byte paragraph boundary
    mov cl, 4
    shr ax, cl          ; AX = paragraphs needed
    mov cx, ax
    mov bx, cs
    mov ah, 0x33        ; SYS_MEM_RESIZE (BX = seg, CX = new_paragraphs)
    int 0x21

.call_main:
    call _app_main
    mov ah, 0x00        ; SYS_EXIT (AL = return code from app_main)
    int 0x21
.hang:
    hlt
    jmp .hang

section CONST class=CONST
section CONST2 class=CONST
section _DATA class=DATA

section _BSS class=BSS
__user_args:
    resb 256
__bss_zero_start:

section _END class=END
global __bss_end
__bss_end:
