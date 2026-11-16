# hobbyOS

hobbyOS is a lightweight, freestanding 16-bit real-mode operating system written from scratch for the Intel 8088 CPU, targeting the **Book8088** laptop and IBM PC/XT-compatible hardware (emulated via 86Box).

## Features

- **Custom FAT16 Bootloader**: Loads the kernel directly from a standard FAT16 partition into conventional memory.
- **Hierarchical FAT16 Driver**: Subdirectory support, path resolution (`.` and `..`), and dynamic file manipulation (`read`, `write`, `append`, `copy`, `rename`, `delete`).
- **Memory Management**:
  - DOS-style Memory Control Block (MCB) allocator managing ~574 KB of conventional memory (`0x0060`–`0x8FFF`).
  - Driver for Lo-tech 2 MB Expanded Memory (EMS) across four 16 KB slots at `0xE000`.
  - Automatic memory and EMS reclamation on process termination.
- **Display & Graphics**:
  - Snow-free 80x25 16-color CGA text driver synchronized to hardware retrace.
  - CGA graphics support: 320x200 (4-color) and 640x200 (2-color), hardware palette control, drawing primitives, BIOS font rendering, and double-buffered linear blitting.
- **Input & Keymaps**:
  - Hardware PC/XT `IRQ 1` keyboard driver with live key-down state tracking.
  - Modular `.MAP` layouts supporting US QWERTY, FR AZERTY, and BE AZERTY.
- **Hardware Detection**:
  - Intel 8087 FPU detection and graceful fallback handling.
- **Built-in User-Space Tools**:
  - `/BIN/SHELL.BIN`: Interactive shell with file manipulation commands (`ls`, `cd`, `pwd`, `mkdir`, `rmdir`, `cat`, `write`, `cp`, `mv`, `rm`, `cls`, `run`).
  - `/BIN/EDIT.BIN`: Nano-inspired full-screen text editor supporting up to 60 KB files, search, and cut/paste.
  - `/BIN/FORTH.BIN`: 16-bit real-mode JONESFORTH interpreter (Indirect Threaded Code, ~70 core words, compiler, and REPL).
  - `/BIN/MEM.BIN`: Memory reporting tool for conventional and EMS RAM.
  - `/BIN/KEYB.BIN`: Dynamic keyboard layout loader.

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

This compiles the bootloader, kernel, user applications, and keymaps, packaging them into `book8088/disk.img`.

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
