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

section _TEXT class=CODE

global _start
global _boot_drive_num
_start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE
    mov [_boot_drive_num], dl
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

    ; Step 3: Check for NEC V20/V30 using AAM with divisor 3.
    ; Intel divides AL (5) by 3 -> AH=1, AL=2.
    ; NEC V20/V30 ignores divisor and divides by 10 -> AH=0, AL=5.
    mov ax, 0x0705
    db 0xD4, 0x03       ; AAM 3
    test ah, ah
    jz .is_nec

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

    mov al, 0x20            ; Send EOI to 8259 PIC
    out 0x20, al

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

.wait_active:
    in  al, dx
    test al, 0x08           ; Vertical retrace active? Safe to write immediately
    jnz .write_now
    test al, 0x01           ; Wait for active scanline before catching rising hblank edge
    jnz .wait_active

    cli
.wait_hblank:
    in  al, dx
    test al, 0x01           ; Wait for start of horizontal blanking
    jz  .wait_hblank
.write_now:
    mov [es:di], bx
    sti

    pop es
    pop di
    pop dx
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

.wait_disp:
    in  al, dx
    test al, 0x08           ; Vertical retrace active? Write immediately
    jnz .store_word
    test al, 0x01
    jnz .wait_disp

    cli
.wait_retrace:
    in  al, dx
    test al, 0x01
    jz  .wait_retrace
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

; void syscall_install_vector(void) -> installs _isr_int21 at INT 0x21 (0000:0084)
global _syscall_install_vector
_syscall_install_vector:
    push es
    xor ax, ax
    mov es, ax
    cli
    mov word [es:0x0084], _isr_int21
    mov word [es:0x0086], cs
    sti
    pop es
    ret

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
    mov word [_usermode_active], 1

    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, bx
    sti

    push ax                 ; CS = user_seg
    xor ax, ax
    push ax                 ; IP = 0x0000
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

_kernel_saved_sp: dw 0xFFFE
_usermode_active: dw 0
