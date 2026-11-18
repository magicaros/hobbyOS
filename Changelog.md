# Changelog

## Revision 0.15 (2026-11-18)

- **HobbyOS Archiver (`HAR.BIN`), Stream Offset Syscall & LZ4 Compression (`user/har.c`, `kernel/fat16.c`, `kernel/syscall.c`, `kernel/kernel.h`, `include/hobbyos.h`, `user/shell.c`, `Makefile`)**:
  - **Large File (> 32 KB) I/O Support & Signed Integer Fix (`kernel/fat16.c`, `kernel/syscall.c`, `kernel/kernel.h`, `include/hobbyos.h`, `user/har.c`, `user/shell.c`, `user/chkdsk.c`)**:
    - Fixed 16-bit signed integer overflow where files $\ge 32,768$ bytes (such as `KERNEL.BIN` at 35,653 bytes) produced negative return values when cast to signed `int` (`-29975 < 0`).
    - Changed `fat16_read_file`, `fat16_read_file_offset`, `fat16_write_file`, and `fat16_append_file` to return `int32_t`, cleanly separating 0..65534 byte counts from negative error codes (`-1`, `-3`).
    - Updated `os_file_read`, `os_file_read_offset`, `os_file_write`, and `os_file_append` prototypes in `include/hobbyos.h` to `uint16_t` (returning `0xFFFF` on failure).
    - Updated `user/har.c` to use unsigned return comparisons (`r == 0xFFFF || r != sz`), allowing archiving, appending, testing, and extraction of files up to the 48 KB buffer ceiling without false read/write errors.
    - Fixed `ls` (`cmd_ls` in `user/shell.c`) truncating file sizes $\ge 64$ KB by printing full 32-bit `dent.size` (`print_uint32`) instead of casting to `uint16_t` (which displayed 71,067 B as 5,531 B).
    - Implemented safe kernel bounce buffering in `vol_read_sector()` and `vol_write_sector()` via `KERNEL_SEG io_buf` (`kernel/fat16.c`), eliminating BIOS `INT 0x13` DMA 64K boundary crossing errors (`AH=0x09`) on floppy drives.
    - Updated `user/chkdsk.c` FAT12 cross-sector cluster lookup (clusters 341 and 682) to use static sector buffer `next_fat_buf` instead of allocating 512 bytes on the user stack.
  - **Recursive Directory Archiving & Append Switch `-a` (`user/har.c`, `user/shell.c`)**:
    - Implemented recursive directory traversal for both `har -c` and `har -a`: directories specified on the command line are recursively scanned, preserving relative subdirectory paths within the archive.
    - Implemented `-a` append switch: appends files and subdirectories directly to existing `.HAR` archives using streamable chunked headers.
    - Added directory tree recreation on extraction (`har -x`): automatically invokes `os_mkdir` via `ensure_parent_dirs()` to rebuild parent directories before writing files.
  - **Random Access File Stream Syscall (`include/hobbyos.h`, `kernel/syscall.c`, `kernel/fat16.c`, `kernel/kernel.h`)**:
    - Added `SYS_FILE_READ_OFFSET` (`0x2B`, `os_file_read_offset(filename, buf, max_len, offset_32)`): Enables reading files from arbitrary 32-bit byte offsets across cluster chains without library dependencies or memory allocation. Fast-forwards cluster lookups via bit shifts, supporting arbitrary-sized file streaming on real-mode 8086 systems.
  - **Pure 8086 Freestanding LZ4 Compression Engine (`user/har.c`)**:
    - Ported Yann Collet's LZ4 block compression and decompression algorithm to freestanding real-mode 8086 C (`-0 -fpi87 -zl`).
    - Decompressor: Ultra-compact, high-speed 16-bit block decompressor supporting overlapping token matches and RLE runs.
    - Compressor: 4096-entry (8 KB) hash table with pure 16-bit hash arithmetic (`(w0 * 40503U) ^ w1`) compiling to single 8086 `mul` instructions without 32-bit math helpers. Automatically falls back to uncompressed storage if compression expands data.
    - Checksum: Integrated CRC-16 computation (`har_crc16`) for end-to-end data integrity verification.
  - **Custom Streamable Archive Format (`.HAR`)**:
    - Compact container: 8-byte archive header (`"HAR1"`), 16-byte packed file entry headers (`"HF"`), relative filenames, CRC16, and end-of-archive marker (`"HE"`).
    - Chunked file appending: Uses `os_file_append()` to stream files directly into the archive without requiring multi-megabyte staging buffers.
  - **User-Space Archiver Utility `/BIN/HAR.BIN` (`user/har.c`, `Makefile`, `user/shell.c`)**:
    - `har -c <archive.har> <files...>`: Compresses and creates archives (supports wildcards `*` and `?`).
    - `har -x <archive.har> [dest_dir]`: Extracts files from archive to current or target volume/directory (e.g. `RAM:`).
    - `har -t <archive.har>`: Tests archive integrity, validating payload decompression and CRC16 checksums.
    - `har -l <archive.har>`: Lists archive contents, uncompressed/compressed sizes, compression ratios, and CRC16.
    - Memory reporting: Displays available conventional memory, EMS, and XMS capacity.
    - Added `har` to shell `help` command.
- **RAM Disk Architecture, Syscall API & `RAMDISK.BIN` Utility (`user/ramdisk.c`, `kernel/fat16.c`, `kernel/ems.c`, `kernel/xms.c`, `kernel/syscall.c`, `kernel/kernel.h`, `include/hobbyos.h`, `user/shell.c`, `Makefile`)**:
  - **RAM Disk Syscalls (`include/hobbyos.h`, `kernel/syscall.c`, `kernel/fat16.c`, `kernel/kernel.h`)**:
    - Added `SYS_RAMDISK_MOUNT` (`0x28`, `os_ramdisk_mount(type, size_kb, &mounted_kb)`): Allocates EMS or XMS memory belonging to `KERNEL_SEG` (`0x9000`) so the disk survives user process termination, generates an in-memory FAT12 VBR (Sector 0) and initial FAT tables (Sector 1 with media descriptor `0xF8`), registers volume `RAM:`, and mounts it dynamically.
    - Added `SYS_RAMDISK_UNMOUNT` (`0x29`, `os_ramdisk_unmount()`): Flushes FAT cache, frees allocated EMS pages / XMS handle, unregisters volume `RAM:`, and resets shell current volume to `SYS:` if needed.
    - Added `SYS_RAMDISK_STATUS` (`0x2A`, `os_ramdisk_status(&status)`): Reports active status, memory backing type (`RAMDISK_TYPE_EMS` vs `RAMDISK_TYPE_XMS`), allocated capacity in KB, cluster size, and cluster metrics.
  - **EMS & XMS Sector I/O Engines (`kernel/ems.c`, `kernel/xms.c`, `kernel/fat16.c`)**:
    - Implemented `ems_rw_sector(phys_page, sec_offset, is_write, buf)`: Transparently maps physical 16 KB pages into EMS Slot 0 (`0xE000:0000`), copies 512 bytes (256 words), and restores previous page mappings.
    - Implemented `xms_rw_sector(handle, byte_offset, is_write, buf)`: Sets up `os_xms_move_t` and performs extended memory sector transfers via `xms_move()`.
    - Integrated with volume manager `vol_rw_lba()` using dummy BIOS drive `0xFF`.
    - Preserved `RAM:` volume across `fat16_rescan_drives()`, preventing partition rescans from destroying active RAM disks.
  - **`RAMDISK.BIN` User Utility (`user/ramdisk.c`, `Makefile`, `user/shell.c`)**:
    - Freestanding 8086 C utility supporting:
      - `/e [size|MAX]`: Mounts RAM disk in Lo-tech EMS memory (e.g., `ramdisk /e 512` or `ramdisk /e MAX`).
      - `/x [size|MAX]`: Mounts RAM disk in XMS extended memory on 286+ systems.
      - `/u`: Unmounts active RAM disk and releases memory back to system pools.
      - `/s` or no arguments: Displays active status, memory type, capacity, free disk space, and total available EMS/XMS memory.
  - **Shell Integration (`user/shell.c`)**:
    - Updated `lsdisk` to detect and format `RAM:` as `RAM Disk (FAT12) | ... KB total (... KB free)`.
    - Added `ramdisk` to shell `help` command list.
- **Raw Volume Sector Syscalls & `CHKDSK.BIN` Integrity Utility (`user/chkdsk.c`, `kernel/fat16.c`, `kernel/syscall.c`, `kernel/kernel.h`, `include/hobbyos.h`, `user/shell.c`, `Makefile`)**:
  - **Raw Logical Volume Sector Syscalls (`include/hobbyos.h`, `kernel/syscall.c`, `kernel/fat16.c`, `kernel/kernel.h`)**:
    - Added `SYS_VOL_READ_SECTOR` (`0x26`, `os_vol_read_sector(vol, lba, buf)`): Reads raw 512-byte sectors relative to a logical volume (`HD0A:`, `SYS:`, `FD0:`, etc.), translating volume partition start LBA, drive geometry, and removable media auto-mounting.
    - Added `SYS_VOL_WRITE_SECTOR` (`0x27`, `os_vol_write_sector(vol, lba, buf)`): Writes raw 512-byte sectors to a logical volume and invalidates kernel FAT cache to ensure consistency.
  - **`CHKDSK.BIN` Filesystem Integrity & Repair Utility (`user/chkdsk.c`, `Makefile`)**:
    - Standalone user-space utility written in freestanding 8086 C (`-0 -fpi87 -zl`).
    - BPB & Boot Sector Validation: Checks `0x55AA` signature, bytes per sector (512), power-of-2 sectors per cluster, reserved sectors, and partition geometry.
    - FAT Table Consistency: Compares primary and secondary FAT copies sector-by-sector, reporting or synchronizing differences with `/F`.
    - Directory Tree Traversal: Iterative queue-based traversal of Root Directory and subdirectories, eliminating C stack recursion limits on 8086.
    - Cluster Chain Validation: Traces FAT12 and FAT16 chains, detecting cross-linked clusters, out-of-range cluster numbers, and infinite circular chain loops.
    - Lost Cluster Detection & Repair: Dynamically allocates an 8,192-byte bitmap via `os_mem_alloc()` to track referenced clusters across all directories, identifies unreferenced orphan clusters in the FAT, and optionally frees them with `/F`.
    - Verbose Mode (`/V`): Lists each file, directory, and byte size as it is inspected.
    - Full MS-DOS Compatible Summary: Reports volume label, serial number, bytes in user files, hidden files, directories, bad sectors, available disk space, allocation unit metrics, and conventional 640 KB memory status (`SYS_MEM_AVAIL`).
  - **Shell Integration (`user/shell.c`)**:
    - Added `chkdsk` to shell `help` command list.
- **VGA Graphics API Expansion & `GFXDEMO.BIN` Showcase (`kernel/gfx.c`, `kernel/entry.asm`, `kernel/syscall.c`, `kernel/kernel.h`, `include/hobbyos.h`, `user/gfxdemo.c`, `user/shell.c`, `Makefile`)**:
  - **VGA Mode Support & Hardware DAC Control (`include/hobbyos.h`, `kernel/gfx.c`, `kernel/syscall.c`, `kernel/kernel.h`)**:
    - Defined `OS_MODE_VGA_640x480` (`0x12`, 640x480 16-color planar graphics) and `OS_MODE_VGA_320x200_256` (`0x13`, 320x200 256-color packed-pixel linear graphics).
    - Added `SYS_GFX_SET_DAC` (`0x6A`, `os_gfx_set_dac(index, r, g, b)`): Sets hardware VGA DAC color registers via I/O ports `0x3C8` (PEL address write) and `0x3C9` (PEL data RGB).
    - Added VGA adapter verification in `gfx_set_mode()`: Validates `video_get_type() >= VIDEO_TYPE_VGA` before setting Modes `0x12` or `0x13`, falling back gracefully to text mode if unsupported.
    - Planar Graphics Controller engine for Mode `0x12`: Implemented `gfx_clear()` using Set/Reset registers (`0x3CE` index 0/1) filling all 4 bitplanes simultaneously, and `gfx_putpixel()` using latch read/write cycles and bitmask selection.
    - Linear frame buffer engine for Mode `0x13`: Direct linear access `0xA000:y*320+x` for `gfx_putpixel()`, word-aligned `gfx_clear()`, and fast assembly `vga_blit_linear` (`kernel/entry.asm`) copying 32,000 words directly into `0xA000:0000`.
    - Headless VSync timeout safeguard: Added bounded loop counter in `_cga_wait_vsync` to eliminate stalls in headless emulators without CRT beam sync.
    - High-level C helpers in `include/hobbyos.h`: Added `os_gfx_line()`, `os_gfx_rect()`, and `os_buf_putpixel_13h()`.
    - EGA/VGA Emulated CGA Palette Swapping (`kernel/gfx.c`, `kernel/entry.asm`, `kernel/kernel.h`): Implemented Attribute Controller palette reprogramming via `bios_set_palette_reg()` (`INT 0x10, AX=0x1000`) when CGA modes run on EGA/VGA adapters (`video_get_type() >= VIDEO_TYPE_EGA`). Emulates CGA Palette 0 (Green/Red/Brown), Palette 1 (Cyan/Magenta/White), and Palette 2 / Mode 5 (Cyan/Red/White) seamlessly on analog VGA hardware where Port `0x3D8`/`0x3D9` writes are ignored by display DACs.
  - **`GFXDEMO.BIN` User Graphics Showcase (`user/gfxdemo.c`, `Makefile`, `user/shell.c`)**:
    - Interactive 5-screen graphics showcase demonstrating CGA and VGA graphics features:
      1. CGA 320x200 4-color palettes (Palettes 0, 1, 2) and geometric primitives.
      2. CGA 640x200 2-color high-res mathematical curves, moiré lines, and borders.
      3. Double-buffered 3D rotating wireframe cube with perspective projection, fixed-point integer sine/cosine table (`sin_table`), and hardware `imul` via `fx_mul()`.
      4. VGA Mode `0x12` (640x480 16-color) planar grid, diamond patterns, and 16 color test bars.
      5. VGA Mode `0x13` (320x200 256-color) custom DAC gradient palette and concentric color rings.
    - Supports automated test mode (`gfxdemo /t` or non-interactive key check) with brief delays.
    - Added `gfxdemo` to shell `help` command list.

## Revision 0.14 (2026-11-17)

- **Hard Disk Partitioning Subsystem, Syscall API & `FDISK.BIN` (`boot/mbr.asm`, `user/fdisk.c`, `kernel/fat16.c`, `kernel/syscall.c`, `kernel/kernel.h`, `include/hobbyos.h`, `Makefile`, `user/format.c`)**:
  - **Partition Management Syscalls (`include/hobbyos.h`, `kernel/syscall.c`, `kernel/fat16.c`)**:
    - Added dedicated syscalls for querying drive geometry and reading/writing partition tables:
      - `SYS_DISK_GET_GEOM` (`0x23`, `os_disk_get_geom(drive, &geom)`): Queries physical drive geometry (cylinders, heads, sectors per track, total LBA sectors) via BIOS `INT 0x13, AH=0x08`.
      - `SYS_PART_READ` (`0x24`, `os_part_read(drive, &table)`): Reads LBA 0 (MBR) and unpacks the 4 partition table entries and boot signature.
      - `SYS_PART_WRITE` (`0x25`, `os_part_write(drive, &table)`): Writes the updated partition table to LBA 0 (preserving or installing MBR boot code) and triggers an in-kernel storage volume rescan (`fat16_rescan_drives()`).
    - Added structures `os_disk_geom_t`, `os_partition_t`, and `os_part_table_t`.
  - **Dynamic In-Kernel Drive Rescanning (`kernel/fat16.c`, `kernel/kernel.h`)**:
    - Added `fat16_rescan_drives()`: Flushes cached FAT sectors, resets volume tables, re-probes floppy drives and hard disks, and restores default aliases (`HD0A:` -> `HD0:`, `SYS:` -> boot volume).
    - Fixed partition enumeration bug in `probe_drive()`: Snapshots MBR partition table to a local array `parts_copy[4]` before probing partition VBRs, preventing subsequent partitions (`HD1B:`, `HD1C:`, etc.) from being lost when `io_buf` is overwritten.
  - **Standard 8086 Master Boot Record (`boot/mbr.asm`, `Makefile`)**:
    - Implemented standard 8086 MBR bootblock (`boot/mbr.asm`, 446 bytes code + 64 bytes partition table + 2 bytes `0xAA55` signature).
    - Relocates itself from `0000:7C00` to `0000:0600`, verifies partition table validity, searches for the active partition (`boot_indicator == 0x80`), loads the partition's VBR into `0000:7C00` with up to 5 retries, and jumps to `0000:7C00` passing `DL = boot_drive` and `DS:SI = partition table entry`.
    - Integrated into build system: compiled with `nasm -f bin` to `build/mbr.bin`, converted by `tools/bin2c.py` into `build/mbrcode.h` (`default_mbr_code[446]`) for embedding into `FDISK.BIN`.
  - **`FDISK.BIN` Interactive & CLI Partitioning Utility (`user/fdisk.c`)**:
    - Complete partition management utility written in freestanding 8086 C.
    - Interactive menu:
      1. Create primary DOS partition (custom size in MB or maximum available).
      2. Set active partition.
      3. Delete partition.
      4. Display partition information.
      5. Switch fixed disk drive.
      Q. Exit.
    - CLI batch mode for scripting and testing:
      - `fdisk /status`: Displays partition tables for all detected fixed disks.
      - `fdisk <drive> /create:size|MAX`: Automatically creates partition aligned to cylinder boundaries.
      - `fdisk <drive> /activate:n`: Marks partition `n` (1-4) as active bootable.
      - `fdisk <drive> /delete:n`: Deletes partition `n`.
      - `fdisk <drive> /mbr`: Installs or updates standard hobbyOS 8086 MBR boot code without touching partitions.
    - Freestanding 8086 arithmetic: Inline hardware `div bx` pragmas (`div_dx_ax`, `rem_dx_ax`), shift/add multiplication (`(val << 3) + (val << 1)`), avoiding Watcom `-zl` 32-bit math helpers (`__U4D`, `__U4M`).
  - **Volume Format Name Matching Fix (`user/format.c`)**:
    - Replaced 3-character prefix comparison with case-insensitive full name matching `str_icmp()`, preventing secondary partition status reporting confusion (e.g. `HD1B:` matching `HD1A:`).

## Revision 0.13 (2026-11-17)

- **FAT16B / BigDOS Partition Formatting & 32-Bit Volume Reporting (`kernel/fat16.c`, `user/format.c`, `user/shell.c`)**:
  - **FAT16B Hard Disk Partition Formatting (`kernel/fat16.c`)**:
    - Extended `fat16_format_volume()` to format hard disk partitions larger than 32 MB (up to 2 GB) with the FAT16B / BigDOS standard.
    - Inspects MBR partition table (LBA 0) for partition devices (`HD0A:`, `HD1A:`, etc.) to obtain the true sector count from the partition entry, preventing volumes previously formatted or clamped to 32 MB from being re-clamped.
    - Automatically updates partition type in the MBR partition table from `0x04` (FAT16 < 32MB) to `0x06` (FAT16B > 32MB) when formatting partitions exceeding 65,535 sectors.
    - Automatic MBR Active Partition Activation: Inspects the MBR partition table at LBA 0; if no partition is marked active (`0x80`), marks the partition being formatted as active (`boot_indicator = 0x80`) so the MBR code chains into the partition VBR upon reboot instead of halting with `INT 0x18`.
    - Dynamic cluster geometry scaling: Automatically scales `spc` (sectors per cluster: 4, 8, 16, 32, 64) using power-of-two bit shifts without 32-bit C division routines, keeping total data clusters `<= 65520` to prevent 16-bit cluster integer overflow.
    - Correctly writes `0` into `BPB_TotSec16` (offset 19) and total sectors into `BPB_TotSec32` (offset 32) when formatting volumes > 65,535 sectors, and vice-versa for volumes <= 32 MB.
  - **32-Bit Volume Capacity Reporting & Free Space (`user/format.c`, `user/shell.c`)**:
    - Implemented freestanding 8086-safe 32-bit decimal printer (`u32_div10` and `print_uint32()`) avoiding Watcom `-zl` `__U4D` library symbols.
    - Upgraded `clusters_to_kb` in `user/shell.c` and size calculation in `user/format.c` to `uint32_t`, eliminating 16-bit integer overflow when calculating capacity and free space on partitions > 64 MB.
    - Verified in 86Box: 504 MB partition formats cleanly and displays full `515792 KB` total and available space across both `format` and `lsdisk`.

## Revision 0.12 (2026-11-17)

- **Multi-Density Floppy Formatting Engine & AT/286+ Architecture Fixes (`kernel/fat16.c`, `kernel/entry.asm`, `kernel/syscall.c`, `kernel/kernel.h`, `include/hobbyos.h`, `user/format.c`)**:
  - **AT/286+ Floppy Controller Rate & DPT Management (`kernel/entry.asm`, `kernel/fat16.c`)**:
    - Added `bios_disk_set_media_type(drive, cyls, spt)` calling `INT 0x13, AH=0x18` ("Set Media Type for Format").
    - Configures AT FDC controller data transfer rate (500 kbps for 1.44M/1.2M, 250 kbps for 720K/360K) and repoints Diskette Parameter Table (`INT 0x1E`) to correct sector count before formatting/writing.
    - Graceful fallback on PC/XT BIOS where `AH=0x18` is unsupported.
  - **Low-Level Track Formatting Engine (`kernel/entry.asm`, `kernel/fat16.c`)**:
    - Added `bios_disk_format_track(drive, cyl, head, table)` calling `INT 0x13, AH=0x05`.
    - Formats all cylinders and heads with 4-byte address fields (C, H, S, N) to lay down physical sector headers.
    - Allows formatting completely raw/unformatted diskettes as well as reformatting media between geometries (e.g. 720K in 1.44M drive).
    - Auto-skips track formatting on hard disk partitions and when `/Q` (quick format) is passed.
  - **Universal Floppy Geometry Support (`kernel/fat16.c`)**:
    - Full support for 4 standard PC floppy geometries:
      - **1.44 MB 3.5" HD**: 80 cyls, 2 heads, 18 spt, 2880 sectors, 1 spc, 224 root entries, 9 spf, media `0xF0`.
      - **720 KB 3.5" DD**: 80 cyls, 2 heads, 9 spt, 1440 sectors, 2 spc, 112 root entries, 3 spf, media `0xF9`.
      - **1.2 MB 5.25" HD**: 80 cyls, 2 heads, 15 spt, 2400 sectors, 1 spc, 224 root entries, 7 spf, media `0xF9`.
      - **360 KB 5.25" DD**: 40 cyls, 2 heads, 9 spt, 720 sectors, 2 spc, 112 root entries, 2 spf, media `0xFD`.
    - In auto-detection mode (`FORMAT_TYPE_AUTO`), inspects existing media VBR first to preserve inserted disk geometry (preventing 720K disks in 1.44M drives from being corrupted as 1.44M), then falls back to BIOS drive parameters or 720K fallback.
  - **User-Space Geometry & Quick Format Flags (`user/format.c`, `include/hobbyos.h`, `kernel/syscall.c`)**:
    - Added `os_vol_format_ex(dev, label, flags)` passing geometry types and quick flag in register `CX`.
    - `FORMAT.BIN` parses `/F:1440`, `/F:720`, `/F:1200`, `/F:360` (also `/1440`, `/720`, `/1200`, `/360`) and `/Q`.
    - Displays selected format details during formatting (e.g. `(720 KB DD, 9 spt)`).

## Revision 0.11 (2026-11-17)

- **Disk & Partition Formatting Utility `FORMAT.BIN` with System Bootblock Installation (`user/format.c`, `kernel/fat16.c`, `kernel/syscall.c`, `kernel/console.c`, `boot/boot.asm`, `include/hobbyos.h`, `kernel/kernel.h`, `Makefile`)**:
  - **`FORMAT.BIN` User Utility (`user/format.c`)**:
    - Full-featured disk formatting tool supporting floppy drives (`FD0:`, `FD1:`) and hard disk partitions (`HD0A:`, `HD1A:`).
    - Syntax: `FORMAT <DEV:> [/S] [/V:label] [/Y]`.
    - Safety confirmation prompt before destructive write (`WARNING: ALL DATA ON DISK DRIVE %s WILL BE LOST! Proceed with Format (Y/N)?`), with `/Y` switch for non-interactive scripting/testing.
    - System transfer (`/S`): Installs bootloader, copies `/KERNEL.BIN` to root, creates `/BIN`, and copies `/BIN/SHELL.BIN` to `/BIN/SHELL.BIN`.
    - Real-time format statistics: Reports total capacity and available free space in kilobytes.
  - **Kernel Formatting Engine & Syscall `SYS_VOL_FORMAT` (`0x1F` / `os_vol_format()`)**:
    - Low-level formatting engine in `kernel/fat16.c` (`fat16_format_volume`):
      - Floppy auto-geometry discovery (defaults 720KB 3.5" 2DD, 80 tracks, 2 heads, 9 spt for PC/XT, detects 1.44MB on AT BIOS).
      - Hard disk partition formatting using start LBA and sector count from MBR.
      - Constructs Sector 0 (VBR) with valid BPB, OEM `"HOBBYOS"`, volume label, and `0x55AA` boot signature.
      - Clears FAT1 and FAT2 tables with proper media descriptors (`0xF9` for 720K, `0xF0` for 1.44M, `0xF8` for hard disks).
      - Clears root directory sectors and writes optional volume label directory entry (`FAT_ATTR_VOLUME_ID`).
      - In-memory state refresh: Invalidate FAT cache, re-parses BPB, resets CWD to `/`, and marks volume mounted.
  - **Universal Dual FAT12/FAT16 Bootloader (`boot/boot.asm`)**:
    - Updated HobbyOS bootloader sector to support booting from both FAT12 (floppies) and FAT16 (hard drives).
    - Dynamic cluster lookup: Detects filesystem type from BPB and switches to 12-bit packed cluster lookup (with boundary-crossing sector pair buffer) or FAT16 cluster lookup.
    - 3-retry spin-up loop with controller reset on `INT 0x13` read failure for robust floppy booting.
    - Fits within Sector 0 (512 bytes with `0x55AA` signature).
    - Embedded into kernel build as `build/bootcode.h` and deployed to `/BIN/BOOT.BIN`.
  - **Optimized Streaming File Copy (`kernel/fat16.c`)**:
    - Boosted `fat16_copy_file` with a 2 KB streaming buffer (`copy_buf`), copying entire clusters per batch rather than 512-byte increments, reducing floppy disk head thrashing and speeding up transfer by over 400%.
  - **Serial Console Mirroring to COM1 (`kernel/console.c`)**:
    - All console text output (`putc_no_cursor`) is mirrored to COM1 (9600 8N1) alongside screen memory, enabling full terminal session logging under 86Box headless execution.
  - **Shell Integration (`user/shell.c`)**:
    - `lsdisk` reports unformatted hard disk partitions cleanly as `[Unformatted]`.
    - Added `format` to `help` command list.

- **FAT12 Filesystem Driver & Removable Media Lazy Mount Architecture (`kernel/fat16.c`, `kernel/entry.asm`, `kernel/kernel.h`, `kernel/syscall.c`, `include/hobbyos.h`, `user/shell.c`)**:
  - **Floppy Drive Detection from BDA**:
    - Discovers physical floppy drives at boot via BIOS Data Area Equipment Word (`0x0040:0x0010` bit 0 & bits 6–7).
    - Registers `FD0:` (BIOS 0x00) and `FD1:` (BIOS 0x01) without spinning up spindle motors at boot, keeping boot time instantaneous.
  - **Lazy Mount & Geometry Auto-Discovery**:
    - Mounts removable drives on demand upon first filesystem access.
    - Reads Sector 0 (CHS 0,0,1) with floppy controller reset (`INT 0x13, AH=0x00`) and 3 retries for motor spin-up.
    - Dynamically configures `sectors_per_track` and `num_heads` straight from the BPB (supporting 360K, 720K, 1.2M, and 1.44M floppy formats).
    - If empty or door open, returns clean `OS_ERR_NO_MEDIA` (`-3`) without hang, displaying `Drive not ready / No disk in drive`.
  - **FAT12 Cluster Chain Engine**:
    - Full 12-bit packed cluster lookup and allocation in `kernel/fat16.c` (`FAT_TYPE_FAT12`).
    - Handles 511-byte FAT sector boundaries when a 12-bit entry spans two sectors.
    - End-of-chain (`>= 0x0FF8`) and free cluster accounting seamlessly unified with FAT16.
  - **AmigaDOS-Style `diskchange` Command & Removable State Management**:
    - Syscall `SYS_DISKCHANGE` (`0x1E` / `os_diskchange()`).
    - `diskchange [dev:]` shell command to unmount removable drives and signal media swap.
    - Auto-unmounts removable volumes upon I/O errors to prevent stale cache corruption.
    - `lsdisk` displays floppy status: `[No disk inserted]` when unmounted, or formatted capacity and free space `(FAT12)` when mounted.

## Revision 0.9 (2026-11-17)

- **Wildcard Pattern Support in User-Space File Management Commands (`user/shell.c`)**:
  - Implemented iterative, non-recursive classic wildcard pattern matcher (`wildcard_match`) for `*` and `?` patterns:
    - Case-insensitive ASCII matching.
    - Handles standard wildcard conventions (`*`, `?`, `*.*` matching all files including extensionless entries).
    - Zero stack recursion, strictly safe for Intel 8088 real-mode stack limits.
  - **`ls` / `dir`**: Supports path patterns (e.g. `ls *.BIN`, `ls /BIN/*.BIN`, `ls SYS:KEYMAPS/*.MAP`) to filter directory listings while preserving total and free storage cluster reporting.
  - **`cp`**: Supports source wildcards (e.g. `cp /KEYMAPS/*.MAP HD1A:`, `cp *.TXT DOCS/`) to batch copy all matching files into destination directory or volume root, preserving original filenames.
  - **`rm`**: Supports wildcard deletion (e.g. `rm *.BAK`, `rm /TEST/*.MAP`) with batch deletion counter, properly handling FAT16 in-place directory entry traversal when entries are deleted.
  - Enhanced path helpers (`path_basename`, `split_path_pattern`, `combine_dir_file`) supporting AmigaDOS device prefixes (e.g. `SYS:`, `HD0A:`) alongside directory paths.

## Revision 0.8 (2026-11-17)

- **AmigaDOS-Style Volume Architecture & Dynamic Storage Detection (`kernel/fat16.c`, `kernel/entry.asm`, `kernel/kernel.h`, `kernel/syscall.c`, `kernel/main.c`, `include/hobbyos.h`, `user/shell.c`, `user/hello.c`, `boot/boot.asm`)**:
  - Implemented dynamic drive detection and AmigaDOS-style device naming:
    - Floppy drives: `FD0:`, `FD1:` (BIOS drives 0x00, 0x01).
    - Hard drives with MBR partitions: `HD0A:`–`HD0D:`, `HD1A:`–`HD1D:` (BIOS drives 0x80, 0x81; MBR partitions 0–3).
    - Raw whole-drive / superfloppy volumes: `HD0:`, `HD1:`.
    - Logical boot alias: **`SYS:`** pointing automatically to the boot volume.
  - Path parsing supports both AmigaDOS conventions (`DEV:PATH` and `DEV:/PATH`), defaulting to the active volume when the device prefix is omitted.
  - Per-volume current working directory tracking; switching devices with `cd DEV:` or entering `DEV:` alone at the shell prompt switches the active device.
  - BIOS dynamic geometry discovery using pure 8086 `INT 0x13, AH=0x08` (extracting cylinders, heads, spt without 186+ instructions).
  - Pure 8086 assembly 32-bit LBA disk read/write translation (`_bios_disk_rw_lba` in `kernel/entry.asm`) utilizing hardware `div bx` to eliminate runtime 32-bit math helpers (`__U4D`, `__I4D`, `__U4M`).
  - Syscall `SYS_VOL_INFO` (`0x1C` / `os_vol_info()`) and `os_vol_info_t` struct returning volume name, BIOS drive, partition index, alias target, total clusters/KB, and free clusters/KB.
  - Added `lsdisk` shell command listing discovered drives, partitions, aliases, sizes, and free space.
  - `ls` / `dir` displays volume total clusters/KB and free clusters/KB summary line.
  - Accelerated `vol_get_free_clusters` with sequential sector-level FAT scanning (100x speedup on 4.77 MHz 8088 hardware).
  - Verified in `user/hello.c` with cross-device path reads (`SYS:DOCS/NOTES.TXT` and `HD0A:DOCS/NOTES.TXT`).

## Revision 0.7 (2026-11-17)

- **Video Controller Detection Hierarchy & MDA Fallback Subsystem (`kernel/console.c`, `kernel/entry.asm`, `kernel/gfx.c`, `kernel/syscall.c`, `kernel/main.c`, `include/hobbyos.h`, `user/mem.c`, `user/hello.c`)**:
  - Implemented real-mode video controller probe hierarchy distinguishing VGA, EGA, CGA, and MDA:
    - **VGA Check**: Calls BIOS `INT 0x10, AX=0x1A00` (Display Combination Code). If `AL == 0x1A` and active display `BL` is valid (`0x07` or `0x08`), identified as VGA (supports Book8088 V2 with Trident VGA as well as modern VGA clones).
    - **EGA Check**: Calls BIOS `INT 0x10, AH=0x12, BL=0x10` (Get EGA Configuration). If `BL != 0x10` and `BL <= 3`, identified as EGA.
    - **CGA vs. MDA Determination**:
      - Inspects BIOS video mode via `INT 0x10, AH=0x0F` (Mode 7 is MDA monochrome text).
      - Checks BIOS Data Area Equipment Word (`0x0040:0x0010` bits 4–5: `11b` = monochrome 80x25 at `0xB000`, `10b`/`01b` = CGA 80x25 / 40x25 at `0xB800`).
      - Confirms via BDA CRTC base I/O port address (`0x0040:0x0063`: `0x3B4` for MDA, `0x3D4` for CGA).
  - **Dynamic Video Subsystem & Zero-Snow Direct Copy**:
    - Console and low-level routines use dynamic video parameters: `_video_vram_seg` (`0xB800` for CGA/EGA/VGA, `0xB000` for MDA), `_video_status_port` (`0x3DA` for CGA, `0x3BA` for MDA), and `_video_has_snow` (1 for discrete CGA, 0 for VGA/EGA/MDA).
    - `_cga_sync_copy` dynamically checks `[_video_has_snow]`. On VGA, EGA, and MDA cards where video memory is dual-ported or snow-free, screen redraw bypasses retrace polling loops and executes a fast `cld; rep movsw`, dramatically speeding up full-screen updates.
    - Hardware cursor manipulation (`update_hw_cursor`) dynamically addresses `0x3B4`/`0x3B5` on MDA or `0x3D4`/`0x3D5` on CGA/EGA/VGA.
  - **MDA Monochrome Attribute Translation**:
    - Automatically maps 16-color CGA text attributes to MDA hardware attributes (0x07 normal white on black, 0x0F bright white on black, 0x01 underline, 0x70 reverse video black on white, and preserve blinking bit 7) to ensure clean readability on monochrome monitors.
  - **Graphics Guarding on MDA**:
    - Guarded `gfx_set_mode` (`SYS_GFX_SET_MODE`) to reject CGA color graphics modes (0x04, 0x05, 0x06) when running on MDA hardware, returning `-1` error.
    - Process exit restoration safely checks and restores Mode 7 text if running under MDA.
  - **Public API & Diagnostics**:
    - Added `SYS_VIDEO_TYPE` (`0x0C` / `os_video_type()`), constants `VIDEO_TYPE_UNKNOWN` (0), `VIDEO_TYPE_MDA` (1), `VIDEO_TYPE_CGA` (2), `VIDEO_TYPE_EGA` (3), `VIDEO_TYPE_VGA` (4), and helper `os_video_name()` in `include/hobbyos.h`.
    - Boot banner displays detected video adapter model and snow-suppression status (`[VIDEO] Detected: IBM CGA (Color 80x25, 0xB800) (snow prevention active)` or `[VIDEO] Detected: IBM MDA (Monochrome 80x25, 0xB000) (monochrome text at 0xB000)`).
    - Added `Video Adapter:` display to `/BIN/MEM.BIN` and self-test verification to `/BIN/HELLO.BIN`.

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
