//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_fs_set_attr on FAT and exFAT (RA8FW-735), against the walker fakes.

const std = @import("std");
const fs = @import("ra8_fs");
const attr = fs.attr;
const fake = @import("fs_walker_fake.zig");

const fat16: u8 = fs.c.k_ra8_fs_type_fat16;
const exfat: u8 = fs.c.k_ra8_fs_type_exfat;
const read_only: u8 = fs.c.k_ra8_fs_attr_read_only;
const hidden: u8 = fs.c.k_ra8_fs_attr_hidden;
const archive: u8 = fs.c.k_ra8_fs_attr_archive;

fn setAttr(path: ?[*:0]const u8, set: u8, clear: u8) u16 {
    return attr.ra8_fs_set_attr(fake.mount(), path, set, clear);
}

test "argument checks: null, unmounted, bad masks, volume root" {
    fake.reset(fat16);
    try std.testing.expectEqual(attr.err_null_ptr, attr.ra8_fs_set_attr(null, "/A.TXT", 0, 0));
    try std.testing.expectEqual(attr.err_null_ptr, setAttr(null, 0, 0));
    try std.testing.expectEqual(attr.err_invalid_arg, setAttr("/A.TXT", 0x10, 0)); // directory bit
    try std.testing.expectEqual(attr.err_invalid_arg, setAttr("/A.TXT", 0, 0x08)); // volume-id bit
    try std.testing.expectEqual(attr.err_invalid_arg, setAttr("/A.TXT", hidden, hidden));
    try std.testing.expectEqual(attr.err_invalid_arg, setAttr("///", read_only, 0));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(attr.err_invalid_state, setAttr("/A.TXT", read_only, 0));
    try std.testing.expectEqual(@as(?u64, null), fake.sector_written);
}

test "FAT patches DIR_Attr of the 8.3 entry and writes its sector" {
    fake.reset(fat16);
    fake.sector[64 + 11] = archive;
    try std.testing.expectEqual(attr.ok, setAttr("/A.TXT", read_only | hidden, archive));
    try std.testing.expectEqual(read_only | hidden, fake.sector[64 + 11]);
    try std.testing.expectEqual(@as(?u64, 7), fake.sector_written);
}

test "FAT falls back to the long name when the 8.3 lookup misses" {
    fake.reset(fat16);
    fake.find83_err = attr.err_not_found;
    try std.testing.expectEqual(attr.ok, setAttr("/long name.txt", hidden, 0));
    try std.testing.expectEqual(hidden, fake.sector[96 + 11]);
    try std.testing.expectEqual(@as(?u64, 9), fake.sector_written);
    fake.reset(fat16);
    fake.have83 = 0;
    try std.testing.expectEqual(attr.ok, setAttr("/long name.txt", hidden, 0));
    try std.testing.expectEqual(@as(?u64, 9), fake.sector_written);
}

test "FAT errors propagate without a write" {
    fake.reset(fat16);
    fake.resolve_err = 0x201;
    try std.testing.expectEqual(@as(u16, 0x201), setAttr("/A.TXT", hidden, 0));
    fake.reset(fat16);
    fake.find83_err = attr.err_not_found;
    fake.find_long_err = attr.err_not_found;
    try std.testing.expectEqual(attr.err_not_found, setAttr("/A.TXT", hidden, 0));
    fake.reset(fat16);
    fake.sector_read_err = 0x202;
    try std.testing.expectEqual(@as(u16, 0x202), setAttr("/A.TXT", hidden, 0));
    try std.testing.expectEqual(@as(?u64, null), fake.sector_written);
}

test "exFAT patches FileAttributes and refreshes the set checksum" {
    fake.reset(exfat);
    fake.dir[2][0] = 0x85;
    fake.dir[2][attr.off_attr] = archive;
    fake.dir[3][0] = 0xC0;
    try std.testing.expectEqual(attr.ok, setAttr("/book.epub", read_only, archive));
    try std.testing.expectEqual(@as(?u32, 2), fake.written_index);
    try std.testing.expectEqual(@as(u32, 5), fake.written_cluster);
    try std.testing.expectEqual(read_only, fake.dir[2][attr.off_attr]);
    try std.testing.expectEqual(@as(u32, 64), fake.checksum_bytes);
    const csum = std.mem.readInt(u16, fake.dir[2][attr.off_csum..][0..2], .little);
    try std.testing.expectEqual(@as(u16, 0xA000) | read_only, csum);
    try std.testing.expectEqual(@as(u8, 0xC0), fake.dir[3][0]); // stream entry untouched
}

test "exFAT errors propagate without a write" {
    fake.reset(exfat);
    fake.find_set_err = attr.err_not_found;
    try std.testing.expectEqual(attr.err_not_found, setAttr("/nope", hidden, 0));
    fake.reset(exfat);
    fake.read_error = 0x203;
    try std.testing.expectEqual(@as(u16, 0x203), setAttr("/book.epub", hidden, 0));
    try std.testing.expectEqual(@as(?u32, null), fake.written_index);
}
