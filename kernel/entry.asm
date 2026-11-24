; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.

[BITS 16]
CPU 8086

extern _kmain
extern _syscall_dispatch
extern _kbd_irq_handler
extern _xms_dispatch
extern _sound_bg_tick
extern _gfx_cga_lcd_sync

section _TEXT class=CODE

global _start
global _boot_drive_num
_start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov [_boot_drive_num], dl

    ; Read base memory size in KB from BIOS Data Area 0040:0013h
    xor bx, bx
    mov es, bx
    mov ax, [es:0x0413]     ; BDA base memory in KB (e.g. 640 or 639)
    mov bx, cs
    mov es, bx              ; Restore ES = CS

    cmp ax, 576
    jbe .fallback_mem
    cmp ax, 640
    jbe .got_mem
    mov ax, 640
.got_mem:
    sub ax, 576             ; KB within 0x9000 segment
    mov cx, 1024
    mul cx                  ; DX:AX = AX * 1024
    sub ax, 2               ; Word-aligned top of stack
    jmp .check_cap
.fallback_mem:
    mov ax, 0xFBFE          ; Safe fallback: 639 KB - 2
.check_cap:
    cmp ax, 0xFBFE
    jbe .sp_ok
    mov ax, 0xFBFE          ; Never exceed 639 KB (0xFC00) to protect XT-IDE / EBDA
.sp_ok:
    mov sp, ax
    mov [_kernel_saved_sp], ax
    sti
    cld
    call _kmain
.hang:
    hlt
    jmp .hang

_boot_drive_num: db 0x80

; uint8_t bios_get_boot_drive(void)
global _bios_get_boot_drive
_bios_get_boot_drive:
    mov al, [_boot_drive_num]
    xor ah, ah
    ret

; void outb(uint16_t port, uint8_t val)
global _outb
_outb:
    push bp
    mov bp, sp
    mov dx, [bp+4]
    mov al, [bp+6]
    out dx, al
    pop bp
    ret

; uint8_t inb(uint16_t port)
global _inb
_inb:
    push bp
    mov bp, sp
    mov dx, [bp+4]
    in al, dx
    xor ah, ah
    pop bp
    ret

; int fpu_8087_init(void) -> returns 1 if 8087 present, 0 otherwise
global _fpu_8087_init
_fpu_8087_init:
    push bp
    mov bp, sp
    sub sp, 2
    mov word [bp-2], 0x5A5A
    fninit
    jmp short $+2
    fnstsw [bp-2]
    jmp short $+2
    cmp byte [bp-2], 0
    jne .no_fpu
    mov word [bp-2], 0x5A5A
    fnstcw [bp-2]
    jmp short $+2
    mov ax, [bp-2]
    and ax, 0x103F
    cmp ax, 0x003F
    jne .no_fpu
    mov ax, 1
    jmp .done
.no_fpu:
    xor ax, ax
.done:
    mov sp, bp
    pop bp
    ret

; int bios_disk_rw(uint8_t drive, uint8_t is_write, uint16_t cyl, uint8_t head, uint8_t sec, void __far *buf)
global _bios_disk_rw
_bios_disk_rw:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push es

    mov ax, [bp+8]          ; cyl
    mov ch, al              ; low 8 bits of cylinder
    ror ah, 1
    ror ah, 1
    and ah, 0xC0
    mov cl, [bp+12]         ; sector (1..63)
    and cl, 0x3F
    or  cl, ah              ; combine high 2 bits of cylinder

    mov dh, [bp+10]         ; head
    mov dl, [bp+4]          ; drive (0x00, 0x80, 0x81, etc.)

    mov bx, [bp+14]         ; buffer offset
    mov es, [bp+16]         ; buffer segment

    mov ah, 0x02            ; BIOS read sectors
    cmp byte [bp+6], 0
    je  .do_io
    mov ah, 0x03            ; BIOS write sectors
.do_io:
    mov al, 0x01            ; 1 sector
    int 0x13
    jc  .err
    xor ax, ax
    jmp .done
.err:
    mov al, ah
    xor ah, ah
.done:
    pop es
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; int bios_disk_rw_lba(uint8_t drive, uint8_t is_write, uint32_t lba, uint16_t spt, uint16_t heads, void __far *buf)
global _bios_disk_rw_lba
_bios_disk_rw_lba:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push es

    ; Divide 32-bit LBA by sectors_per_track
    mov ax, [bp+8]          ; LBA low word
    mov dx, [bp+10]         ; LBA high word
    mov bx, [bp+12]         ; spt
    div bx                  ; AX = track, DX = sector - 1
    inc dx                  ; DX = sector (1..63)
    push dx                 ; save sector on stack

    ; Divide track (AX) by heads
    xor dx, dx
    mov bx, [bp+14]         ; heads
    div bx                  ; AX = cylinder, DX = head
    mov dh, dl              ; DH = head

    pop cx                  ; CL = sector (1..63)
    and cl, 0x3F

    ; Encode CH/CL
    mov ch, al              ; CH = low 8 bits of cylinder
    ror ah, 1
    ror ah, 1
    and ah, 0xC0
    or  cl, ah              ; combine high 2 bits of cylinder with sector

    mov dl, [bp+4]          ; drive

    mov bx, [bp+16]         ; buffer offset
    mov es, [bp+18]         ; buffer segment

    mov ah, 0x02            ; BIOS read sectors
    cmp byte [bp+6], 0
    je  .do_lba_io
    mov ah, 0x03            ; BIOS write sectors
.do_lba_io:
    mov al, 0x01            ; 1 sector
    int 0x13
    jc  .lba_err
    xor ax, ax
    jmp .lba_done
.lba_err:
    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .lba_done
    mov ax, 1
.lba_done:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; int bios_disk_reset(uint8_t drive)
global _bios_disk_reset
_bios_disk_reset:
    push bp
    mov bp, sp
    push dx
    mov dl, [bp+4]          ; drive
    xor ah, ah              ; reset disk subsystem
    int 0x13
    xor ax, ax
    pop dx
    pop bp
    ret

; int bios_disk_set_media_type(uint8_t drive, uint16_t num_cyls, uint8_t spt)
; Sets floppy media transfer rate and parameter table on AT BIOS (INT 0x13, AH=0x18)
global _bios_disk_set_media_type
_bios_disk_set_media_type:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push di
    push es

    mov dl, [bp+4]          ; drive
    mov ax, [bp+6]          ; num_cyls
    dec ax                  ; max cylinder (num_cyls - 1)
    mov ch, al              ; CH = low 8 bits
    ror ah, 1
    ror ah, 1
    and ah, 0xC0
    mov cl, [bp+8]          ; spt
    and cl, 0x3F
    or  cl, ah              ; CL = spt | cyl_hi

    mov ah, 0x18            ; INT 0x13, AH=0x18: Set Media Type for Format
    int 0x13
    jnc .smt_ok

    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .smt_done
    mov ax, 1
    jmp .smt_done

.smt_ok:
    xor ax, ax
.smt_done:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; int bios_disk_format_track(uint8_t drive, uint16_t cyl, uint8_t head, void __far *table)
; Formats 1 track (cylinder, head) using INT 0x13, AH=0x05
global _bios_disk_format_track
_bios_disk_format_track:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push es

    mov dl, [bp+4]          ; drive
    mov dh, [bp+8]          ; head
    mov ax, [bp+6]          ; cyl
    mov ch, al              ; low 8 bits of cylinder
    ror ah, 1
    ror ah, 1
    and ah, 0xC0
    mov cl, ah              ; high 2 bits of cylinder in CL (bits 7-6)

    mov bx, [bp+10]         ; table offset
    mov es, [bp+12]         ; table segment

    mov ah, 0x05            ; BIOS format track
    xor al, al              ; interleave
    int 0x13
    jnc .fmt_ok

    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .fmt_done
    mov ax, 1
    jmp .fmt_done

.fmt_ok:
    xor ax, ax
.fmt_done:
    pop es
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; int bios_get_drive_params(uint8_t drive, uint16_t *cyls, uint8_t *heads, uint8_t *spt, uint8_t *drives)
global _bios_get_drive_params
_bios_get_drive_params:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push es

    mov dl, [bp+4]          ; drive
    mov ah, 0x08            ; Get Drive Parameters
    xor di, di
    mov es, di
    int 0x13
    jc  .err

    push dx                 ; [sp+2] = DX (DH=max head, DL=num drives)
    push cx                 ; [sp]   = CX (CH=cyl low, CL=cyl hi + spt)

    ; Calculate total cylinders: max_cyl + 1
    mov al, cl
    rol al, 1
    rol al, 1
    and ax, 0x0003
    mov ah, al
    mov al, ch
    inc ax                  ; AX = total cylinders

    mov si, [bp+6]          ; cyls ptr
    test si, si
    jz  .no_cyls
    mov [si], ax
.no_cyls:

    pop cx                  ; restore CX
    pop dx                  ; restore DX

    ; Calculate heads: DH + 1
    mov al, dh
    inc al
    mov si, [bp+8]          ; heads ptr
    test si, si
    jz  .no_heads
    mov [si], al
.no_heads:

    ; Calculate spt: CL & 0x3F
    mov al, cl
    and al, 0x3F
    mov si, [bp+10]         ; spt ptr
    test si, si
    jz  .no_spt
    mov [si], al
.no_spt:

    ; Number of drives: DL
    mov al, dl
    mov si, [bp+12]         ; drives ptr
    test si, si
    jz  .no_drives
    mov [si], al
.no_drives:

    xor ax, ax              ; return 0 (success)
    jmp .done

.err:
    mov al, ah              ; return BIOS error status in AX
    xor ah, ah
    test ax, ax
    jnz .done
    mov ax, 1               ; ensure non-zero if AH was 0
.done:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; void cpu_halt(void) -> enables interrupts and halts CPU until next IRQ
global _cpu_halt
_cpu_halt:
    sti
    hlt
    ret

; void cpu_reboot(void) -> IBM PC/XT BIOS warm boot (skip RAM test)
global _cpu_reboot
_cpu_reboot:
    cli
    mov al, 0x20
    out 0x20, al            ; Send EOI to PIC
    mov ax, 0x0040
    mov ds, ax
    mov word [0x0072], 0x1234 ; BDA warm boot flag
    jmp 0xFFFF:0x0000       ; Jump to BIOS reset vector

; uint16_t cpu_detect_type(void) -> returns CPU_TYPE_* (0..7)
global _cpu_detect_type
_cpu_detect_type:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    push cs
    pop ds
    push cs
    pop es

    ; Save original flags in CX
    pushf
    pop cx

    ; Step 1: Check bits 12-15 of FLAGS.
    ; On 8086/8088/V20/V30/186/188, bits 12-15 are always set to 1 (0xF000).
    ; On 80286+, bits 12-15 are cleared to 0 in real mode.
    mov ax, cx
    and ax, 0x0FFF      ; try to clear bits 12-15
    push ax
    popf
    pushf
    pop ax
    and ax, 0xF000
    cmp ax, 0xF000
    je .is_8086_class

    ; Step 2: 80286 vs 80386+.
    ; Try to set bits 12-14 (IOPL). On 80286 real mode, they remain 0.
    ; On 80386+, they can be set in real mode.
    mov ax, cx
    or ax, 0x7000       ; try to set bits 12-14
    push ax
    popf
    pushf
    pop ax
    and ax, 0x7000
    jz .is_286
    mov ax, 7           ; CPU_TYPE_80386
    jmp .done
.is_286:
    mov ax, 6           ; CPU_TYPE_80286
    jmp .done

.is_8086_class:
    ; Restore original flags before proceeding
    push cx
    popf

    ; Step 3: Check for NEC V20/V30 using AAD immediate operand behavior.
    ; AAD (0xD5 imm8) calculates AL = (AH * imm8) + AL.
    ; Intel 8086/8088/186: uses the immediate operand (0x01 * 0x10 + 0x01 = 0x11).
    ; NEC V20/V30: ignores immediate operand and always multiplies by 10 (0x01 * 0x0A + 0x01 = 0x0B).
    mov ax, 0x0101
    db 0xD5, 0x10       ; AAD 0x10
    cmp al, 0x0B
    je .is_nec

    ; Step 4: Distinguish Intel 8086/8088 from Intel 80186/80188.
    ; Shift count masking test:
    ; 8086/8088 shifts by 33 -> AL = 0.
    ; 80186/80188 masks shift count to 5 bits (33 & 0x1F = 1) -> AL = 2.
    mov al, 1
    mov cl, 33
    shl al, cl
    test al, al
    jnz .is_186_series

    ; Intel 8088 / 8086
    xor bx, bx          ; Base = CPU_TYPE_8088 (0)
    jmp .test_queue

.is_nec:
    mov bx, 2           ; Base = CPU_TYPE_V20 (2)
    jmp .test_queue

.is_186_series:
    mov bx, 4           ; Base = CPU_TYPE_80188 (4)

.test_queue:
    ; Step 5: Prefetch queue size test (8-bit bus vs 16-bit bus).
    ; 8088, V20, 80188: 4-byte prefetch queue -> DX remains 0.
    ; 8086, V30, 80186: 6-byte prefetch queue -> DX increments to 1.
    std
    mov di, .q_end
    mov al, 0xFB        ; Opcode for STI
    mov cx, 3
    xor dx, dx
    cli
    rep stosb
    cld
    nop
    nop
    nop
.q_target:
    inc dx              ; 0x42
    nop                 ; 0x90
.q_end:
    sti                 ; 0xFB

    ; Restore original instructions at .q_target, .q_target+1, .q_target+2
    mov byte [cs:.q_target], 0x42
    mov byte [cs:.q_target+1], 0x90
    mov byte [cs:.q_target+2], 0xFB
    sti

    mov ax, bx
    add ax, dx

.done:
    ; Restore original flags (restores original IF)
    push cx
    popf

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; void kbd_install_vector(void) -> installs _isr_int09 at INT 0x09 (0000:0024)
global _kbd_install_vector
_kbd_install_vector:
    push es
    xor ax, ax
    mov es, ax
    cli
    mov word [es:0x0024], _isr_int09
    mov word [es:0x0026], cs
    sti
    pop es
    ret

; INT 0x09 (IRQ 1) PC/XT Hardware Keyboard Interrupt Handler
global _isr_int09
_isr_int09:
    push es
    push ds
    push bp
    push di
    push si
    push dx
    push cx
    push bx
    push ax

    mov ax, cs
    mov ds, ax
    mov es, ax
    cld

    in  al, 0x60            ; Read raw scancode from XT 8255 Port A
    xor ah, ah
    push ax                 ; Pass scancode to _kbd_irq_handler(uint8_t raw)

    in  al, 0x61            ; Pulse bit 7 of 8255 Port B to acknowledge XT keyboard
    mov ah, al
    or  al, 0x80
    out 0x61, al
    mov al, ah
    out 0x61, al

    call _kbd_irq_handler
    add sp, 2

    mov bx, ax              ; Save action (1 = abort requested)

    mov al, 0x20            ; Send EOI to 8259 PIC
    out 0x20, al

    cmp bx, 1               ; Abort requested?
    jne .done_iret

    ; Check if interrupted code was executing in user space (SS != CS)
    mov ax, ss
    mov cx, cs
    cmp ax, cx
    je .pending_abort

    ; Interrupted in user space! Exit user mode immediately.
    mov ax, 130
    push ax
    call _proc_exit_usermode
    ; _proc_exit_usermode never returns here

.pending_abort:
    mov word [_ctrl_c_abort_pending], 1

.done_iret:
    pop ax
    pop bx
    pop cx
    pop dx
    pop si
    pop di
    pop bp
    pop ds
    pop es
    iret

; Video controller dynamic state (updated by con_init)
global _video_vram_seg
global _video_has_snow
global _video_status_port

_video_vram_seg:    dw 0xB800
_video_has_snow:    dw 1
_video_status_port: dw 0x03DA

; void cga_write_cell(uint16_t cell_idx, uint16_t cell_val)
; Writes 1 character/attribute word to text VRAM (0xB800 for CGA/VGA or 0xB000 for MDA)
global _cga_write_cell
_cga_write_cell:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push di
    push es

    mov ax, [_video_vram_seg]
    mov es, ax
    mov di, [bp+4]          ; cell_idx (0..1999)
    shl di, 1               ; byte offset in VRAM
    mov bx, [bp+6]          ; cell_val ((attr << 8) | ch)

    cmp word [_video_has_snow], 0
    je .write_now           ; On MDA, EGA, VGA: direct fast write without snow wait

    mov dx, [_video_status_port]

    mov cx, 0x0FFF
.wait_active:
    in  al, dx
    test al, 0x08           ; Vertical retrace active? Safe to write immediately
    jnz .write_now
    test al, 0x01           ; Wait for active scanline before catching rising hblank edge
    jz  .active_ok
    loop .wait_active
    jmp .write_now          ; Timeout safeguard -> write anyway

.active_ok:
    cli
    mov cx, 0x0FFF
.wait_hblank:
    in  al, dx
    test al, 0x09           ; Horizontal blanking (0x01) or Vertical retrace (0x08)
    jnz .write_now
    loop .wait_hblank

.write_now:
    mov [es:di], bx
    sti

    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; void cga_sync_copy(const uint16_t *src, uint16_t word_count)
; Copies shadow buffer words from DS:src to text VRAM
global _cga_sync_copy
_cga_sync_copy:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push es

    mov ax, [_video_vram_seg]
    mov es, ax
    xor di, di
    mov si, [bp+4]          ; src pointer in kernel DS
    mov cx, [bp+6]          ; word_count

    cmp word [_video_has_snow], 0
    jne .snow_copy

    cld
    rep movsw
    jmp .done

.snow_copy:
    mov dx, [_video_status_port]

.copy_loop:
    mov bx, [si]            ; Pre-fetch word from system RAM before waiting for retrace
    add si, 2

    push cx
    mov cx, 0x0FFF
.wait_disp:
    in  al, dx
    test al, 0x08           ; Vertical retrace active? Write immediately
    jnz .store_word_pop
    test al, 0x01
    jz  .disp_ok
    loop .wait_disp
    jmp .store_word_pop

.disp_ok:
    cli
    mov cx, 0x0FFF
.wait_retrace:
    in  al, dx
    test al, 0x09
    jnz .store_word_pop
    loop .wait_retrace

.store_word_pop:
    pop cx
.store_word:
    mov [es:di], bx
    sti
    add di, 2
    loop .copy_loop

.done:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; uint16_t bios_get_vga_dcc(void) -> returns AL=0x1A, AH=BL if VGA present, 0 otherwise
global _bios_get_vga_dcc
_bios_get_vga_dcc:
    push bp
    mov bp, sp
    push bx
    mov ax, 0x1A00
    int 0x10
    cmp al, 0x1A
    jne .not_vga
    mov ah, bl
    pop bx
    pop bp
    ret
.not_vga:
    xor ax, ax
    pop bx
    pop bp
    ret

; uint16_t bios_get_ega_info(void) -> returns AL=BL, AH=BH (if AL != 0x10, EGA present)
global _bios_get_ega_info
_bios_get_ega_info:
    push bp
    mov bp, sp
    push bx
    push cx
    mov ah, 0x12
    mov bl, 0x10
    int 0x10
    mov al, bl
    mov ah, bh
    pop cx
    pop bx
    pop bp
    ret

; uint8_t bios_get_video_mode(void) -> returns current video mode in AL
global _bios_get_video_mode
_bios_get_video_mode:
    push bp
    mov bp, sp
    push bx
    mov ah, 0x0F
    int 0x10
    xor ah, ah
    pop bx
    pop bp
    ret

; void bios_set_video_mode(uint8_t mode) -> calls INT 0x10 AH=0x00
global _bios_set_video_mode
_bios_set_video_mode:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    xor ah, ah
    mov al, [bp+4]
    int 0x10

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; void bios_scroll_up(uint8_t lines, uint8_t attr, uint8_t top, uint8_t left, uint8_t bottom, uint8_t right)
; INT 0x10, AH=0x06 (Scroll Window Up)
global _bios_scroll_up
_bios_scroll_up:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov ah, 0x06
    mov al, [bp+4]          ; lines
    mov bh, [bp+6]          ; attr
    mov ch, [bp+8]          ; top
    mov cl, [bp+10]         ; left
    mov dh, [bp+12]         ; bottom
    mov dl, [bp+14]         ; right
    int 0x10

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; void bios_set_palette_reg(uint8_t reg, uint8_t val) -> calls INT 0x10 AX=0x1000
global _bios_set_palette_reg
_bios_set_palette_reg:
    push bp
    mov bp, sp
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov ah, 0x10
    mov al, 0x00
    mov bl, [bp+4]
    mov bh, [bp+6]
    int 0x10

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop bp
    ret

; void cga_wait_vsync(void) -> waits for rising edge of CGA vertical retrace (0x3DA bit 3)
global _cga_wait_vsync
_cga_wait_vsync:
    push cx
    push dx
    mov dx, 0x03DA
    mov cx, 1000
.wait_end:
    in  al, dx
    test al, 0x08
    jz  .wait_start
    loop .wait_end
.wait_start:
    mov cx, 1000
.wait_start_loop:
    in  al, dx
    test al, 0x08
    jnz .done
    loop .wait_start_loop
.done:
    pop dx
    pop cx
    ret

; void cga_lcd_sync_delay(void) -> exact busy-wait delay calibrated for Book 8088 LCD scaler
global _cga_lcd_sync_delay
_cga_lcd_sync_delay:
    push cx
    push dx
    mov dx, 20
.wait_outer:
    mov cx, 0xFFFF
.wait_inner:
    loop .wait_inner
    dec dx
    jnz .wait_outer
    pop dx
    pop cx
    ret

; void cga_blit_linear(const void __far *src)
; Copies a 16,000-byte (200x80) linear buffer into CGA interleaved banks at 0xB800:0000 and 0xB800:2000
global _cga_blit_linear
_cga_blit_linear:
    push bp
    mov bp, sp
    push cx
    push dx
    push si
    push di
    push ds
    push es

    cld
    mov ax, 0xB800
    mov es, ax

    ; Bank 0 (even scanlines 0, 2, ..., 198 -> 0xB800:0000)
    lds si, [bp+4]
    xor di, di
    mov dx, 100
.even_loop:
    mov cx, 40              ; 40 words = 80 bytes per scanline
    rep movsw
    add si, 80              ; skip odd scanline
    dec dx
    jnz .even_loop

    ; Bank 1 (odd scanlines 1, 3, ..., 199 -> 0xB800:2000)
    lds si, [bp+4]
    add si, 80              ; start at scanline 1
    mov di, 0x2000
    mov dx, 100
.odd_loop:
    mov cx, 40
    rep movsw
    add si, 80              ; skip even scanline
    dec dx
    jnz .odd_loop

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bp
    ret

; void vga_blit_linear(const void __far *src)
; Copies a 64,000-byte (320x200) linear buffer into VGA VRAM at 0xA000:0000
global _vga_blit_linear
_vga_blit_linear:
    push bp
    mov bp, sp
    push cx
    push si
    push di
    push ds
    push es

    cld
    mov ax, 0xA000
    mov es, ax
    xor di, di
    lds si, [bp+4]
    mov cx, 32000
    rep movsw

    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop bp
    ret

; =============================================================================
; INT 0x08 (IRQ 0) Hardware Timer Interrupt Handler for Background Music
; =============================================================================
global _sound_timer_install
global _sound_timer_restore
global _isr_int08

_old_int08_off  dw 0
_old_int08_seg  dw 0
_timer_step     dw 0
_timer_accum    dw 0
_timer_hooked   db 0

; void sound_timer_install(uint16_t divisor)
_sound_timer_install:
    push bp
    mov bp, sp
    push bx
    push es

    cmp byte [cs:_timer_hooked], 0
    jne .already_hooked

    ; Save old INT 0x08 vector at 0000:0020
    xor ax, ax
    mov es, ax
    cli
    mov ax, [es:0x0020]
    mov [cs:_old_int08_off], ax
    mov ax, [es:0x0022]
    mov [cs:_old_int08_seg], ax

    ; Install our ISR
    mov word [es:0x0020], _isr_int08
    mov word [es:0x0022], cs
    mov byte [cs:_timer_hooked], 1
    sti

.already_hooked:
    mov bx, [bp+4]          ; Divisor (e.g. 2131 for 560Hz, 1705 for 700Hz)
    mov [cs:_timer_step], bx
    mov word [cs:_timer_accum], 0

    ; Reprogram Timer 0 Mode 2 (Rate Generator) with divisor
    cli
    mov al, 0x34
    out 0x43, al
    mov al, bl
    out 0x40, al
    mov al, bh
    out 0x40, al
    sti

    pop es
    pop bx
    pop bp
    ret

; void sound_timer_restore(void)
_sound_timer_restore:
    push es
    cmp byte [cs:_timer_hooked], 0
    je .not_hooked

    ; Restore Timer 0 Mode 3 (Square Wave), divisor 65536 (0)
    cli
    mov al, 0x36
    out 0x43, al
    xor al, al
    out 0x40, al
    out 0x40, al

    ; Restore old INT 0x08 vector
    xor ax, ax
    mov es, ax
    mov ax, [cs:_old_int08_off]
    mov [es:0x0020], ax
    mov ax, [cs:_old_int08_seg]
    mov [es:0x0022], ax
    mov byte [cs:_timer_hooked], 0
    sti

.not_hooked:
    pop es
    ret

; INT 0x08 ISR for Background IMF Music
_isr_int08:
    push ds
    push es
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov ax, cs
    mov ds, ax

    ; Advance background music
    call _sound_bg_tick

    ; Accumulate timer step
    mov ax, [cs:_timer_step]
    add [cs:_timer_accum], ax
    jc .chain_bios

    ; Normal tick: acknowledge PIC (EOI) and return
    mov al, 0x20
    out 0x20, al

    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    pop ds
    iret

.chain_bios:
    ; 18.2 Hz BIOS mark reached: chain to original BIOS INT 0x08 handler
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    pop ds
    jmp far [cs:_old_int08_off]

_old_int10_off  dw 0
_old_int10_seg  dw 0
_in_int10_sync  db 0

; void syscall_install_vector(void) -> installs _isr_int21 at INT 0x21 (0000:0084)
; and hooks BIOS INT 0x10 (0000:0040) for Book 8088 CGA LCD boundary sync
global _syscall_install_vector
_syscall_install_vector:
    push es
    xor ax, ax
    mov es, ax
    cli
    mov word [es:0x0084], _isr_int21
    mov word [es:0x0086], cs

    ; Hook INT 0x10 if not already hooked
    cmp word [cs:_old_int10_seg], 0
    jne .int10_done
    mov ax, [es:0x0040]
    mov [cs:_old_int10_off], ax
    mov ax, [es:0x0042]
    mov [cs:_old_int10_seg], ax
    mov word [es:0x0040], _isr_int10
    mov word [es:0x0042], cs
.int10_done:
    sti
    pop es
    ret

; INT 0x10 BIOS Video Service Hook
; Intercepts AH=00h (Set Video Mode) and runs Book 8088 CGA LCD boundary sync
global _isr_int10
_isr_int10:
    cmp ah, 0x00
    jne .int10_passthru

    cmp byte [cs:_in_int10_sync], 0
    jne .int10_passthru

    ; Call BIOS handler first to set mode
    pushf
    call far [cs:_old_int10_off]

    ; Perform LCD boundary sync
    mov byte [cs:_in_int10_sync], 1
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    sti
    call _gfx_cga_lcd_sync

    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    mov byte [cs:_in_int10_sync], 0
    iret

.int10_passthru:
    jmp far [cs:_old_int10_off]

; INT 0x21 System Call Handler
global _isr_int21
_isr_int21:
    push es
    push ds
    push bp
    push di
    push si
    push dx
    push cx
    push bx
    push ax

    mov dx, ss              ; dx:ax = far pointer to syscall_regs_t
    mov ax, sp

    mov cx, cs
    mov ds, cx
    mov es, cx

    cmp dx, cx              ; If called from user segment, switch to kernel stack
    je  .on_kernel_stack
    mov ss, cx
    mov sp, [_kernel_saved_sp]
.on_kernel_stack:
    sti
    cld
    push dx                 ; segment of regs
    push ax                 ; offset of regs
    call _syscall_dispatch
    pop ax                  ; offset of regs
    pop dx                  ; segment of regs

    ; Check if Ctrl+C abort was flagged while in syscall
    cmp word [_ctrl_c_abort_pending], 0
    je .no_pending_abort
    mov word [_ctrl_c_abort_pending], 0
    cmp word [_usermode_active], 1
    jbe .no_pending_abort

    mov ax, 130
    push ax
    call _proc_exit_usermode

.no_pending_abort:
    cli
    mov ss, dx
    mov sp, ax

    pop ax
    pop bx
    pop cx
    pop dx
    pop si
    pop di
    pop bp
    pop ds
    pop es
    iret

; int proc_exec_far(uint16_t user_seg, uint16_t user_sp)
global _proc_exec_far
_proc_exec_far:
    push bp
    mov bp, sp
    push si
    push di
    push ds
    push es
    push word [_kernel_saved_sp]
    push word [_usermode_active]

    mov ax, [bp+4]          ; user_seg
    mov bx, [bp+6]          ; user_sp

    cli
    mov [_kernel_saved_sp], sp
    inc word [_usermode_active]

    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, bx
    sti

    push ax                 ; CS = user_seg
    xor ax, ax
    push ax                 ; IP = 0x0000
    retf

; int proc_exec_mz(uint16_t cs, uint16_t ip, uint16_t ss, uint16_t sp, uint16_t ds_es)
global _proc_exec_mz
_proc_exec_mz:
    push bp
    mov bp, sp
    push si
    push di
    push ds
    push es
    push word [_kernel_saved_sp]
    push word [_usermode_active]

    mov ax, [bp+4]          ; target CS
    mov bx, [bp+6]          ; target IP
    mov cx, [bp+8]          ; target SS
    mov dx, [bp+10]         ; target SP
    mov si, [bp+12]         ; target DS/ES

    cli
    mov [_kernel_saved_sp], sp
    inc word [_usermode_active]

    mov ds, si
    mov es, si
    mov ss, cx
    mov sp, dx
    sti

    push ax                 ; CS
    push bx                 ; IP
    retf

; void proc_exit_usermode(uint8_t code)
global _proc_exit_usermode
_proc_exit_usermode:
    mov bp, sp
    mov al, [bp+2]
    xor ah, ah

    cli
    mov cx, cs
    mov ds, cx
    mov es, cx
    mov ss, cx
    mov sp, [_kernel_saved_sp]

    pop word [_usermode_active]
    pop word [_kernel_saved_sp]
    pop es
    pop ds
    pop di
    pop si
    pop bp
    sti
    ret

; uint16_t get_usermode_active(void)
global _get_usermode_active
_get_usermode_active:
    mov ax, [_usermode_active]
    ret

; uint16_t bios_get_ext_mem_kb(void)
global _bios_get_ext_mem_kb
_bios_get_ext_mem_kb:
    push bp
    mov bp, sp
    mov ah, 0x88
    clc
    int 0x15
    jnc .ok
    xor ax, ax
.ok:
    pop bp
    ret

; int bios_move_ext_mem(void __far *gdt_table, uint16_t word_count)
global _bios_move_ext_mem
_bios_move_ext_mem:
    push bp
    mov bp, sp
    push si
    push di
    push ds
    push es

    mov si, [bp+4]      ; offset
    mov ax, [bp+6]      ; segment
    mov es, ax
    mov cx, [bp+8]      ; word_count

    mov ah, 0x87
    clc
    int 0x15
    jc .err
    test ah, ah
    jnz .err

    xor ax, ax
    jmp .done
.err:
    test ah, ah
    jnz .done
    mov ah, 1
.done:
    pop es
    pop ds
    pop di
    pop si
    pop bp
    ret

; int a20_test(void) -> returns 1 if A20 is enabled, 0 if wrapped/disabled
global _a20_test
_a20_test:
    push bp
    mov bp, sp
    push ds
    push es
    push si
    push di

    xor ax, ax
    mov ds, ax
    mov ax, 0xFFFF
    mov es, ax

    mov si, 0x0500
    mov di, 0x0510

    push word [ds:si]
    push word [es:di]

    mov word [ds:si], 0x0000
    mov word [es:di], 0x1234

    cmp word [ds:si], 0x1234
    je .wrapped

    mov ax, 1
    jmp .finish

.wrapped:
    xor ax, ax

.finish:
    pop word [es:di]
    pop word [ds:si]

    pop di
    pop si
    pop es
    pop ds
    pop bp
    ret

_kbd_wait_input_empty:
    push cx
    xor cx, cx
.wait_loop:
    in al, 0x64
    test al, 0x02
    jz .done
    loop .wait_loop
.done:
    pop cx
    ret

; int a20_enable(void) -> returns 1 if enabled, 0 on failure
global _a20_enable
_a20_enable:
    push bp
    mov bp, sp

    ; 1. Check if already enabled
    call _a20_test
    test ax, ax
    jnz .success

    ; 2. Try BIOS INT 0x15, AX=0x2401
    mov ax, 0x2401
    clc
    int 0x15
    call _a20_test
    test ax, ax
    jnz .success

    ; 3. Try Fast A20 gate (port 0x92)
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    call _a20_test
    test ax, ax
    jnz .success

    ; 4. Try Keyboard Controller 8042
    call _kbd_wait_input_empty
    mov al, 0xD1
    out 0x64, al
    call _kbd_wait_input_empty
    mov al, 0xDF
    out 0x60, al
    call _kbd_wait_input_empty
    call _a20_test
    test ax, ax
    jnz .success

    xor ax, ax
    pop bp
    ret

.success:
    mov ax, 1
    pop bp
    ret

; void a20_disable(void)
global _a20_disable
_a20_disable:
    push bp
    mov bp, sp

    ; 1. Try BIOS INT 0x15, AX=0x2400
    mov ax, 0x2400
    clc
    int 0x15

    ; 2. Try Fast A20 gate (port 0x92)
    in al, 0x92
    and al, 0xFD
    out 0x92, al

    ; 3. Try 8042
    call _kbd_wait_input_empty
    mov al, 0xD1
    out 0x64, al
    call _kbd_wait_input_empty
    mov al, 0xDD
    out 0x60, al
    call _kbd_wait_input_empty

    pop bp
    ret

; void xms_install_vector(void) -> installs _isr_int2f at INT 0x2F (0000:00BC)
global _xms_install_vector
_xms_install_vector:
    push es
    xor ax, ax
    mov es, ax
    cli
    mov word [es:0x00BC], _isr_int2f
    mov word [es:0x00BE], cs
    sti
    pop es
    ret

_isr_int2f:
    cmp ax, 0x4300
    jne .chk_4310
    mov al, 0x80        ; XMS installed
    iret

.chk_4310:
    cmp ax, 0x4310
    jne .chain
    push cs
    pop es
    mov bx, _xms_driver_entry
    iret

.chain:
    iret

global _xms_driver_entry
_xms_driver_entry:
    pushf
    push es
    push ds
    push bp
    push di
    push si
    push dx
    push cx
    push bx
    push ax

    mov dx, ss
    mov ax, sp

    mov cx, cs
    mov ds, cx
    mov es, cx

    push dx             ; segment of regs
    push ax             ; offset of regs
    call _xms_dispatch
    pop ax
    pop dx

    pop ax
    pop bx
    pop cx
    pop dx
    pop si
    pop di
    pop bp
    pop ds
    pop es
    popf
    retf

_kernel_saved_sp: dw 0xFBFE
_usermode_active: dw 0
global _ctrl_c_abort_pending
_ctrl_c_abort_pending: dw 0

extern _mouse_dispatch
extern _mouse_irq_handler

; void mouse_install_vectors(uint8_t irq)
global _mouse_install_vectors
_mouse_install_vectors:
    push bp
    mov bp, sp
    push es

    xor ax, ax
    mov es, ax

    cli
    ; Install INT 0x33 vector (0x33 * 4 = 0x00CC)
    mov word [es:0x00CC], _isr_int33
    mov word [es:0x00CE], cs

    mov al, [bp+4]          ; IRQ number (3, 4, or 12)

    cmp al, 3
    je .install_irq3
    cmp al, 4
    je .install_irq4
    cmp al, 12
    je .install_irq12
    jmp .done

.install_irq3:
    ; Install INT 0x0B vector (0x0B * 4 = 0x002C)
    mov word [es:0x002C], _isr_mouse_irq3
    mov word [es:0x002E], cs
    in al, 0x21
    and al, 0xF7            ; Unmask IRQ 3 (bit 3)
    out 0x21, al
    jmp .done

.install_irq4:
    ; Install INT 0x0C vector (0x0C * 4 = 0x0030)
    mov word [es:0x0030], _isr_mouse_irq4
    mov word [es:0x0032], cs
    in al, 0x21
    and al, 0xEF            ; Unmask IRQ 4 (bit 4)
    out 0x21, al
    jmp .done

.install_irq12:
    ; Install INT 0x74 vector (0x74 * 4 = 0x01D0)
    mov word [es:0x01D0], _isr_mouse_irq12
    mov word [es:0x01D2], cs
    in al, 0x21
    and al, 0xFB            ; Unmask IRQ 2 (cascade)
    out 0x21, al
    in al, 0xA1
    and al, 0xEF            ; Unmask IRQ 12
    out 0xA1, al

.done:
    sti
    pop es
    pop bp
    ret

global _isr_mouse_irq3
_isr_mouse_irq3:
    push es
    push ds
    push bp
    push di
    push si
    push dx
    push cx
    push bx
    push ax
    mov ax, cs
    mov ds, ax
    mov es, ax
    cld
    call _mouse_irq_handler
    mov al, 0x20
    out 0x20, al            ; EOI to master PIC
    pop ax
    pop bx
    pop cx
    pop dx
    pop si
    pop di
    pop bp
    pop ds
    pop es
    iret

global _isr_mouse_irq4
_isr_mouse_irq4:
    push es
    push ds
    push bp
    push di
    push si
    push dx
    push cx
    push bx
    push ax
    mov ax, cs
    mov ds, ax
    mov es, ax
    cld
    call _mouse_irq_handler
    mov al, 0x20
    out 0x20, al            ; EOI to master PIC
    pop ax
    pop bx
    pop cx
    pop dx
    pop si
    pop di
    pop bp
    pop ds
    pop es
    iret

global _isr_mouse_irq12
_isr_mouse_irq12:
    push es
    push ds
    push bp
    push di
    push si
    push dx
    push cx
    push bx
    push ax
    mov ax, cs
    mov ds, ax
    mov es, ax
    cld
    call _mouse_irq_handler
    mov al, 0x20
    out 0xA0, al            ; EOI to slave PIC
    out 0x20, al            ; EOI to master PIC
    pop ax
    pop bx
    pop cx
    pop dx
    pop si
    pop di
    pop bp
    pop ds
    pop es
    iret

; int bios_ps2_enable(uint8_t enable)
; BH = enable (1) or disable (0)
global _bios_ps2_enable
_bios_ps2_enable:
    push bp
    mov bp, sp
    push bx
    mov ah, 0xC2
    mov al, 0x00
    mov bh, [bp+4]          ; enable (0 or 1)
    clc
    int 0x15
    jc .err
    test ah, ah
    jnz .err
    xor ax, ax
    jmp .done
.err:
    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .done
    mov ax, 1
.done:
    pop bx
    pop bp
    ret

; int bios_ps2_init(uint8_t pkg_size)
; BH = package size (e.g. 3)
global _bios_ps2_init
_bios_ps2_init:
    push bp
    mov bp, sp
    push bx
    mov ah, 0xC2
    mov al, 0x05
    mov bh, [bp+4]          ; package size
    clc
    int 0x15
    jc .err
    test ah, ah
    jnz .err
    xor ax, ax
    jmp .done
.err:
    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .done
    mov ax, 1
.done:
    pop bx
    pop bp
    ret

; int bios_ps2_set_handler(void)
; ES:BX = cs:_ps2_bios_callback
global _bios_ps2_set_handler
_bios_ps2_set_handler:
    push bp
    mov bp, sp
    push bx
    push es
    mov bx, _ps2_bios_callback
    mov ax, cs
    mov es, ax
    mov ah, 0xC2
    mov al, 0x07
    clc
    int 0x15
    jc .err
    test ah, ah
    jnz .err
    xor ax, ax
    jmp .done
.err:
    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .done
    mov ax, 1
.done:
    pop es
    pop bx
    pop bp
    ret

; int bios_ps2_reset_handler(void)
; ES:BX = 0000:0000
global _bios_ps2_reset_handler
_bios_ps2_reset_handler:
    push bp
    mov bp, sp
    push bx
    push es
    xor bx, bx
    xor ax, ax
    mov es, ax
    mov ah, 0xC2
    mov al, 0x07
    clc
    int 0x15
    jc .err
    test ah, ah
    jnz .err
    xor ax, ax
    jmp .done
.err:
    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .done
    mov ax, 1
.done:
    pop es
    pop bx
    pop bp
    ret

; int bios_ps2_get_id(uint8_t *id)
; Returns device ID in *id
global _bios_ps2_get_id
_bios_ps2_get_id:
    push bp
    mov bp, sp
    push bx
    push si
    mov ah, 0xC2
    mov al, 0x04
    clc
    int 0x15
    jc .err
    test ah, ah
    jnz .err
    mov si, [bp+4]
    test si, si
    jz .ok
    mov [si], bh
.ok:
    xor ax, ax
    jmp .done
.err:
    mov al, ah
    xor ah, ah
    test ax, ax
    jnz .done
    mov ax, 1
.done:
    pop si
    pop bx
    pop bp
    ret

extern _mouse_handle_ps2_packet
global _ps2_bios_callback
_ps2_bios_callback:
    push bp
    mov bp, sp
    push ds
    push es
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov ax, cs
    mov ds, ax
    mov es, ax
    cld

    ; Stack layout on entry:
    ; [bp + 12] = Word 1: status flags
    ; [bp + 10] = Word 2: X movement
    ; [bp + 8]  = Word 3: Y movement
    ; [bp + 6]  = Word 4: Z movement
    ; [bp + 4]  = caller CS
    ; [bp + 2]  = caller IP
    ; [bp + 0]  = saved BP
    push word [bp + 8]      ; raw_y
    push word [bp + 10]     ; raw_x
    push word [bp + 12]     ; status
    call _mouse_handle_ps2_packet
    add sp, 6

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop es
    pop ds
    pop bp
    retf

global _isr_int33
_isr_int33:
    push es
    push ds
    push bp
    push di
    push si
    push dx
    push cx
    push bx
    push ax

    mov dx, ss              ; dx:ax = pointer to syscall_regs_t
    mov ax, sp

    mov cx, cs
    mov ds, cx
    mov es, cx

    cmp dx, cx
    je .on_kstack
    mov ss, cx
    mov sp, [_kernel_saved_sp]
.on_kstack:
    sti
    cld
    push dx                 ; segment of regs
    push ax                 ; offset of regs
    call _mouse_dispatch
    pop ax
    pop dx

    cli
    mov ss, dx
    mov sp, ax

    pop ax
    pop bx
    pop cx
    pop dx
    pop si
    pop di
    pop bp
    pop ds
    pop es
    iret
