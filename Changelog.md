# Changelog

## Revision 0.6 (2026-11-16)

- ** Changed i8087 FPU to x87 FPU detected at boot banner an in /BIN/MEM.BIN as we just check the fpu is present, unlike dectecting the CPU type, however fpu usually match there respective CPU.
- **XMS 2.0 Extended Memory Driver & A20 Gate Manager (`kernel/xms.c`, `kernel/entry.asm`, `kernel/syscall.c`, `kernel/main.c`, `include/hobbyos.h`, `user/mem.c`, `Makefile`)**:
  - Implemented a native XMS 2.0 driver that activates whenever a 286+ CPU is detected:
    - **A20 Gate Control**: Real-mode memory aliasing probe (`0000:0500` vs `FFFF:0510`) with multi-method activation (BIOS `INT 0x15, AX=0x2401`, Fast A20 Port `0x92`, and 8042 Keyboard Controller Port `0x64`/`0x60`).
    - **Extended Memory Detection & Allocation**: Queries extended RAM above 1 MB via BIOS `INT 0x15, AH=0x88`. Manages Extended Memory Blocks (EMBs) up to 16 handles with automatic per-process ownership reclamation on exit.
    - **Block Move (`INT 0x15, AH=0x87`)**: Sets up real-mode 48-byte GDT tables and executes chunked data transfers between conventional and extended memory.
    - **Standard `INT 0x2F` Multiplex API**: Handlers installed for `AX=0x4300` (installation check) and `AX=0x4310` (get driver entry point), supporting standard XMS calls (versions, HMA, A20 query/control, EMB allocate/free/move/lock/unlock/info).
    - **`INT 0x21` Syscalls**: Added `SYS_XMS_ALLOC` (`0x44` / `os_xms_alloc`), `SYS_XMS_FREE` (`0x45` / `os_xms_free`), `SYS_XMS_MOVE` (`0x46` / `os_xms_move`), and `SYS_XMS_AVAIL` (`0x47` / `os_xms_avail` & `os_xms_total`).
    - **System Reporting**: Boot banner reports XMS driver status or explains why it is unavailable (`[XMS] Not supported (requires 80286+ CPU)` on 8088/8086). `/BIN/MEM.BIN` reports XMS extended memory alongside Conventional and EMS.
- **Pre-CPUID Classic x86 CPU & Bus Width Detection (`kernel/entry.asm`, `kernel/main.c`, `kernel/syscall.c`, `include/hobbyos.h`, `user/mem.c`, `user/hello.c`)**:
  - Implemented classic real-mode processor detection in `_cpu_detect_type` (`kernel/entry.asm`) targeting Intel 8088, Intel 8086, NEC V20, NEC V30, Intel 80188, Intel 80186, Intel 80286, and Intel 80386+ without CPUID:
    - **FLAGS Bits 12–15 test**: Differentiates 8086/8088/V20/V30/186/188 (where bits 12–15 are hardwired to 1) from 80286+ (where bits 12–15 are cleared in real mode).
    - **IOPL Flag test**: Distinguishes 80286 (bits 12–14 cannot be set in real mode) from 80386+ (bits 12–14 can be modified).
    - **Undocumented `AAM` Divisor Quirk**: `AAM 3` (`0xD4 0x03`) divides `AL` (5) by 3 on Intel CPUs (`AH = 1`, `AL = 2`), while NEC V20/V30 microcode ignores the divisor byte and always divides by 10 (`AH = 0`, `AL = 5`).
    - **Shift Count Masking**: `SHL AL, 33` shifts out all bits on 8086/8088 (`AL = 0`), whereas 80186/80188 masks the count to 5 bits (`AL = 2`).
    - **Self-Modifying Prefetch Queue Size Test**: Under `CLI`, dynamically overwrites instructions ahead in RAM using `STD` and `REP STOSB` with `STI` (`0xFB`) across the 4-byte / 6-byte prefetch queue boundary. 8-bit bus processors (8088, V20, 80188) with 4-byte prefetch queues fetch the updated `STI` (skipping `INC DX`), while 16-bit bus processors (8086, V30, 80186) with 6-byte prefetch queues have already prefetched `INC DX` into the BIU before the memory store occurs. Original instruction bytes are restored cleanly after detection.
  - Added `SYS_CPU_TYPE` (`0x0B` / `os_cpu_type()`), `CPU_TYPE_*` constants, and `os_cpu_name()` in `include/hobbyos.h`.
  - Displayed detected CPU model and bus/queue characteristics at boot banner in `kmain()` (`[CPU] Detected: Intel 8088 (8-bit bus, 4-byte prefetch queue)`).
  - Updated `/BIN/MEM.BIN` to report CPU architecture alongside memory and FPU status, and added boot self-test verification in `/BIN/HELLO.BIN`.
- **JONESFORTH 16-Bit Real Mode Port (`user/forth.asm`, `Makefile`, `user/shell.c`)**:
  - Ported Richard W.M. Jones' public domain educational Forth interpreter (`JONESFORTH`) to 16-bit 8088 real mode as `/BIN/FORTH.BIN`.
  - Implemented an Indirect Threaded Code (ITC) inner interpreter (`NEXT`, `DOCOL`, `EXIT`) optimized for the strict 8086/8088 instruction set (`lodsw`, `mov bx, ax`, `jmp word [bx]`).
  - Allocated Forth registers: `SI` as the Forth Instruction Pointer (IP), hardware `SP` as the Data Stack Pointer (DSP), and `BP` as the Return Stack Pointer (RSP).
  - Implemented ~70 core primitives and words in NASM:
    - Stack manipulation: `DROP`, `SWAP`, `DUP`, `OVER`, `ROT`, `-ROT`, `2DROP`, `2DUP`, `?DUP`, `>R`, `R>`, `R@`, `RSP@`, `RSP!`, `RDROP`, `DSP@`, `DSP!`.
    - Arithmetic & logic: `+`, `-`, `*`, `/MOD`, `/`, `MOD`, `1+`, `1-`, `2+`, `2-`, `AND`, `OR`, `XOR`, `INVERT`, `NEGATE`.
    - Comparisons: `=`, `<>`, `<`, `>`, `<=`, `>=`, `0=`, `0<>`, `0<`, `0>`.
    - Memory & dictionary: `!`, `@`, `+!`, `-!`, `C!`, `C@`, `CMOVE`, `HERE`, `LATEST`, `STATE`, `BASE`, `S0`, `R0`, `DOCOL`, `F_IMMED`, `F_HIDDEN`, `F_LENMASK`.
    - Compiler & control flow: `:`, `;`, `CREATE`, `,`, `[`, `]`, `'`, `IMMEDIATE`, `HIDDEN`, `BRANCH`, `0BRANCH`, `LIT`, `LITSTRING`, `EXECUTE`, `IF`, `ELSE`, `THEN`, `BEGIN`, `UNTIL`, `AGAIN`, `WHILE`, `REPEAT`, `CONSTANT`, `VARIABLE`.
    - I/O & formatting: `KEY`, `EMIT`, `CR`, `SPACE`, `SPACES`, `.`, `U.`, `.S`, `TELL`, `."`, `WORDS`, `BYE`.
  - Built-in interactive REPL with line buffering and backspace editing, command-line argument evaluation (`0xFF00`), and clean exit back to the shell via `SYS_EXIT` (`BYE`).
- **Cursor Visibility Syscall & Flicker-Free `EDIT.BIN` Redraw (`kernel/console.c`, `kernel/syscall.c`, `include/hobbyos.h`, `user/edit.c`)**:
  - Added `SYS_CURSOR_VISIBLE` (`0x0A` / `os_set_cursor_visible`) to toggle hardware cursor visibility (parking it off-screen when disabled).
  - Updated `exec_program` in `kernel/syscall.c` to automatically restore cursor visibility whenever a process exits.
  - Updated `edit` redraw routine to hide the hardware cursor before redrawing screen rows and re-enable it only after placing it at the final cursor position, eliminating cursor jumping/racing artifacts during editing and navigation.
- **Global `PATH` Syscalls & Executable Search (`kernel/syscall.c`, `include/hobbyos.h`)**:
  - Implemented kernel-level global `PATH` storage and management via `SYS_GET_PATH` (`0x21` / `os_get_path`) and `SYS_SET_PATH` (`0x22` / `os_set_path`).
  - Updated `resolve_exec_target` to dynamically search along all directories defined in `PATH` (delimited by `;`) for both exact matches and automatically resolved `.BIN` executables.
- **Shell Environment Variables & `.sh` Script Engine (`user/shell.c`, `boot.sh`, `Makefile`)**:
  - Implemented shell-level environment variables (`set [NAME=VAL]`, `NAME=VAL`), special system variables (`$?`, `$CWD`, `$PATH`), and variable expansion (`$VAR`, `${VAR}`). Setting `PATH` automatically updates the global kernel `PATH`.
  - Added native execution of `.sh` batch scripts with line-by-line interpretation, `#` comment support, `@` prefix line-echo suppression, `echo [text|on|off]`, labels (`:label`), `goto <label>`, flow control (`if [not] <a> == <b> <cmd>`, `if [not] exist <file> <cmd>`), and argument passing (`$1`..`$9`).
  - Added automatic execution of `/BOOT.SH` at shell startup, populating a default `/BOOT.SH` setting `PATH=.;/BIN` on the FAT16 system disk image.

## Revision 0.5 (2026-11-15)

- **Custom `IRQ 1` (`INT 0x09`) Keyboard Driver & Live Key State Bitmap (`kernel/entry.asm`, `kernel/kbd.c`, `kernel/console.c`, `kernel/main.c`)**:
  - Replaced BIOS `INT 0x16` polling with a hardware `INT 0x09` (`IRQ 1`) ISR reading raw PC/XT Set 1 make/break scancodes from 8255 Port A (`0x60`), acknowledging via Port B (`0x61`), and issuing EOI to the 8259 PIC (`0x20`).
  - Added full tracking for `0xE0` extended scancodes, modifiers (`LShift`, `RShift`, `Ctrl`, `Alt`, `AltGr`, `CapsLock`, `NumLock`), BIOS Data Area shift flag synchronization (`0x0040:0x0017`), a 32-entry key event ring buffer (`cpu_halt` idle wait), and a 128-scancode (`16`-byte) live key-down state bitmap for games.
  - Updated `con_getline` (`kernel/console.c`) and `read_line` (`user/shell.c`) to accept printable extended CP437 characters (`é`, `è`, `ç`, `à`, `ù`, `²`, `£`, `µ`, `°`, `§`).
- **Loadable `.MAP` Keyboard Layouts & `/BIN/KEYB.BIN` Utility (`include/hobbyos.h`, `kernel/syscall.c`, `user/keyb.c`, `tools/mkkeymap.py`, `Makefile`)**:
  - Defined the 392-byte `os_keymap_t` `.MAP` binary format (`8`-byte layout name + `128`-byte `normal`, `shift`, and `altgr` scancode tables) and `INT 0x21` syscalls: `SYS_KEYB_SET_MAP` (`0x50` / `os_keyb_set_map`), `SYS_KEYB_GET_NAME` (`0x51` / `os_keyb_get_name`), and `SYS_KEYB_STATE` (`0x52` / `os_keyb_state`).
  - Added `tools/mkkeymap.py` and populated `/KEYMAPS/US.MAP` (US QWERTY), `/KEYMAPS/FR.MAP` (French AZERTY), and `/KEYMAPS/BE.MAP` (Belgian AZERTY) on the FAT16 volume while keeping `US` compiled into the kernel as the default boot layout.
  - Added user-space `/BIN/KEYB.BIN` (`keyb [us|fr|be|/path/to/map]`) to inspect the active layout, list `/KEYMAPS`, and switch keyboard mappings at runtime.
- **EMS Presence Detection, Slot Preservation & `MEM.BIN` Reporting (`kernel/ems.c`, `kernel/main.c`, `kernel/syscall.c`, `include/hobbyos.h`, `user/mem.c`)**:
  - Hardened `ems_init()` to verify both Page 0 (`0xAA55`) and Page 1 (`0x1234`) and zero the probe words afterward, updated `ems_map_page()` to require an allocated page, and added `ems_save_slots` / `ems_restore_slots` across `exec_program`.
  - Updated `SYS_EMS_AVAIL` (`0x43`) to return `AX = free_pages` (`0..128`, `0` when absent) and `DX = total_pages` (`128` when present, `0` when absent via `os_ems_total()`), and updated boot log output to `[LOTECH EMS] Not found`.
  - Updated `/BIN/MEM.BIN` to report `0 KB` Total / `0 KB` Used / `0 KB` Free (`0 / 0 (not present)`) when the EMS card is absent, and suppressed `mem -altman` output when the card is absent or when free EMS is `<= 10%`.
- **8087 FPU Presence Syscall & User-Space Guard (`kernel/entry.asm`, `kernel/main.c`, `kernel/syscall.c`, `include/hobbyos.h`)**:
  - Updated `_fpu_8087_init` to use the no-wait `fninit` + `fnstsw` + `fnstcw` probe (avoiding `fwait` before confirming 8087 presence) and isolated boot floating-point verification inside `verify_fpu_math()`.
  - Added `SYS_FPU_PRESENT` (`0x09` / `os_fpu_present()`) and the `os_require_fpu()` inline helper in `include/hobbyos.h` so FPU-dependent programs can fail gracefully with `"FPU not present\n"`.
- **File Copy, Concatenation & Move (`kernel/fat16.c`, `kernel/syscall.c`, `include/hobbyos.h`, `user/shell.c`)**:
  - Added `SYS_FILE_COPY` (`0x1A` / `os_file_copy`) and `SYS_FILE_RENAME` (`0x1B` / `os_file_rename`) in `kernel/fat16.c` (`fat16_copy_file` sector-by-sector copy/append and `fat16_rename_file` directory-entry move).
  - Added `cp` (`copy`) and `mv` (`move` / `ren`) built-ins to `/BIN/SHELL.BIN` supporting file or directory destinations, MS-DOS style `cp a+b dst` file concatenation, and interactive overwrite confirmation (`Overwrite <dst>? (y/n): `) when the destination file already exists.
- **CGA Graphics Subsystem & Linear Backbuffer Blitter (`kernel/gfx.c`, `kernel/entry.asm`, `kernel/syscall.c`, `include/hobbyos.h`)**:
  - Added CGA graphics mode driver supporting `320x200` 4-color (`OS_MODE_CGA_320x200` `0x04` and `OS_MODE_CGA_320x200_ALT` `0x05`), `640x200` 2-color (`OS_MODE_CGA_640x200` `0x06`), and `80x25` 16-color text (`OS_MODE_TEXT_80x25` `0x03`), with automatic restoration of 80x25 text mode when a process exits.
  - Added palette/color control on Ports `0x3D8`/`0x3D9` (Palette 0 Green/Red/Brown, Palette 1 Cyan/Magenta/White, Palette 2 Cyan/Red/White, high/low intensity, and 16 background/foreground colors) and vertical retrace synchronization (`_cga_wait_vsync` on Port `0x3DA`).
  - Added kernel drawing primitives (`gfx_clear`, `gfx_putpixel`, integer Bresenham `gfx_line`, outline/filled `gfx_rect`, and `gfx_draw_text` using the 8x8 IBM BIOS ROM font at `0xF000:0xFA6E`) and a fast 8086 assembly linear-to-interleaved blitter (`_cga_blit_linear`, splitting a `16,000`-byte linear buffer into even scanlines at `0xB800:0000` and odd scanlines at `0xB800:2000`).
  - Exposed `INT 0x21` graphics syscalls (`0x60`–`0x69`: `SYS_GFX_SET_MODE`, `SYS_GFX_GET_MODE`, `SYS_GFX_PALETTE`, `SYS_GFX_VSYNC`, `SYS_GFX_CLEAR`, `SYS_GFX_PUTPIXEL`, `SYS_GFX_LINE`, `SYS_GFX_RECT`, `SYS_GFX_TEXT`, `SYS_GFX_BLIT`) and fast inline backbuffer pixel helpers (`os_buf_putpixel_320`, `os_buf_putpixel_640`).
- **`/BIN/EDIT.BIN` Text Editor (`user/edit.c`, `Makefile`, `user/shell.c`)**:
  - Added a lean nano-inspired full-screen editor (`edit <file>`) with a title bar (file name + `Modified`), 22 text lines, a status line (messages + `Ln`/`Col`), and a key-help bar.
  - Editing: arrows, Home/End, PgUp/PgDn, Backspace/Del, Enter, Tab (4-column stops), printable CP437 characters, horizontal scrolling for long lines, `^O` save (asks before overwriting a different existing file), `^X` exit (asks to save a modified buffer), `^K`/`^U` one-line cut/paste, and `^W` case-insensitive forward search with wrap-around.
  - Text is held in a gap buffer inside a 64 KB `os_mem_alloc` block (60 KB text + 4 KB clipboard); DOS `CR LF` line endings are converted to `LF` on load.
- **Kernel Fixes (`kernel/syscall.c`, `kernel/kbd.c`)**:
  - `SYS_FILE_READ`, `SYS_FILE_WRITE` and `SYS_FILE_APPEND` no longer report an error for transfers larger than 32 KB (the byte count was being treated as a negative `int`).
  - The `IRQ 1` driver now ignores the fake Shift codes (`E0 2A` / `E0 36`) that 101-key keyboards send around the grey navigation keys.

## Revision 0.4 (2026-11-14)

- **Hierarchical FAT16 Directory Support (`kernel/fat16.c`, `tools/fat16.py`, `include/hobbyos.h`, `kernel/syscall.c`)**:
  - Extended the kernel FAT16 driver to support hierarchical subdirectories (`FAT_ATTR_DIRECTORY`), `.` and `..` navigation, `/` and `\` path separators, relative and absolute paths, and automatic subdirectory cluster-chain extension when a subdirectory fills up.
  - Added kernel-tracked Current Working Directory (`cwd_cluster`, `cwd_path`) and new `INT 0x21` directory & file syscalls: `SYS_MKDIR` (`0x15` / `os_mkdir`), `SYS_RMDIR` (`0x16` / `os_rmdir`), `SYS_CHDIR` (`0x17` / `os_chdir`), `SYS_GETCWD` (`0x18` / `os_getcwd`), and `SYS_FILE_APPEND` (`0x19` / `os_file_append`).
  - Updated `tools/fat16.py` to create subdirectories and populate `/BIN/SHELL.BIN`, `/BIN/MEM.BIN`, and `/BIN/HELLO.BIN`.
  - Updated `exec_program` to search the Current Working Directory first and fall back to `/BIN` (automatically resolving `.BIN` extensions).
- **User-Space Interactive Shell (`user/shell.c`, `kernel/main.c`)**:
  - Moved the interactive command shell out of `kernel/main.c` into user-space `/BIN/SHELL.BIN` (`user/shell.c`).
  - Added `cd`, `pwd`, `mkdir`, and `rmdir` commands alongside `help`, `ls [dir]` / `dir [dir]`, `cat`, `write`, `rm`, `run`, and `cls`, with the prompt displaying the active directory (`hobbyOS:/PATH> `).
  - Updated `write` to echo quoted strings to the console when no file is given (`write "text"`), write with a trailing newline to a file (`write <file> "text"`), and append to an existing file with `-a` (`write -a <file> "text"`).
- **`mem -altman` Easter Egg (`user/mem.c`)**:
  - Added hidden `-altman` (or `/altman`) flag to `/BIN/MEM.BIN` (`user/mem.c`) reporting the percentage, KiB, and page count of free Lo-tech 2MB EMS at `0xE000` in the style of the `ram-altman` letter.
- **Snow-Free CGA Text Output (`kernel/entry.asm`, `kernel/console.c`)**:
  - Eliminated CGA 80x25 VRAM bus contention ("snow") by adding a 4 KB kernel RAM shadow buffer (`cga_shadow`) and synchronizing all single-cell and bulk VRAM writes (`_cga_write_cell`, `_cga_sync_copy`) to the CGA status register (`0x3DA`) vertical blanking (`0x08`) and horizontal retrace rising edge (`0x01`).

## Revision 0.3 (2026-11-14)

- **Kernel Relocation & Low Memory Reclamation (`boot/boot.asm`, `kernel/kernel.h`)**:
  - Moved the kernel from segment `0x1000` to the top of conventional RAM at segment `0x9000` (`576 KB`–`640 KB`).
  - Reclaimed the bootloader low-memory region (`0x0060`–`0x8FFF`), creating a single contiguous `574.5 KB` conventional memory arena.
- **Conventional Memory MCB Paragraph Allocator (`kernel/mem.c`, `kernel/syscall.c`, `include/hobbyos.h`)**:
  - Implemented a DOS-style Memory Control Block (MCB) paragraph allocator (`16`-byte granularity) across segments `0x0060`–`0x8FFF` with block splitting, free-block coalescing, and per-process ownership tracking.
  - Updated `exec_program` to dynamically allocate user process segments from the MCB arena and automatically reclaim process memory upon `SYS_EXIT`.
  - Added `INT 0x21` conventional memory syscalls: `SYS_MEM_ALLOC` (`0x30`), `SYS_MEM_FREE` (`0x31`), and `SYS_MEM_AVAIL` (`0x32`).
- **Lo-tech 2 MB EMS Page Manager (`kernel/ems.c`, `kernel/syscall.c`, `include/hobbyos.h`)**:
  - Added a direct page allocator and bank-switching manager for all `128` physical `16 KB` EMS pages (`2 MB`) across the `4` page frame slots at `0xE000` (`I/O 0x260`–`0x263`), with automatic process cleanup on exit.
  - Added `INT 0x21` EMS syscalls: `SYS_EMS_ALLOC` (`0x40`), `SYS_EMS_FREE` (`0x41`), `SYS_EMS_MAP` (`0x42`), and `SYS_EMS_AVAIL` (`0x43`).
- **`MEM.BIN` Utility (`user/mem.c`, `kernel/syscall.c`, `Makefile`)**:
  - Added user-space `MEM.BIN` application modeled after MS-DOS `mem`, reporting Conventional and EMS totals/used/free KB, walking the MCB chain (`0x0060`–`0x8FFF`), and displaying the largest contiguous executable block and free EMS pages.
  - Updated `exec_program` to automatically resolve `.BIN` extensions so commands like `mem` or `hello` can be invoked without typing `.bin`.

## Revision 0.2 (2026-11-14)

- **System Call Interface & Public API (`include/hobbyos.h`, `kernel/syscall.c`, `kernel/entry.asm`)**:
  - Implemented `INT 0x21` system call handler switching safely to the kernel data/stack segment (`0x1000`) during dispatch.
  - Added public Open Watcom `#pragma aux` wrappers for user-space and kernel callers: `os_exit`, `os_putc`, `os_puts`, `os_getc`, `os_kbhit`, `os_cls`, `os_set_cursor`, `os_get_cursor`, `os_get_args`, `os_file_read`, `os_file_write`, `os_file_size`, `os_file_delete`, `os_dir_entry`, and `os_exec`.
- **Screen & Keyboard Drivers (`kernel/console.c`, `kernel/entry.asm`)**:
  - Added direct CGA 80x25 text-mode screen driver (`0xB800:0000`) with hardware CRTC cursor updates (`0x3D4`/`0x3D5`), BIOS Data Area cursor synchronization (`0x0040:0x0050`), colors, newline/tab/backspace handling, and scrolling.
  - Added PC/XT keyboard driver via BIOS `int 0x16` (`AH=00h`/`01h`) and line editing (`con_getline`).
  - Dropped `COM1` serial console output/input usage while retaining `serial_init()` for future serial mouse support.
- **Kernel FAT16 Driver (`kernel/fat16.c`)**:
  - Implemented full FAT16 volume support via BIOS `int 0x13` (`AH=02h`/`03h`): BPB parsing, directory enumeration, file size query, file reading (`fat16_read_file`), file creation/overwriting with dual-FAT cluster allocation (`fat16_write_file`), file deletion (`fat16_delete_file`), and multi-segment executable loading (`fat16_load_executable`).
- **Executable Loader, User-Space Runtime & Interactive Shell (`user/crt0.asm`, `user/hello.c`, `kernel/main.c`)**:
  - Added `exec_program` and `_proc_exec_far` / `_proc_exit_usermode` to load flat 16-bit binaries into user segment `0x2000:0000`, pass command-line arguments at `0x2000:0xFF00`, and restore kernel state upon `SYS_EXIT`.
  - Added user-space runtime (`user/crt0.asm`) and sample user program (`HELLO.BIN`) that tests console output, FAT16 file write/read (`NOTES.TXT`), and directory listing via `INT 0x21`.
  - Added interactive kernel shell (`hobbyOS> `) supporting `help`, `ls`/`dir`, `cat`, `write`, `rm`, `run`, and `cls`.

## Revision 0.1 (2026-11-14)

- **FAT16 Bootloader (`boot/boot.asm`)**:
  - Replaced raw hardcoded sector reads with a real FAT16 Volume Boot Record (VBR) including a standard BIOS Parameter Block (BPB) and Extended BPB (`4` heads, `17` sectors/track, `512` bytes/sector, `4` sectors/cluster).
  - Enforced strict 8086/8088 instruction set compatibility (`CPU 8086`).
  - Dynamically computes FAT, Root Directory, and Data region LBAs from the BPB.
  - Searches the FAT16 Root Directory for `KERNEL.BIN`, follows its FAT16 cluster chain, converts LBAs to CHS for BIOS `int 0x13` (`AH=0x02`), loads the kernel into `0x1000:0x0000` (with 64 KB segment wrap handling), and jumps to it.
  - Added BIOS `int 0x10` status indicators: `B` (boot started), `K` (kernel loaded), `N` (`KERNEL.BIN` not found), and `E` (disk I/O error).
- **FAT16 Disk Image Pipeline (`tools/fat16.py` & `Makefile`)**:
  - Formatted `book8088/disk.img` as a standard FAT16 volume (`HOBBYOS`) using `mkfs.fat`.
  - Added `tools/fat16.py` to install the compiled VBR boot code while preserving the BPB and copy `build/kernel.bin` into the FAT16 root directory as `KERNEL.BIN`.
  - Updated `make agent-test` to run 86Box headlessly via `QT_QPA_PLATFORM=offscreen`.
