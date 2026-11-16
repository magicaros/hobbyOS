; Copyright (C) 2026 Benoit Colson
; SPDX-License-Identifier: GPL-3.0-or-later
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.

[BITS 16]
[ORG 0x7C00]
CPU 8086

jmp short start
nop

; --- FAT16 BIOS Parameter Block (BPB) ---
bpb_oem:                 db "HOBBYOS "
bpb_bytes_per_sector:    dw 512
bpb_sectors_per_cluster: db 4
bpb_reserved_sectors:    dw 1
bpb_num_fats:            db 2
bpb_root_entries:        dw 512
bpb_total_sectors16:     dw 20808
bpb_media:               db 0xF8
bpb_sectors_per_fat:     dw 20
bpb_sectors_per_track:   dw 17
bpb_num_heads:           dw 4
bpb_hidden_sectors:      dd 0
bpb_total_sectors32:     dd 0

; --- Extended BIOS Parameter Block (FAT16) ---
bpb_drive_num:           db 0x80
bpb_reserved1:           db 0
bpb_boot_sig:            db 0x29
bpb_volume_id:           dd 0x12345678
bpb_volume_label:        db "HOBBYOS    "
bpb_fs_type:             db "FAT16   "

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [bpb_drive_num], dl

    mov ax, 0x0E42                      ; Print 'B' (Boot sector started)
    int 0x10

    ; Calculate fat_lba = reserved_sectors + hidden_sectors
    mov ax, [bpb_reserved_sectors]
    add ax, [bpb_hidden_sectors]
    mov [fat_lba], ax

    ; Calculate root_lba = fat_lba + (num_fats * sectors_per_fat)
    xor ah, ah
    mov al, [bpb_num_fats]
    mul word [bpb_sectors_per_fat]
    add ax, [fat_lba]
    mov [root_lba], ax

    ; Calculate root_sectors = (root_entries * 32) / 512
    mov ax, [bpb_root_entries]
    mov cl, 4
    shr ax, cl
    mov cx, ax                          ; cx = root_sectors

    ; Calculate data_lba = root_lba + root_sectors
    add ax, [root_lba]
    mov [data_lba], ax

    ; Search FAT16 root directory for "KERNEL  BIN"
    mov ax, [root_lba]
.search_root_sector:
    xor bx, bx
    mov es, bx
    mov bx, 0x0500                      ; Buffer at 0000:0500
    call read_sector

    mov di, 0x0500
    mov dx, 16                          ; 16 directory entries per sector
.check_entry:
    cmp byte [di], 0x00                 ; End of root directory entries
    je  not_found
    push cx
    push di
    mov si, kernel_filename
    mov cx, 11
    repe cmpsb
    pop di
    pop cx
    je  found_kernel
    add di, 32
    dec dx
    jnz .check_entry

    inc ax
    loop .search_root_sector
    jmp not_found

found_kernel:
    mov bp, [di + 26]                   ; Starting cluster (DIR_FstClusLO)
    cmp bp, 2
    jb  not_found

.load_cluster:
    mov ax, bp
    sub ax, 2
    xor ch, ch
    mov cl, [bpb_sectors_per_cluster]
    mul cx                              ; ax = (cluster - 2) * sectors_per_cluster
    add ax, [data_lba]                  ; ax = first LBA of cluster

.read_cluster_sector:
    mov es, [dest_seg]
    mov bx, [dest_off]
    call read_sector
    inc ax
    add word [dest_off], 512
    jnc .no_seg_wrap
    add word [dest_seg], 0x1000
.no_seg_wrap:
    loop .read_cluster_sector

    ; Look up next cluster in FAT16 table
    mov ax, bp
    mov al, ah
    xor ah, ah
    add ax, [fat_lba]                   ; FAT sector = fat_lba + (cluster / 256)
    xor bx, bx
    mov es, bx
    mov bx, 0x0500
    call read_sector

    mov bx, bp
    xor bh, bh
    shl bx, 1                           ; Byte offset = (cluster % 256) * 2
    mov bp, [bx + 0x0500]
    cmp bp, 0xFFF8
    jb  .load_cluster

    mov ax, 0x0E4B                      ; Print 'K' (Kernel loaded from FAT16)
    int 0x10

    jmp 0x9000:0000

not_found:
    mov ax, 0x0E4E                      ; Print 'N' (KERNEL.BIN not found)
    int 0x10
    jmp hang

disk_err:
    mov ax, 0x0E45                      ; Print 'E' (Disk I/O error)
    int 0x10
hang:
    hlt
    jmp hang

; Read 1 sector at LBA AX into ES:BX
read_sector:
    push ax
    push bx
    push cx
    push dx

    xor dx, dx
    div word [bpb_sectors_per_track]    ; ax = track, dx = sector (0-based)
    mov cl, dl
    inc cl                              ; cl = sector (1-based, bits 0..5)

    xor dx, dx
    div word [bpb_num_heads]            ; ax = cylinder, dx = head
    mov dh, dl                          ; dh = head
    mov ch, al                          ; ch = cylinder low 8 bits
    ror ah, 1
    ror ah, 1
    and ah, 0xC0
    or  cl, ah                          ; cl |= cylinder high 2 bits (bits 6..7)

    mov dl, [bpb_drive_num]
    mov ax, 0x0201                      ; BIOS Read 1 sector
    int 0x13
    jc  disk_err

    pop dx
    pop cx
    pop bx
    pop ax
    ret

kernel_filename: db "KERNEL  BIN"
dest_off:        dw 0x0000
dest_seg:        dw 0x9000
fat_lba:         dw 0
root_lba:        dw 0
data_lba:        dw 0

times 510 - ($ - $$) db 0
dw 0xAA55
