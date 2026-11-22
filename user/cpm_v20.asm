; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; NEC V20 Hardware 8080 Emulation Helpers for hobbyOS CPM.BIN

[BITS 16]
CPU 8086

extern _cpm_bdos_dispatch
extern _cpm_bios_dispatch
extern _s_cpm_running

global _v20_enter_8080
global _v20_bdos_isr
global _v20_bios_isr
global _v20_exit_isr
global _cpm_vram_init
global _cpm_vram_write_cell

section _TEXT class=CODE

s_v20_host_ds:   dw 0
s_v20_host_ss:   dw 0
s_v20_host_sp:   dw 0

s_v20_cpm_ss:    dw 0
s_v20_cpm_sp:    dw 0

s_v20_bdos_a:    db 0
s_v20_bdos_func: db 0
s_v20_bios_func: db 0
s_v20_bios_arg:  db 0
s_v20_ret_a:     db 0
s_v20_ret_hl:    dw 0

; void v20_enter_8080(uint16_t cpm_seg);
_v20_enter_8080:
    push bp
    mov bp, sp
    push ds
    push es
    push si
    push di

    ; Save host stack and data segments
    mov ax, ds
    mov cs:[s_v20_host_ds], ax
    mov ax, ss
    mov cs:[s_v20_host_ss], ax
    mov ax, sp
    mov cs:[s_v20_host_sp], ax

    ; Set up 8080 segments: DS = SS = ES = cpm_seg
    mov ax, [bp+4]      ; cpm_seg
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE      ; Native SP in cpm_seg (for BRKEM / CALLN stack frames)
    mov bp, 0xFC00      ; 8080 SP (mapped to BP in 8080 mode)

    ; Clear 8080 registers
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx

    ; Enter 8080 emulation mode via vector 0xE0
    ; Opcode: 0x0F, 0xFF, 0xE0 (BRKEM 0xE0)
    db 0x0F, 0xFF, 0xE0

_v20_return_point:
    ; Restore host data and stack segments
    mov ax, cs:[s_v20_host_ds]
    mov ds, ax
    mov es, ax
    mov ax, cs:[s_v20_host_ss]
    mov ss, ax
    mov sp, cs:[s_v20_host_sp]

    pop di
    pop si
    pop es
    pop ds
    pop bp
    ret

; Interrupt handler for CALLN 0xE1 (BDOS call)
_v20_bdos_isr:
    ; On entry from CALLN 0xE1:
    ; 8080 registers are mapped:
    ;   CL = func
    ;   DX = de
    ;   BX = hl
    ;   AL = a
    ;   BP = 8080 stack pointer
    ; SS:SP = cpm_seg:SP (native stack holding CALLN return frame: IP, CS, FLAGS)

    mov cs:[s_v20_bdos_a], al   ; Preserve A input parameter
    mov cs:[s_v20_bdos_func], cl ; Preserve func input parameter

    ; Save registers
    push ds
    push es
    push bx             ; save HL
    push cx             ; save BC
    push dx             ; save DE
    push si
    push di
    push bp             ; save 8080 stack pointer

    ; Save CPM stack
    mov cs:[s_v20_cpm_ss], ss
    mov cs:[s_v20_cpm_sp], sp

    ; Switch to host stack and data segments
    mov ax, cs:[s_v20_host_ds]
    mov ds, ax
    mov es, ax
    mov ax, cs:[s_v20_host_ss]
    mov ss, ax
    mov sp, cs:[s_v20_host_sp]

    sti                 ; Enable interrupts for host operations

    ; Push parameters right-to-left for __cdecl:
    ; cpm_bdos_dispatch(uint8_t func, uint16_t de, uint16_t hl, uint8_t a)
    xor ah, ah
    mov al, cs:[s_v20_bdos_a]
    push ax             ; a (uint8_t)
    push bx             ; hl (uint16_t)
    push dx             ; de (uint16_t)
    xor ch, ch
    push cx             ; func (uint8_t)

    call _cpm_bdos_dispatch
    add sp, 8

    ; Result in AX: AL = A, AX = HL (for functions returning HL)
    mov cs:[s_v20_ret_a], al
    mov cs:[s_v20_ret_hl], ax

    ; Check if CP/M execution was terminated (s_cpm_running == 0)
    cmp word [_s_cpm_running], 0
    je _v20_exit_isr

    cli                 ; Protect stack switch

    ; Switch back to CPM stack
    mov ax, cs:[s_v20_cpm_ss]
    mov ss, ax
    mov sp, cs:[s_v20_cpm_sp]

    ; Restore registers
    pop bp              ; restore 8080 BP
    pop di
    pop si
    pop dx              ; restore DE
    pop cx              ; restore BC
    pop bx              ; restore caller's HL!
    pop es
    pop ds

    ; If func was 27 or 31, return new HL in BX
    cmp byte cs:[s_v20_bdos_func], 27
    je .ret_hl
    cmp byte cs:[s_v20_bdos_func], 31
    je .ret_hl
    mov bl, cs:[s_v20_ret_a] ; CP/M 2.2 standard: single-byte results in A are mirrored in L (BL)
    jmp .ret_done

.ret_hl:
    mov bx, cs:[s_v20_ret_hl]

.ret_done:
    mov al, cs:[s_v20_ret_a] ; Restore A return value!

    ; Return to 8080 emulation mode
    iret

; Interrupt handler for CALLN 0xE3 (Direct BIOS call)
_v20_bios_isr:
    ; AL = func, CL = arg/char
    mov cs:[s_v20_bios_func], al ; Preserve BIOS func (2=CONST, 3=CONIN, 4=CONOUT)
    mov cs:[s_v20_bios_arg], cl  ; Preserve BIOS arg/char

    push ds
    push es
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov cs:[s_v20_cpm_ss], ss
    mov cs:[s_v20_cpm_sp], sp

    mov ax, cs:[s_v20_host_ds]
    mov ds, ax
    mov es, ax
    mov ax, cs:[s_v20_host_ss]
    mov ss, ax
    mov sp, cs:[s_v20_host_sp]

    sti

    ; Push parameters right-to-left: cpm_bios_dispatch(uint8_t func, uint8_t c)
    xor ah, ah
    mov al, cs:[s_v20_bios_arg]
    push ax             ; 2nd param: c

    mov al, cs:[s_v20_bios_func]
    push ax             ; 1st param: func

    call _cpm_bios_dispatch
    add sp, 4

    mov cs:[s_v20_ret_a], al

    cli

    mov ax, cs:[s_v20_cpm_ss]
    mov ss, ax
    mov sp, cs:[s_v20_cpm_sp]

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop es
    pop ds

    mov al, cs:[s_v20_ret_a]
    iret

; Interrupt handler for CALLN 0xE2 (Exit / Warm Boot)
_v20_exit_isr:
    ; Switch directly to host stack and exit back to caller of v20_enter_8080
    mov word [_s_cpm_running], 0
    mov ax, cs:[s_v20_host_ds]
    mov ds, ax
    mov es, ax
    mov ax, cs:[s_v20_host_ss]
    mov ss, ax
    mov sp, cs:[s_v20_host_sp]

    pop di
    pop si
    pop es
    pop ds
    pop bp
    ret

s_cpm_vram_seg: dw 0xB800
s_cpm_has_snow: dw 0

; void cpm_vram_init(uint16_t vram_seg, uint16_t has_snow);
_cpm_vram_init:
    push bp
    mov bp, sp
    mov ax, [bp+4]
    mov cs:[s_cpm_vram_seg], ax
    mov ax, [bp+6]
    mov cs:[s_cpm_has_snow], ax
    pop bp
    ret

; void cpm_vram_write_cell(uint16_t idx, uint16_t val);
_cpm_vram_write_cell:
    push bp
    mov bp, sp
    push bx
    push dx
    push di
    push es

    mov ax, cs:[s_cpm_vram_seg]
    mov es, ax
    mov di, [bp+4]          ; cell idx (0..1999)
    shl di, 1               ; byte offset in VRAM
    mov bx, [bp+6]          ; (attr << 8) | ch

    cmp word cs:[s_cpm_has_snow], 0
    je .write_fast

    mov dx, 0x03DA

.wait_active:
    in  al, dx
    test al, 0x08           ; In vertical blanking?
    jnz .write_fast
    test al, 0x01           ; Wait for active display before catching hblank
    jnz .wait_active

    cli
.wait_hblank:
    in  al, dx
    test al, 0x01           ; Wait for horizontal blanking to begin
    jz  .wait_hblank

.write_fast:
    mov [es:di], bx
    sti

    pop es
    pop di
    pop dx
    pop bx
    pop bp
    ret
