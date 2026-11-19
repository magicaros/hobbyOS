; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; Standard Master Boot Record (MBR) for hobbyOS.
; Relocates itself from 0000:7C00 to 0000:0600, scans the partition table
; for the active (0x80) partition, loads its VBR into 0000:7C00, and boots it.

[BITS 16]
[ORG 0x0600]
CPU 8086

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x0600
    sti
    cld

    ; Save BIOS boot drive passed in DL
    mov [boot_drive], dl

    ; Relocate 512 bytes from 0000:7C00 to 0000:0600
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw

    ; Jump to relocated code
    jmp 0x0000:.relocated

.relocated:
    ; SI points to partition table at 0x0600 + 0x01BE = 0x07BE
    mov si, 0x07BE
    mov bl, 4                           ; 4 partition entries
    xor bp, bp                          ; BP = active partition pointer (0 = none yet)

.scan_parts:
    mov al, [si]                        ; boot indicator
    cmp al, 0x80
    je  .found_active
    cmp al, 0x00
    jne .invalid_table                  ; boot indicator must be 0x00 or 0x80
    jmp .next_part

.found_active:
    test bp, bp
    jnz .invalid_table                  ; multiple active partitions!
    mov bp, si

.next_part:
    add si, 16
    dec bl
    jnz .scan_parts

    test bp, bp
    jz  .no_active                      ; no active partition found

    ; Active partition found! BP points to the active partition entry.
    ; Retry loop to read Sector 1 of the partition into 0000:7C00
    mov di, 5                           ; 5 retries

.read_vbr:
    mov dl, [boot_drive]
    mov dh, [bp + 1]                    ; starting head
    mov cx, [bp + 2]                    ; starting sector (CL) and cylinder (CH + CL bits 6-7)
    mov bx, 0x7C00                      ; ES:BX = 0000:7C00
    mov ax, 0x0201                      ; AH=02 (read), AL=01 (1 sector)
    int 0x13
    jnc .read_ok

    ; Reset disk system and retry
    xor ax, ax
    int 0x13
    dec di
    jnz .read_vbr

    ; Disk read error
    mov si, msg_disk_err
    jmp .print_and_hang

.read_ok:
    ; Verify 0x55AA signature at 0x7DFE
    cmp word [0x7DFE], 0xAA55
    jne .missing_os

    ; Jump to VBR!
    ; Passing DL = boot drive, DS:SI = active partition table entry
    mov si, bp
    mov dl, [boot_drive]
    jmp 0x0000:0x7C00

.invalid_table:
    mov si, msg_invalid_part
    jmp .print_and_hang

.no_active:
    int 0x18                            ; ROM BASIC / No bootable device fallback
    mov si, msg_no_active
    jmp .print_and_hang

.missing_os:
    mov si, msg_missing_os

.print_and_hang:
    cld
.print_loop:
    lodsb
    test al, al
    jz  .hang
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print_loop

.hang:
    hlt
    jmp .hang

msg_invalid_part: db "Invalid partition table", 0
msg_disk_err:     db "Error loading operating system", 0
msg_missing_os:   db "Missing operating system", 0
msg_no_active:    db "No active partition", 0

times 444 - ($ - $$) db 0
boot_drive: db 0x80
dw 0x0000

; Partition table (64 bytes) and 0x55AA signature will be at 446..512
times 510 - ($ - $$) db 0
dw 0xAA55
