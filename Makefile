# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later

WATCOM := /opt/watcom
PATH   := $(WATCOM)/binl64:$(PATH)

86BOX  := /usr/local/bin/86Box
IMG    := book8088/disk.img
CFLAGS := -0 -fpi87 -ms -s -zl -ecc -zq -Iinclude -Ibuild

KERNEL_OBJS := build/entry.obj build/console.obj build/gfx.obj build/kbd.obj build/mem.obj build/ems.obj build/xms.obj build/fat16.obj build/syscall.obj build/main.obj
USER_BINS   := build/hello.bin build/mem.bin build/keyb.bin build/edit.bin build/shell.bin build/forth.bin build/format.bin build/fdisk.bin build/chkdsk.bin build/gfxdemo.bin
KEYMAPS     := build/us.map build/fr.map build/be.map

all: $(IMG)

build/boot.bin: boot/boot.asm
	mkdir -p build
	nasm -f bin $< -o $@

build/bootcode.h: build/boot.bin
	python3 -c "b = open('$<', 'rb').read(); open('$@', 'w').write('/* Auto-generated default boot sector */\nstatic const unsigned char default_boot_sector[512] = {\n' + ', '.join(f'0x{x:02x}' for x in b) + '\n};\n')"

build/mbr.bin: boot/mbr.asm
	mkdir -p build
	nasm -f bin $< -o $@

build/mbrcode.h: build/mbr.bin
	python3 -c "b = open('$<', 'rb').read(); open('$@', 'w').write('/* Auto-generated default MBR code */\nstatic const unsigned char default_mbr_code[446] = {\n' + ', '.join(f'0x{x:02x}' for x in b[:446]) + '\n};\n')"

build/entry.obj: kernel/entry.asm
	mkdir -p build
	nasm -f obj $< -o $@

build/fat16.obj: kernel/fat16.c include/hobbyos.h kernel/kernel.h build/bootcode.h build/mbrcode.h
	mkdir -p build
	wcc $(CFLAGS) $< -fo=$@

build/%.obj: kernel/%.c include/hobbyos.h kernel/kernel.h
	mkdir -p build
	wcc $(CFLAGS) $< -fo=$@

build/kernel.bin: $(KERNEL_OBJS)
	wlink format raw bin name $@ file { $(KERNEL_OBJS) } order clname CODE clname CONST clname DATA clname BSS

build/crt0.obj: user/crt0.asm
	mkdir -p build
	nasm -f obj $< -o $@

build/%.uobj: user/%.c include/hobbyos.h
	mkdir -p build
	wcc $(CFLAGS) $< -fo=$@

build/%.bin: build/crt0.obj build/%.uobj
	wlink format raw bin name $@ file { build/crt0.obj build/$*.uobj } order clname CODE clname CONST clname DATA clname BSS

build/forth.bin: user/forth.asm
	mkdir -p build
	nasm -f bin $< -o $@

$(KEYMAPS): tools/mkkeymap.py
	python3 tools/mkkeymap.py build

$(IMG): build/boot.bin build/kernel.bin $(USER_BINS) $(KEYMAPS) boot.sh tools/fat16.py
	dd if=/dev/zero of=$@ bs=512 count=20808 status=none
	/usr/sbin/mkfs.fat -a -F 16 -s 4 -R 1 -f 2 -r 512 -g 4/17 -M 0xF8 -D 0x80 -n "HOBBYOS" $@ >/dev/null
	python3 tools/fat16.py $@ build/boot.bin \
		build/kernel.bin:KERNEL.BIN \
		build/shell.bin:/BIN/SHELL.BIN \
		build/mem.bin:/BIN/MEM.BIN \
		build/keyb.bin:/BIN/KEYB.BIN \
		build/edit.bin:/BIN/EDIT.BIN \
		build/hello.bin:/BIN/HELLO.BIN \
		build/forth.bin:/BIN/FORTH.BIN \
		build/format.bin:/BIN/FORMAT.BIN \
		build/fdisk.bin:/BIN/FDISK.BIN \
		build/chkdsk.bin:/BIN/CHKDSK.BIN \
		build/gfxdemo.bin:/BIN/GFXDEMO.BIN \
		build/boot.bin:/BIN/BOOT.BIN \
		boot.sh:/BOOT.SH \
		build/us.map:/KEYMAPS/US.MAP \
		build/fr.map:/KEYMAPS/FR.MAP \
		build/be.map:/KEYMAPS/BE.MAP

clean:
	rm -rf build

run: $(IMG)
	$(86BOX) -P book8088 &
	sleep 1
	gdb -batch -ex "target remote :12345" -ex "c"

agent-test: $(IMG)
	timeout 20s env QT_QPA_PLATFORM=offscreen $(86BOX) -P book8088 > build/run.log 2>&1 & \
	PID=$$!; \
	trap "kill $$PID 2>/dev/null || true" EXIT; \
	while ! nc -z 127.0.0.1 12345; do sleep 0.1; done; \
	gdb -batch -ex "set architecture i8086" -ex "target remote :12345" -ex "c"; \
	sleep 6; \
	kill $$PID 2>/dev/null || true; \
	cat build/run.log
