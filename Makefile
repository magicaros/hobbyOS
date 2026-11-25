# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later

WATCOM := /opt/watcom
PATH   := $(WATCOM)/binl64:$(PATH)

86BOX  := /usr/local/bin/86Box
IMG    := book8088/disk.img
CFLAGS := -0 -fpi87 -ms -s -os -zl -ecc -zq -Iinclude -Ibuild
CFLAGS_MM := -0 -fpi87 -mm -s -os -zl -ecc -zq -zm -Iinclude -Ibuild

KERNEL_OBJS := build/entry.obj build/console.obj build/gfx.obj build/kbd.obj build/mouse.obj build/sound.obj build/mem.obj build/ems.obj build/xms.obj build/fat16.obj build/syscall.obj build/main.obj
USER_BINS   := build/hello.bin build/mem.bin build/keyb.bin build/edit.bin build/shell.bin build/forth.bin build/format.bin build/fdisk.bin build/chkdsk.bin build/gfxdemo.bin build/ramdisk.bin build/har.bin build/sam.bin build/mouse.bin build/play.bin build/volume.bin build/more.bin build/tree.bin build/vidsync.bin build/view.bin build/zcode.bin build/fm.bin build/cpm.bin build/basic.exe build/tetris.bin
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
	wlink format raw bin name $@ op map=build/kernel.map file { $(KERNEL_OBJS) } order clname CODE clname CONST clname DATA clname BSS

build/crt0.obj: user/crt0.asm
	mkdir -p build
	nasm -f obj $< -o $@

build/crt0_mz.obj: user/crt0_mz.asm
	mkdir -p build
	nasm -f obj $< -o $@

build/%.uobj: user/%.c include/hobbyos.h
	mkdir -p build
	wcc $(CFLAGS) $< -fo=$@

build/%.uobj_mm: user/%.c include/hobbyos.h
	mkdir -p build
	wcc $(CFLAGS_MM) $< -fo=$@

build/%.exe: build/crt0_mz.obj build/%.uobj_mm
	wlink format dos name $@ option start=_ustart file { build/crt0_mz.obj build/$*.uobj_mm }

build/cpm_v20.obj: user/cpm_v20.asm
	mkdir -p build
	nasm -f obj $< -o $@

build/cpm.bin: build/crt0.obj build/cpm.uobj build/cpm_v20.obj
	wlink format raw bin name $@ file { build/crt0.obj build/cpm.uobj build/cpm_v20.obj } order clname CODE clname CONST clname DATA clname BSS

build/test80.com: tools/gen_test80.py
	mkdir -p build
	python3 tools/gen_test80.py $@

build/%.bin: build/crt0.obj build/%.uobj
	wlink format raw bin name $@ file { build/crt0.obj build/$*.uobj } order clname CODE clname CONST clname DATA clname BSS

build/forth.bin: user/forth.asm
	mkdir -p build
	nasm -f bin $< -o $@

$(KEYMAPS): tools/mkkeymap.py
	python3 tools/mkkeymap.py build

build/sam.mdl: tools/train_sam.py
	mkdir -p build
	python3 tools/train_sam.py $@

MUSIC_FILES := build/ode.imf build/wonderin.imf build/keen.imf build/chime.wav build/tetris.imf

build/tetris.imf: tools/gen_tetris_imf.py
	mkdir -p build
	python3 tools/gen_tetris_imf.py $@

build/ode.imf: tools/gen_imf.py
	mkdir -p build
	python3 tools/gen_imf.py $@

build/wonderin.imf: music/wonderin.imf
	mkdir -p build
	cp $< $@

build/keen.imf: music/keen.imf
	mkdir -p build
	cp $< $@

build/chime.wav: tools/gen_wav.py
	mkdir -p build
	python3 tools/gen_wav.py $@

build/sample.cga: tools/gen_cga.py
	mkdir -p build
	python3 tools/gen_cga.py $@

build/sample.pcx: tools/gen_pcx.py
	mkdir -p build
	python3 tools/gen_pcx.py $@

build/sample.vga: tools/gen_vga.py
	mkdir -p build
	python3 tools/gen_vga.py $@

$(IMG): build/boot.bin build/kernel.bin $(USER_BINS) $(KEYMAPS) build/sam.mdl $(MUSIC_FILES) build/sample.cga build/sample.pcx build/sample.vga games/zork1.z3 boot.sh tools/fat16.py build/test80.com $(wildcard basic/*.bas)
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
		build/ramdisk.bin:/BIN/RAMDISK.BIN \
		build/har.bin:/BIN/HAR.BIN \
		build/sam.bin:/BIN/SAM.BIN \
		build/mouse.bin:/BIN/MOUSE.BIN \
		build/play.bin:/BIN/PLAY.BIN \
		build/volume.bin:/BIN/VOLUME.BIN \
		build/more.bin:/BIN/MORE.BIN \
		build/tree.bin:/BIN/TREE.BIN \
		build/vidsync.bin:/BIN/VIDSYNC.BIN \
		build/view.bin:/BIN/VIEW.BIN \
		build/zcode.bin:/BIN/ZCODE.BIN \
		build/fm.bin:/BIN/FM.BIN \
		build/cpm.bin:/BIN/CPM.BIN \
		build/basic.exe:/BIN/BASIC.EXE \
		build/tetris.bin:/BIN/TETRIS.BIN \
		basic/math.bas:/BASIC/MATH.BAS \
		basic/sys.bas:/BASIC/SYS.BAS \
		basic/test1.bas:/BASIC/TEST1.BAS \
		basic/test2.bas:/BASIC/TEST2.BAS \
		basic/test3.bas:/BASIC/TEST3.BAS \
		basic/str.bas:/BASIC/STR.BAS \
		basic/err.bas:/BASIC/ERR.BAS \
		basic/event.bas:/BASIC/EVENT.BAS \
		basic/random.bas:/BASIC/RANDOM.BAS \
		basic/final.bas:/BASIC/FINAL.BAS \
		basic/phase13.bas:/BASIC/PHASE13.BAS \
		basic/phase14.bas:/BASIC/PHASE14.BAS \
		basic/phase15.bas:/BASIC/PHASE15.BAS \
		basic/phase16.bas:/BASIC/PHASE16.BAS \
		basic/joytest.bas:/BASIC/JOYTEST.BAS \
		build/test80.com:/BIN/TEST80.COM \
		games/zork1.z3:/GAMES/ZORK1.Z3 \
		build/sample.cga:/PICS/SAMPLE.CGA \
		build/sample.pcx:/PICS/SAMPLE.PCX \
		build/sample.vga:/PICS/SAMPLE.VGA \
		build/sam.mdl:/MODELS/SAM.MDL \
		build/ode.imf:/MUSIC/ODE.IMF \
		build/wonderin.imf:/MUSIC/WONDERIN.IMF \
		build/keen.imf:/MUSIC/KEEN.IMF \
		build/chime.wav:/MUSIC/CHIME.WAV \
		build/tetris.imf:/MUSIC/TETRIS.IMF \
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
	timeout 90s env QT_QPA_PLATFORM=offscreen $(86BOX) -P book8088 > build/run.log 2>&1 & \
	PID=$$!; \
	trap "kill $$PID 2>/dev/null || true" EXIT; \
	while ! nc -z 127.0.0.1 12345; do sleep 0.1; done; \
	gdb -batch -ex "set architecture i8086" -ex "target remote :12345" -ex "c"; \
	sleep 40; \
	kill $$PID 2>/dev/null || true; \
	cat build/run.log
