; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; User-space C runtime entry point for hobbyOS MS-DOS MZ executables (Medium Model).

[BITS 16]
CPU 8086

extern _app_main

group DGROUP _DATA CONST _BSS STACK

section _TEXT class=CODE

global _ustart
global _cstart_
_ustart:
_cstart_:
    cld
    ; ES is psp_seg from the loader
    mov ax, DGROUP
    mov ds, ax
    mov ss, ax
    mov sp, stack_top

    ; Copy command-line args from PSP:0x0080 to DGROUP:0xFF00 (USER_ARGS_OFFSET)
    xor ch, ch
    mov cl, [es:0x0080]     ; length of args
    mov si, 0x0081          ; source in ES:0x0081
    mov di, 0xFF00          ; dest in DS:0xFF00
    jcxz .no_args
.copy_args:
    mov al, [es:si]
    inc si
    mov [di], al
    inc di
    loop .copy_args
.no_args:
    mov byte [di], 0        ; null-terminate

    call far _app_main
    mov ah, 0x00        ; SYS_EXIT (AL = return code from app_main)
    int 0x21
.hang:
    hlt
    jmp .hang

section _DATA class=DATA
section CONST class=CONST
section _BSS class=BSS

section STACK class=STACK
stack_bottom:
    resb 4096
stack_top:
