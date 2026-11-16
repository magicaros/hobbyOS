#!/usr/bin/env python3
# Copyright (C) 2026 Benoit Colson
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
"""
FAT16 disk image utility for hobbyOS.
Installs the FAT16 VBR boot code (preserving/validating the BPB) and copies
files into the root directory or subdirectories of a FAT16 disk image.
"""

import os
import struct
import sys
import time

ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE   = 0x20


def format_83_name(filename: str) -> bytes:
    base = filename.upper()
    if base == ".":
        return b".          "
    if base == "..":
        return b"..         "
    if "." in base:
        name, ext = base.rsplit(".", 1)
    else:
        name, ext = base, ""
    if len(name) > 8 or len(ext) > 3 or not name:
        raise ValueError(f"Filename '{base}' does not fit 8.3 format")
    return f"{name:<8}{ext:<3}".encode("ascii")


def dos_datetime():
    t = time.localtime()
    dos_time = ((t.tm_hour & 0x1F) << 11) | ((t.tm_min & 0x3F) << 5) | ((t.tm_sec // 2) & 0x1F)
    year = max(1980, t.tm_year) - 1980
    dos_date = ((year & 0x7F) << 9) | ((t.tm_mon & 0x0F) << 5) | (t.tm_mday & 0x1F)
    return dos_time, dos_date


def install_boot_sector(img: bytearray, boot_bin: bytes) -> None:
    if len(boot_bin) != 512:
        raise ValueError(f"Boot sector must be 512 bytes, got {len(boot_bin)}")
    if boot_bin[510:512] != b"\x55\xaa":
        raise ValueError("Boot sector missing 0xAA55 signature")
    img[0:3] = boot_bin[0:3]
    img[62:512] = boot_bin[62:512]


def parse_bpb(img: bytearray) -> dict:
    (
        bytes_per_sector,
        sectors_per_cluster,
        reserved_sectors,
        num_fats,
        root_entries,
        total_sectors16,
        media,
        sectors_per_fat,
        sectors_per_track,
        num_heads,
        hidden_sectors,
        total_sectors32,
    ) = struct.unpack_from("<HBHBHHBHHHII", img, 11)
    total_sectors = total_sectors16 if total_sectors16 != 0 else total_sectors32
    fat_lba = reserved_sectors
    root_lba = fat_lba + num_fats * sectors_per_fat
    root_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector
    data_lba = root_lba + root_sectors
    total_clusters = (total_sectors - data_lba) // sectors_per_cluster
    return {
        "bytes_per_sector": bytes_per_sector,
        "sectors_per_cluster": sectors_per_cluster,
        "reserved_sectors": reserved_sectors,
        "num_fats": num_fats,
        "root_entries": root_entries,
        "total_sectors": total_sectors,
        "sectors_per_fat": sectors_per_fat,
        "fat_lba": fat_lba,
        "root_lba": root_lba,
        "root_sectors": root_sectors,
        "data_lba": data_lba,
        "total_clusters": total_clusters,
    }


def read_fat_entry(img: bytearray, bpb: dict, cluster: int) -> int:
    fat_offset = bpb["fat_lba"] * bpb["bytes_per_sector"] + cluster * 2
    return struct.unpack_from("<H", img, fat_offset)[0]


def write_fat_entry(img: bytearray, bpb: dict, cluster: int, value: int) -> None:
    for fat_idx in range(bpb["num_fats"]):
        fat_start = (bpb["fat_lba"] + fat_idx * bpb["sectors_per_fat"]) * bpb["bytes_per_sector"]
        struct.pack_into("<H", img, fat_start + cluster * 2, value)


def allocate_clusters(img: bytearray, bpb: dict, count: int) -> list[int]:
    allocated = []
    for c in range(2, bpb["total_clusters"] + 2):
        if len(allocated) == count:
            break
        if read_fat_entry(img, bpb, c) == 0x0000:
            allocated.append(c)
    if len(allocated) < count:
        raise RuntimeError("Not enough free clusters in FAT16 image")
    for i, c in enumerate(allocated):
        next_val = allocated[i + 1] if i + 1 < len(allocated) else 0xFFFF
        write_fat_entry(img, bpb, c, next_val)
    return allocated


def cluster_byte_offset(bpb: dict, cluster: int) -> int:
    lba = bpb["data_lba"] + (cluster - 2) * bpb["sectors_per_cluster"]
    return lba * bpb["bytes_per_sector"]


def pack_dirent(name83: bytes, attr: int, first_cluster: int, size: int) -> bytes:
    dos_time, dos_date = dos_datetime()
    return struct.pack(
        "<11sBBBHHHHHHHI",
        name83,
        attr,
        0,
        0,
        dos_time,
        dos_date,
        dos_date,
        0,
        dos_time,
        dos_date,
        first_cluster,
        size,
    )


def get_dir_entry_offsets(img: bytearray, bpb: dict, dir_cluster: int) -> list[int]:
    offsets = []
    if dir_cluster == 0:
        root_start = bpb["root_lba"] * bpb["bytes_per_sector"]
        for i in range(bpb["root_entries"]):
            offsets.append(root_start + i * 32)
    else:
        cluster_size = bpb["bytes_per_sector"] * bpb["sectors_per_cluster"]
        entries_per_cluster = cluster_size // 32
        cur = dir_cluster
        while 2 <= cur < 0xFFF8:
            base = cluster_byte_offset(bpb, cur)
            for i in range(entries_per_cluster):
                offsets.append(base + i * 32)
            cur = read_fat_entry(img, bpb, cur)
    return offsets


def find_or_create_subdir(img: bytearray, bpb: dict, parent_cluster: int, dirname: str) -> int:
    name83 = format_83_name(dirname)
    offsets = get_dir_entry_offsets(img, bpb, parent_cluster)

    for off in offsets:
        if img[off] == 0x00:
            break
        if img[off] == 0xE5:
            continue
        if img[off : off + 11] == name83 and (img[off + 11] & ATTR_DIRECTORY):
            return struct.unpack_from("<H", img, off + 26)[0]

    new_cluster = allocate_clusters(img, bpb, 1)[0]
    cluster_size = bpb["bytes_per_sector"] * bpb["sectors_per_cluster"]
    c_off = cluster_byte_offset(img_bpb := bpb, new_cluster)
    img[c_off : c_off + cluster_size] = b"\x00" * cluster_size

    # Write '.' and '..' entries into the new directory cluster
    img[c_off : c_off + 32] = pack_dirent(b".          ", ATTR_DIRECTORY, new_cluster, 0)
    img[c_off + 32 : c_off + 64] = pack_dirent(b"..         ", ATTR_DIRECTORY, parent_cluster, 0)

    for off in offsets:
        if img[off] in (0x00, 0xE5):
            img[off : off + 32] = pack_dirent(name83, ATTR_DIRECTORY, new_cluster, 0)
            return new_cluster

    raise RuntimeError(f"Directory full when creating '{dirname}'")


def add_file(img: bytearray, bpb: dict, host_path: str, target_path: str | None = None) -> None:
    with open(host_path, "rb") as f:
        data = f.read()

    rel_path = (target_path or os.path.basename(host_path)).replace("\\", "/").strip("/")
    parts = [p for p in rel_path.split("/") if p]
    dir_parts, filename = parts[:-1], parts[-1]

    dir_cluster = 0
    for d in dir_parts:
        dir_cluster = find_or_create_subdir(img, bpb, dir_cluster, d)

    name83 = format_83_name(filename)
    cluster_size = bpb["bytes_per_sector"] * bpb["sectors_per_cluster"]
    num_clusters = max(1, (len(data) + cluster_size - 1) // cluster_size) if len(data) > 0 else 0
    clusters = allocate_clusters(img, bpb, num_clusters) if num_clusters > 0 else []

    for idx, c in enumerate(clusters):
        chunk = data[idx * cluster_size : (idx + 1) * cluster_size]
        chunk_padded = chunk.ljust(cluster_size, b"\x00")
        byte_offset = cluster_byte_offset(bpb, c)
        img[byte_offset : byte_offset + cluster_size] = chunk_padded

    first_cluster = clusters[0] if clusters else 0
    for off in get_dir_entry_offsets(img, bpb, dir_cluster):
        if img[off] in (0x00, 0xE5) or img[off : off + 11] == name83:
            img[off : off + 32] = pack_dirent(name83, ATTR_ARCHIVE, first_cluster, len(data))
            return

    raise RuntimeError(f"Directory full when adding '{rel_path}'")


def main() -> None:
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <disk.img> <boot.bin> [file[:TARGET_PATH] ...]", file=sys.stderr)
        sys.exit(1)

    img_path = sys.argv[1]
    boot_path = sys.argv[2]
    file_specs = sys.argv[3:]

    with open(img_path, "rb") as f:
        img = bytearray(f.read())
    with open(boot_path, "rb") as f:
        boot_bin = f.read()

    install_boot_sector(img, boot_bin)
    bpb = parse_bpb(img)

    for spec in file_specs:
        if ":" in spec:
            host_path, target_name = spec.split(":", 1)
        else:
            host_path, target_name = spec, None
        add_file(img, bpb, host_path, target_name)

    with open(img_path, "wb") as f:
        f.write(img)


if __name__ == "__main__":
    main()
