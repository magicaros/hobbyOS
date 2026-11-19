# hobbyOS

hobbyOS is a lightweight, freestanding 16-bit real-mode operating system written from scratch for the Intel 8088 CPU, targeting the **Book8088** laptop and IBM PC/XT-compatible hardware (emulated via 86Box).

## Features

- **Bootloader & Storage**:
  - Dual FAT12/FAT16 Volume Boot Record (VBR) with floppy spin-up retry handling.
  - Master Boot Record (MBR) partition chainloader.
  - Multi-volume storage architecture with AmigaDOS-style device naming (`FD0:`, `FD1:`, `HD0:`, `SYS:`, `RAM:`).
  - FAT12, FAT16, and FAT16B (BigDOS) filesystem driver with subdirectories, path resolution (`.` and `..`), and random-access 32-bit offset streaming (`SYS_FILE_READ_OFFSET`).
- **Memory Management**:
  - DOS-style Memory Control Block (MCB) allocator managing ~574 KB of conventional memory (`0x0060`–`0x8FFF`).
  - Lo-tech 2 MB Expanded Memory (EMS) driver across four 16 KB page frame slots at `0xC000` (I/O `0x268`–`0x26B`, 128 physical pages).
  - XMS 2.0 Extended Memory driver for 80286+ hardware.
  - Dynamic EMS/XMS-backed RAM disk (`RAM:`).
  - Automatic reclamation of conventional MCBs, EMS pages, and XMS handles on process termination.
- **Display & Graphics**:
  - Snow-free 80x25 16-color CGA text driver synchronized to hardware horizontal/vertical retrace.
  - CGA graphics modes (320x200 4-color, 640x200 2-color) and VGA Mode 0x13 (320x200 256-color) with DAC palette manipulation, drawing primitives, BIOS font rendering, and double-buffered linear blitting.
- **Input & Peripherals**:
  - Hardware PC/XT `IRQ 1` keyboard driver with live key-down state tracking.
  - Dynamic 392-byte binary keymap tables (`US`, `FR`, `BE`).
  - Standard `INT 0x33` mouse driver with auto-probing for PS/2 and Serial mice (Microsoft 2/3-button & Mouse Systems 3-button on COM1..COM4).
- **Hardware Probing**:
  - Pre-CPUID CPU architecture and bus-width detection (8088, 8086, 186, 286, 386, 486, Pentium).
  - Intel 8087 FPU detection and graceful fallback handling.
  - Video subsystem detection (CGA, EGA, VGA).
- **Built-in User-Space Suite**:
  - `/BIN/SHELL.BIN`: Interactive shell with script execution (`.SH`), `PATH` resolution, volume switching, and file management commands (`ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `cat`, `write`, `cp`, `mv`, `rm`, `cls`, `set`, `echo`, `run`).
  - `/BIN/EDIT.BIN`: Nano-inspired full-screen text editor supporting search, line navigation, cut/paste, and files up to 60 KB.
  - `/BIN/FORTH.BIN`: 16-bit real-mode JONESFORTH interpreter (Indirect Threaded Code, ~70 core words, compiler, and REPL).
  - `/BIN/FORMAT.BIN`: Disk and partition formatter supporting floppy geometries (`360K`, `720K`, `1.2M`, `1.44M`), hard disk FAT16B partitions, quick format (`/Q`), and bootable system transfer (`/S`).
  - `/BIN/FDISK.BIN`: Interactive and CLI hard disk partition manager (`/status`, `/create`, `/activate`, `/delete`, `/mbr`).
  - `/BIN/CHKDSK.BIN`: Filesystem integrity check and cluster repair utility (`/f`, `/v`).
  - `/BIN/RAMDISK.BIN`: Dynamic EMS/XMS RAM disk utility (`/e`, `/x`, `/u`, `/s`).
  - `/BIN/HAR.BIN`: HobbyOS Archiver with freestanding 8086 LZ4 block compression, recursive directory archiving, and CRC16 verification.
  - `/BIN/SAM.BIN`: 4-bit quantized retro AI micro language model running on the 8088 CPU, staged in Lo-tech EMS SRAM at `0xC000` (or conventional RAM fallback).
  - `/BIN/GFXDEMO.BIN`: Interactive CGA/VGA graphics showcase and 3D rotating wireframe demo.
  - `/BIN/MOUSE.BIN`: Mouse driver status and interactive motion/button testing utility (`/s`).
  - `/BIN/PLAY.BIN`: Sound and IMF music player utility with OPL2/OPL3 FM synthesis, PC Speaker fallback, and 8-bit PCM digital audio via Sound Blaster DMA.
  - `/BIN/VOLUME.BIN`: Sound Blaster hardware mixer volume control (`volume [master|fm|pcm] <0-100>`).
  - `/BIN/MORE.BIN`: Paged terminal text viewer (`more [file] | <cmd> | more`) with interactive Space/Enter/Q navigation.
  - `/BIN/TREE.BIN`: Visual directory hierarchy visualizer (`tree [path] [/f] [/a]`) with IBM CP437 box drawing and ASCII modes.
  - `/BIN/VIDSYNC.BIN`: Book 8088 CGA LCD boundary sync and display recalibration utility.
  - `/BIN/MEM.BIN`: Memory reporting tool for conventional, EMS, and XMS RAM.
  - `/BIN/KEYB.BIN`: Dynamic keyboard layout loader.
  - `/BIN/HELLO.BIN`: Automated boot self-test utility.

## Building & Testing

### Prerequisites

- [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2) C compiler (`wcc`) and linker (`wlink`)
- [NASM](https://www.nasm.us/)
- Python 3
- `dosfstools` (`mkfs.fat`)
- Optional for emulation: [86Box](https://86box.net/)

### Build

```bash
make
```

This compiles the bootloader, kernel, user applications, trained AI model weights (`/MODELS/SAM.MDL`), and keymaps, packaging them into `book8088/disk.img`.

### Testing

Run the test suite in headless 86Box:

```bash
make agent-test
```

Or run interactively with 86Box:

```bash
make run
```

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE) (`GPL-3.0-or-later`).  
Copyright (C) 2026 Benoit Colson.
