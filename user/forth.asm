; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; ==============================================================================
; JONESFORTH for hobbyOS (16-bit 8088 Real Mode)
;
; Originally written by Richard W.M. Jones <rich@annexia.org>
; and placed into the Public Domain.
;
; Ported and adapted for Intel 8088 / IBM PC/XT and hobbyOS
; by Benoit Colson <magic.aros@gmail.com>.
; ==============================================================================

[BITS 16]
CPU 8086
org 0x0000

; Entry point
jmp start

; ==============================================================================
; Constants & Flags
; ==============================================================================
F_IMMED     equ 0x80
F_HIDDEN    equ 0x20
F_LENMASK   equ 0x1F

; ==============================================================================
; Dictionary Macros (NASM)
; ==============================================================================
%define link 0

%macro NEXT 0
    lodsw
    mov bx, ax
    jmp word [bx]
%endmacro

%macro defcode 4
    align 2
name_%4:
    dw link
%define link name_%4
    db %3 + %2
    db %1
    align 2
%4:
    dw code_%4
code_%4:
%endmacro

%macro defword 4
    align 2
name_%4:
    dw link
%define link name_%4
    db %3 + %2
    db %1
    align 2
%4:
    dw DOCOL
%endmacro

%macro defvar 5
    align 2
name_%4:
    dw link
%define link name_%4
    db %3 + %2
    db %1
    align 2
%4:
    dw code_%4
code_%4:
    mov ax, var_%4
    push ax
    NEXT
var_%4:
    dw %5
%endmacro

%macro defconst 5
    align 2
name_%4:
    dw link
%define link name_%4
    db %3 + %2
    db %1
    align 2
%4:
    dw code_%4
code_%4:
    mov ax, %5
    push ax
    NEXT
%endmacro

; ==============================================================================
; Startup Sequence
; ==============================================================================
start:
    cld
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFEFE      ; Data stack pointer
    mov bp, 0xFAFE      ; Return stack pointer

    mov word [var_HERE], end_of_static_dict
    mov word [var_LATEST], last_word_header
    mov word [var_S0], 0xFEFE
    mov word [var_BASE], 10
    mov word [var_STATE], 0
    mov word [currkey], tib
    mov word [bufftop], tib

    ; Check if command-line arguments were provided at 0xFF00
    mov si, 0xFF00
.skip_arg_sp:
    cmp byte [si], ' '
    jne .arg_found
    inc si
    jmp .skip_arg_sp

.arg_found:
    cmp byte [si], 0
    je .no_args

    mov di, tib
.copy_arg:
    lodsb
    test al, al
    jz .arg_done
    stosb
    cmp di, tib + 240
    jb .copy_arg
.arg_done:
    mov byte [di], 10   ; append newline
    inc di
    mov [bufftop], di
.no_args:

    ; Print startup banner
    mov ah, 0x02        ; SYS_PUTS
    mov bx, msg_banner
    int 0x21

    ; Enter outer interpreter loop
    mov si, cold_start_thread
    NEXT

cold_start_thread:
    dw QUIT

; ==============================================================================
; Inner Interpreter (DOCOL, EXIT)
; ==============================================================================
align 2
DOCOL:
    sub bp, 2
    mov [bp], si
    lea si, [bx + 2]
    NEXT

defcode "EXIT", 4, 0, EXIT
    mov si, [bp]
    add bp, 2
    NEXT

; ==============================================================================
; Core Stack Manipulation Primitives
; ==============================================================================
defcode "DROP", 4, 0, DROP
    pop ax
    NEXT

defcode "SWAP", 4, 0, SWAP
    pop ax
    pop bx
    push ax
    push bx
    NEXT

defcode "DUP", 3, 0, DUP
    pop ax
    push ax
    push ax
    NEXT

defcode "OVER", 4, 0, OVER
    pop ax
    pop bx
    push bx
    push ax
    push bx
    NEXT

defcode "ROT", 3, 0, ROT
    pop ax              ; c
    pop bx              ; b
    pop cx              ; a
    push bx             ; b
    push ax             ; c
    push cx             ; a
    NEXT

defcode "-ROT", 4, 0, NROT
    pop ax              ; c
    pop bx              ; b
    pop cx              ; a
    push ax             ; c
    push cx             ; a
    push bx             ; b
    NEXT

defcode "2DROP", 5, 0, TWODROP
    pop ax
    pop ax
    NEXT

defcode "2DUP", 4, 0, TWODUP
    pop bx
    pop ax
    push ax
    push bx
    push ax
    push bx
    NEXT

defcode "?DUP", 4, 0, QDUP
    pop ax
    test ax, ax
    jz .z
    push ax
.z:
    push ax
    NEXT

defcode ">R", 2, 0, TOR
    pop ax
    sub bp, 2
    mov [bp], ax
    NEXT

defcode "R>", 2, 0, FROMR
    mov ax, [bp]
    add bp, 2
    push ax
    NEXT

defcode "R@", 2, 0, RFETCH
    mov ax, [bp]
    push ax
    NEXT

defcode "RSP@", 4, 0, RSPFETCH
    push bp
    NEXT

defcode "RSP!", 4, 0, RSPSTORE
    pop bp
    NEXT

defcode "RDROP", 5, 0, RDROP
    add bp, 2
    NEXT

defcode "DSP@", 4, 0, DSPFETCH
    mov ax, sp
    push ax
    NEXT

defcode "DSP!", 4, 0, DSPSTORE
    pop ax
    mov sp, ax
    NEXT

; ==============================================================================
; Arithmetic and Logical Primitives
; ==============================================================================
defcode "+", 1, 0, ADD
    pop bx
    pop ax
    add ax, bx
    push ax
    NEXT

defcode "-", 1, 0, SUB
    pop bx
    pop ax
    sub ax, bx
    push ax
    NEXT

defcode "*", 1, 0, MUL
    pop bx
    pop ax
    imul bx
    push ax
    NEXT

defcode "/MOD", 4, 0, DIVMOD
    pop bx
    pop ax
    cwd
    idiv bx
    push dx             ; remainder
    push ax             ; quotient
    NEXT

defcode "/", 1, 0, DIV
    pop bx
    pop ax
    cwd
    idiv bx
    push ax
    NEXT

defcode "MOD", 3, 0, MOD
    pop bx
    pop ax
    cwd
    idiv bx
    push dx
    NEXT

defcode "1+", 2, 0, INCR
    pop ax
    inc ax
    push ax
    NEXT

defcode "1-", 2, 0, DECR
    pop ax
    dec ax
    push ax
    NEXT

defcode "2+", 2, 0, INCR2
    pop ax
    add ax, 2
    push ax
    NEXT

defcode "2-", 2, 0, DECR2
    pop ax
    sub ax, 2
    push ax
    NEXT

defcode "AND", 3, 0, _AND
    pop bx
    pop ax
    and ax, bx
    push ax
    NEXT

defcode "OR", 2, 0, _OR
    pop bx
    pop ax
    or ax, bx
    push ax
    NEXT

defcode "XOR", 3, 0, _XOR
    pop bx
    pop ax
    xor ax, bx
    push ax
    NEXT

defcode "INVERT", 6, 0, INVERT
    pop ax
    not ax
    push ax
    NEXT

defcode "NEGATE", 6, 0, NEGATE
    pop ax
    neg ax
    push ax
    NEXT

; ==============================================================================
; Comparison Primitives (true = -1, false = 0)
; ==============================================================================
defcode "=", 1, 0, EQU
    pop bx
    pop ax
    cmp ax, bx
    jne .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode "<>", 2, 0, NEQU
    pop bx
    pop ax
    cmp ax, bx
    je .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode "<", 1, 0, LT
    pop bx
    pop ax
    cmp ax, bx
    jge .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode ">", 1, 0, GT
    pop bx
    pop ax
    cmp ax, bx
    jle .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode "<=", 2, 0, LE
    pop bx
    pop ax
    cmp ax, bx
    jg .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode ">=", 2, 0, GE
    pop bx
    pop ax
    cmp ax, bx
    jl .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode "0=", 2, 0, ZEQU
    pop ax
    test ax, ax
    jnz .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode "0<>", 3, 0, ZNEQU
    pop ax
    test ax, ax
    jz .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode "0<", 2, 0, ZLT
    pop ax
    test ax, ax
    jns .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

defcode "0>", 2, 0, ZGT
    pop ax
    cmp ax, 0
    jle .f
    mov ax, -1
    jmp .done
.f:
    xor ax, ax
.done:
    push ax
    NEXT

; ==============================================================================
; Memory Access Primitives
; ==============================================================================
defcode "!", 1, 0, STORE
    pop bx
    pop ax
    mov [bx], ax
    NEXT

defcode "@", 1, 0, FETCH
    pop bx
    push word [bx]
    NEXT

defcode "+!", 2, 0, ADDSTORE
    pop bx
    pop ax
    add [bx], ax
    NEXT

defcode "-!", 2, 0, SUBSTORE
    pop bx
    pop ax
    sub [bx], ax
    NEXT

defcode "C!", 2, 0, CSTORE
    pop bx
    pop ax
    mov [bx], al
    NEXT

defcode "C@", 2, 0, CFETCH
    pop bx
    mov al, [bx]
    xor ah, ah
    push ax
    NEXT

defcode "CMOVE", 5, 0, CMOVE
    pop cx
    pop di
    pop si
    rep movsb
    NEXT

; ==============================================================================
; Control Flow and Literals
; ==============================================================================
defcode "BRANCH", 6, 0, BRANCH
    add si, [si]
    NEXT

defcode "0BRANCH", 7, 0, ZBRANCH
    pop ax
    test ax, ax
    jz code_BRANCH
    add si, 2
    NEXT

defcode "LIT", 3, 0, LIT
    lodsw
    push ax
    NEXT

defcode "LITSTRING", 9, 0, LITSTRING
    lodsw               ; string length
    push si             ; string address
    push ax             ; length
    add si, ax          ; skip string
    inc si              ; align to 2
    and si, ~1
    NEXT

defcode "EXECUTE", 7, 0, EXECUTE
    pop bx
    jmp word [bx]

; ==============================================================================
; I/O & Console Output
; ==============================================================================
defcode "KEY", 3, 0, KEY
    call _KEY
    push ax
    NEXT

defcode "EMIT", 4, 0, EMIT
    pop ax
    mov ah, 0x01        ; SYS_PUTC
    int 0x21
    NEXT

defcode "CR", 2, 0, CR
    mov ax, 0x010A      ; AH=1, AL=10 (\n)
    int 0x21
    NEXT

defcode "SPACE", 5, 0, SPACE
    mov ax, 0x0120      ; AH=1, AL=' '
    int 0x21
    NEXT

defcode "SPACES", 6, 0, SPACES
    pop cx
.sp_loop:
    jcxz .sp_done
    mov ax, 0x0120
    int 0x21
    dec cx
    jmp .sp_loop
.sp_done:
    NEXT

defcode ".", 1, 0, DOT
    pop ax
    call _PRINT_INT
    mov ax, 0x0120
    int 0x21
    NEXT

defcode "U.", 2, 0, UDOT
    pop ax
    call _PRINT_UINT
    mov ax, 0x0120
    int 0x21
    NEXT

defcode ".S", 2, 0, DOTS
    mov ax, 0x013C      ; '<'
    int 0x21

    mov ax, [var_S0]
    sub ax, sp
    shr ax, 1           ; item count
    call _PRINT_UINT

    mov ax, 0x013E      ; '>'
    int 0x21
    mov ax, 0x0120      ; ' '
    int 0x21

    mov bx, [var_S0]
.dots_loop:
    cmp bx, sp
    jbe .dots_done
    sub bx, 2
    mov ax, [bx]
    push bx
    call _PRINT_INT
    mov ax, 0x0120
    int 0x21
    pop bx
    jmp .dots_loop
.dots_done:
    NEXT

defcode "TELL", 4, 0, TELL
    pop cx
    pop bx
.tell_loop:
    jcxz .tell_done
    mov al, [bx]
    mov ah, 0x01
    int 0x21
    inc bx
    dec cx
    jmp .tell_loop
.tell_done:
    NEXT

defcode '."', 2, F_IMMED, DOTQUOTE
    mov di, word_buf
    xor cx, cx
.dq_loop:
    call _KEY
    cmp al, '"'
    je .dq_done
    mov [di], al
    inc di
    inc cx
    jmp .dq_loop
.dq_done:
    mov di, LITSTRING
    call _COMMA
    mov di, cx
    call _COMMA

    ; Write characters into HERE
    mov bx, [var_HERE]
    push si
    mov si, word_buf
    push cx
    mov di, bx
    rep movsb
    pop cx
    pop si
    ; Align HERE to 2 bytes
    inc di
    and di, ~1
    mov [var_HERE], di

    mov di, TELL
    call _COMMA
    NEXT

defcode "WORDS", 5, 0, WORDS
    mov bx, [var_LATEST]
.words_loop:
    test bx, bx
    jz .words_done

    mov al, [bx + 2]
    test al, F_HIDDEN
    jnz .words_next

    and al, F_LENMASK
    xor ch, ch
    mov cl, al

    push bx
    lea bx, [bx + 3]
.w_emit:
    jcxz .w_sp
    mov al, [bx]
    mov ah, 0x01
    int 0x21
    inc bx
    dec cx
    jmp .w_emit
.w_sp:
    mov ax, 0x0120      ; ' '
    int 0x21
    pop bx

.words_next:
    mov bx, [bx]
    jmp .words_loop

.words_done:
    mov ax, 0x010A      ; \n
    int 0x21
    NEXT

defcode "BYE", 3, 0, BYE
    mov ah, 0x00        ; SYS_EXIT
    xor al, al
    int 0x21

; ==============================================================================
; Dictionary and Compiler Primitives
; ==============================================================================
defcode "FIND", 4, 0, FIND
    pop cx              ; length
    pop di              ; address
    call _FIND
    push ax             ; header address or 0
    NEXT

defcode ">CFA", 4, 0, TCFA
    pop di
    call _TCFA
    push di
    NEXT

defcode ">DFA", 4, 0, TDFA
    pop di
    call _TCFA
    add di, 2
    push di
    NEXT

defcode "CREATE", 6, 0, CREATE
    pop cx
    pop di
    call _CREATE
    NEXT

defcode ",", 1, 0, COMMA
    pop di
    call _COMMA
    NEXT

defcode "[", 1, F_IMMED, LBRAC
    mov word [var_STATE], 0
    NEXT

defcode "]", 1, 0, RBRAC
    mov word [var_STATE], 1
    NEXT

defcode "IMMEDIATE", 9, F_IMMED, IMMEDIATE
    mov bx, [var_LATEST]
    xor byte [bx + 2], F_IMMED
    NEXT

defcode "HIDDEN", 6, 0, HIDDEN
    pop bx
    xor byte [bx + 2], F_HIDDEN
    NEXT

defcode "'", 1, 0, TICK
    call _WORD
    call _FIND
    test ax, ax
    jz .tick_nf
    mov di, ax
    call _TCFA
    push di
    NEXT
.tick_nf:
    mov ah, 0x02
    mov bx, msg_tick_err
    int 0x21
    xor ax, ax
    push ax
    NEXT

defcode "CHAR", 4, 0, CHAR
    call _WORD
    mov al, [di]
    xor ah, ah
    push ax
    NEXT

defcode "WORD", 4, 0, F_WORD
    call _WORD
    push di
    push cx
    NEXT

defcode "NUMBER", 6, 0, NUMBER
    pop cx
    pop di
    call _NUMBER
    push ax
    push cx
    NEXT

; ==============================================================================
; Variables and Constants in Dictionary
; ==============================================================================
defvar "STATE", 5, 0, STATE, 0
defvar "HERE", 4, 0, HERE, 0
defvar "LATEST", 6, 0, LATEST, 0
defvar "S0", 2, 0, S0, 0xFEFE
defvar "BASE", 4, 0, BASE, 10

defconst "R0", 2, 0, RZ, 0xFAFE
defconst "DOCOL", 5, 0, __DOCOL, DOCOL
defconst "F_IMMED", 7, 0, __F_IMMED, F_IMMED
defconst "F_HIDDEN", 8, 0, __F_HIDDEN, F_HIDDEN
defconst "F_LENMASK", 9, 0, __F_LENMASK, F_LENMASK

; ==============================================================================
; High-Level Colon Definitions (defword)
; ==============================================================================
defword ":", 1, 0, COLON
    dw F_WORD
    dw CREATE
    dw LIT, DOCOL, COMMA
    dw LATEST, FETCH, HIDDEN
    dw RBRAC
    dw EXIT

defword ";", 1, F_IMMED, SEMICOLON
    dw LIT, EXIT, COMMA
    dw LATEST, FETCH, HIDDEN
    dw LBRAC
    dw EXIT

defword "IF", 2, F_IMMED, F_IF
    dw LIT, ZBRANCH, COMMA
    dw HERE, FETCH
    dw LIT, 0, COMMA
    dw EXIT

defword "THEN", 4, F_IMMED, THEN
    dw DUP
    dw HERE, FETCH, SWAP, SUB
    dw SWAP, STORE
    dw EXIT

defword "ELSE", 4, F_IMMED, F_ELSE
    dw LIT, BRANCH, COMMA
    dw HERE, FETCH
    dw LIT, 0, COMMA
    dw SWAP
    dw DUP
    dw HERE, FETCH, SWAP, SUB
    dw SWAP, STORE
    dw EXIT

defword "BEGIN", 5, F_IMMED, BEGIN
    dw HERE, FETCH
    dw EXIT

defword "UNTIL", 5, F_IMMED, UNTIL
    dw LIT, ZBRANCH, COMMA
    dw HERE, FETCH, SUB
    dw COMMA
    dw EXIT

defword "AGAIN", 5, F_IMMED, AGAIN
    dw LIT, BRANCH, COMMA
    dw HERE, FETCH, SUB
    dw COMMA
    dw EXIT

defword "WHILE", 5, F_IMMED, WHILE
    dw LIT, ZBRANCH, COMMA
    dw HERE, FETCH
    dw LIT, 0, COMMA
    dw EXIT

defword "REPEAT", 6, F_IMMED, REPEAT
    dw LIT, BRANCH, COMMA
    dw SWAP
    dw HERE, FETCH, SUB
    dw COMMA
    dw DUP
    dw HERE, FETCH, SWAP, SUB
    dw SWAP, STORE
    dw EXIT

defword "CONSTANT", 8, 0, CONSTANT
    dw F_WORD, CREATE
    dw LIT, DOCOL, COMMA
    dw LIT, LIT, COMMA
    dw COMMA
    dw LIT, EXIT, COMMA
    dw EXIT

defword "VARIABLE", 8, 0, VARIABLE
    dw F_WORD, CREATE
    dw LIT, DOCOL, COMMA
    dw LIT, LIT, COMMA
    dw HERE, FETCH, LIT, 6, ADD, COMMA
    dw LIT, EXIT, COMMA
    dw LIT, 0, COMMA
    dw EXIT

; ==============================================================================
; Outer Interpreter Loop (INTERPRET, QUIT)
; ==============================================================================
defcode "INTERPRET", 9, 0, INTERPRET
    ; Check for data stack underflow
    mov ax, [var_S0]
    cmp sp, ax
    jbe .stack_ok
    mov sp, ax
    mov ah, 0x02
    mov bx, msg_underflow
    int 0x21
    mov word [var_STATE], 0
    NEXT

.stack_ok:
    call _WORD          ; DI = word string, CX = length
    test cx, cx
    jnz .have_token

    ; End of line reached. If STATE == 0, print " ok\n"
    cmp word [var_STATE], 0
    jne .skip_ok
    mov ah, 0x02
    mov bx, msg_ok
    int 0x21
.skip_ok:
    NEXT

.have_token:
    push cx
    push di
    call _FIND          ; AX = header address, or 0
    pop di
    pop cx

    test ax, ax
    jz .try_number

    ; Word found in dictionary
    mov di, ax
    mov al, [di + 2]    ; flags byte
    push ax
    call _TCFA          ; DI = CFA
    pop ax

    ; If IMMEDIATE or interpreting (STATE == 0), execute now!
    test al, F_IMMED
    jnz .exec_now
    cmp word [var_STATE], 0
    je .exec_now

    ; Compiling: append CFA to HERE
    call _COMMA
    NEXT

.exec_now:
    mov bx, di
    jmp word [bx]

.try_number:
    call _NUMBER        ; AX = number, CX = unparsed chars remaining
    test cx, cx
    jnz .parse_error

    ; Valid number parsed
    cmp word [var_STATE], 0
    je .push_num

    ; Compiling: compile LIT, then value
    push ax
    mov di, LIT
    call _COMMA
    pop di
    call _COMMA
    NEXT

.push_num:
    push ax
    NEXT

.parse_error:
    ; Unknown word: print " <word> ?\n"
    mov ax, 0x0120      ; ' '
    int 0x21

    mov bx, word_buf
    mov cx, [word_buf_len]
.err_loop:
    jcxz .err_q
    mov al, [bx]
    mov ah, 0x01
    int 0x21
    inc bx
    dec cx
    jmp .err_loop
.err_q:
    mov ah, 0x02
    mov bx, msg_unknown
    int 0x21

    ; Reset interpreter state on error
    mov word [var_STATE], 0
    NEXT

defword "QUIT", 4, 0, QUIT
    dw RZ, RSPSTORE     ; reset return stack
    dw INTERPRET        ; interpret next word/line
    dw BRANCH, -4       ; loop forever back to INTERPRET

; ==============================================================================
; Internal Subroutines (Assembly Helpers)
; ==============================================================================

; --- _TCFA: (DI = header) -> returns DI = CFA ---
_TCFA:
    mov al, [di + 2]
    and al, F_LENMASK
    xor ah, ah
    lea di, [di + 4]
    add di, ax
    and di, ~1
    ret

; --- _COMMA: Appends DI to dictionary at [var_HERE] ---
_COMMA:
    mov bx, [var_HERE]
    mov [bx], di
    add word [var_HERE], 2
    ret

; --- _CREATE: (DI = name, CX = len) -> creates header at [var_HERE] ---
_CREATE:
    mov bx, [var_HERE]  ; new header address

    ; Link pointer
    mov ax, [var_LATEST]
    mov [bx], ax

    ; Length + flags byte (initially 0 flags)
    mov [bx + 2], cl

    ; Copy name characters (convert to uppercase)
    push di
    push si
    push cx
    lea si, [bx + 3]
.cr_name:
    jcxz .cr_namedone
    mov al, [di]
    cmp al, 'a'
    jb .cr_nou
    cmp al, 'z'
    ja .cr_nou
    sub al, 32
.cr_nou:
    mov [si], al
    inc di
    inc si
    dec cx
    jmp .cr_name
.cr_namedone:
    pop cx
    pop si
    pop di

    ; Align CFA address: (bx + 4 + cx) & ~1
    lea ax, [bx + 4]
    add ax, cx
    and ax, ~1

    ; Update LATEST to this header
    mov [var_LATEST], bx

    ; Update HERE to the aligned CFA address
    mov [var_HERE], ax
    ret

; --- _FIND: (DI = word, CX = len) -> returns AX = header or 0 ---
_FIND:
    push si
    mov bx, [var_LATEST]
.f_loop:
    test bx, bx
    jz .f_notfound

    mov al, [bx + 2]
    test al, F_HIDDEN
    jnz .f_next

    and al, F_LENMASK
    xor ah, ah
    cmp ax, cx
    jne .f_next

    ; Lengths match, compare characters case-insensitively
    push cx
    push di
    push bx
    lea si, [bx + 3]
.f_cmp:
    mov al, [di]
    mov dl, [si]
    cmp al, 'a'
    jb .f_u1
    cmp al, 'z'
    ja .f_u1
    sub al, 32
.f_u1:
    cmp dl, 'a'
    jb .f_u2
    cmp dl, 'z'
    ja .f_u2
    sub dl, 32
.f_u2:
    cmp al, dl
    jne .f_mismatch
    inc di
    inc si
    loop .f_cmp

    pop bx
    pop di
    pop cx
    pop si
    mov ax, bx          ; match found!
    ret

.f_mismatch:
    pop bx
    pop di
    pop cx

.f_next:
    mov bx, [bx]        ; link to previous word
    jmp .f_loop

.f_notfound:
    pop si
    xor ax, ax
    ret

; --- _NUMBER: (DI = str, CX = len) -> AX = number, CX = remaining unparsed ---
_NUMBER:
    test cx, cx
    jz .num_err

    push bx
    push dx
    push si

    xor ax, ax          ; accumulator = 0
    xor dx, dx          ; sign: 0 = positive, 1 = negative
    mov si, di

    ; Check for leading '-'
    cmp byte [si], '-'
    jne .num_loop
    cmp cx, 1
    je .num_err_pop
    inc dx              ; sign = 1
    inc si
    dec cx

.num_loop:
    mov bl, [si]
    xor bh, bh

    cmp bl, '0'
    jb .num_done
    cmp bl, '9'
    ja .num_alpha
    sub bl, '0'
    jmp .num_chkbase

.num_alpha:
    cmp bl, 'A'
    jb .num_lower
    cmp bl, 'Z'
    ja .num_lower
    sub bl, 'A' - 10
    jmp .num_chkbase

.num_lower:
    cmp bl, 'a'
    jb .num_done
    cmp bl, 'z'
    ja .num_done
    sub bl, 'a' - 10

.num_chkbase:
    cmp bx, [var_BASE]
    jae .num_done

    push dx
    mov dx, [var_BASE]
    mul dx              ; DX:AX = AX * BASE
    pop dx
    add ax, bx

    inc si
    dec cx
    jnz .num_loop

.num_done:
    test dx, dx
    jz .num_ok
    neg ax

.num_ok:
    pop si
    pop dx
    pop bx
    ret

.num_err_pop:
    pop si
    pop dx
    pop bx
.num_err:
    ret

; --- _KEY: Reads next char from tib, refills buffer if empty ---
_KEY:
    mov bx, [currkey]
    cmp bx, [bufftop]
    jb .have_char

    ; Buffer empty: read a new line from console
    mov di, tib
    xor cx, cx

.refill_loop:
    mov ah, 0x03        ; SYS_GETC
    int 0x21

    mov dl, al

    ; Enter (\r or \n)
    cmp dl, 13
    je .refill_enter
    cmp dl, 10
    je .refill_enter

    ; Backspace (\b or 127)
    cmp dl, 8
    je .refill_bs
    cmp dl, 127
    je .refill_bs

    ; Ctrl+D or Ctrl+C
    cmp dl, 4
    je .refill_eof
    cmp dl, 3
    je .refill_eof

    ; Filter non-printable
    cmp dl, ' '
    jb .refill_loop
    cmp dl, 126
    ja .refill_loop

    cmp cx, 240
    jae .refill_loop

    mov [di], dl
    inc di
    inc cx

    mov al, dl
    mov ah, 0x01        ; SYS_PUTC (echo)
    int 0x21
    jmp .refill_loop

.refill_bs:
    test cx, cx
    jz .refill_loop
    dec di
    dec cx
    mov ax, 0x0108      ; backspace
    int 0x21
    jmp .refill_loop

.refill_enter:
    mov ax, 0x010A      ; newline echo
    int 0x21

    mov byte [di], 10   ; store newline
    inc di
    inc cx

    mov word [currkey], tib
    mov [bufftop], di

.have_char:
    mov bx, [currkey]
    mov al, [bx]
    inc word [currkey]
    xor ah, ah
    ret

.refill_eof:
    mov ah, 0x00        ; SYS_EXIT
    xor al, al
    int 0x21

; --- _WORD: Parses next whitespace-delimited token ---
; Returns DI = word_buf, CX = length (0 if end of line)
_WORD:
.w_start:
    mov bx, [currkey]
    cmp bx, [bufftop]
    jae .w_refill

    mov al, [bx]
    inc word [currkey]

    ; Check for end of line (\n)
    cmp al, 10
    je .w_eol
    cmp al, 13
    je .w_eol

    ; Check for whitespace
    cmp al, ' '
    je .w_start
    cmp al, 9
    je .w_start

    ; Check for line comment: \
    cmp al, '\'
    je .w_comment_line

    ; Check for block comment: (
    cmp al, '('
    je .w_comment_paren

    ; Start of word!
    mov di, word_buf
    mov [di], al
    inc di
    mov cx, 1

.w_collect:
    mov bx, [currkey]
    cmp bx, [bufftop]
    jae .w_finish

    mov al, [bx]
    ; If whitespace, word delimiter found!
    cmp al, ' '
    je .w_delim_space
    cmp al, 9
    je .w_delim_space
    cmp al, 10
    je .w_delim_nl
    cmp al, 13
    je .w_delim_nl

    ; Character belongs to token
    inc word [currkey]
    cmp cx, 60
    jae .w_collect      ; truncate tokens > 60 chars
    mov [di], al
    inc di
    inc cx
    jmp .w_collect

.w_delim_space:
    inc word [currkey]  ; consume trailing space
    jmp .w_finish

.w_delim_nl:
    ; Do NOT consume \n; let next call to _WORD see it as end-of-line!
    jmp .w_finish

.w_finish:
    mov byte [di], 0
    mov [word_buf_len], cx
    mov di, word_buf
    ret

.w_eol:
    xor cx, cx
    mov [word_buf_len], cx
    mov di, word_buf
    ret

.w_refill:
    ; Buffer empty: call _KEY to read next line
    call _KEY
    ; _KEY placed characters in tib and returned first character in AL.
    ; Rewind currkey by 1 so .w_start can inspect it.
    dec word [currkey]
    jmp .w_start

.w_comment_line:
    ; Skip until \n
    mov bx, [currkey]
    cmp bx, [bufftop]
    jae .w_eol
    mov al, [bx]
    inc word [currkey]
    cmp al, 10
    jne .w_comment_line
    jmp .w_eol

.w_comment_paren:
    ; Skip until ')'
    mov bx, [currkey]
    cmp bx, [bufftop]
    jae .w_paren_refill
    mov al, [bx]
    inc word [currkey]
    cmp al, ')'
    je .w_start
    jmp .w_comment_paren

.w_paren_refill:
    call _KEY
    cmp al, ')'
    je .w_start
    jmp .w_comment_paren

; --- Number formatting helpers ---
_PRINT_INT:
    test ax, ax
    jns _PRINT_UINT
    push ax
    mov ax, 0x012D      ; '-'
    int 0x21
    pop ax
    neg ax

_PRINT_UINT:
    push bx
    push cx
    push dx
    xor cx, cx
    mov bx, [var_BASE]

.pu_div:
    xor dx, dx
    div bx
    push dx
    inc cx
    test ax, ax
    jnz .pu_div

.pu_emit:
    pop ax
    cmp al, 10
    jb .pu_dec
    add al, 'A' - 10
    jmp .pu_out
.pu_dec:
    add al, '0'
.pu_out:
    mov ah, 0x01
    int 0x21
    loop .pu_emit

    pop dx
    pop cx
    pop bx
    ret

; ==============================================================================
; Static Data and Messages
; ==============================================================================
msg_banner:
    db "JONESFORTH (8088 16-bit)", 10
    db "Type 'bye' to exit, 'words' for dictionary.", 10, 0

msg_ok:
    db " ok", 10, 0

msg_unknown:
    db " ?", 10, 0

msg_underflow:
    db "Stack underflow!", 10, 0

msg_tick_err:
    db "Word not found for '", 10, 0

; ==============================================================================
; Buffers and Static Dictionary End
; ==============================================================================
currkey:        dw 0
bufftop:        dw 0
word_buf_len:   dw 0
word_buf:       times 64 db 0
tib:            times 256 db 0

align 2
last_word_header equ link
end_of_static_dict:
