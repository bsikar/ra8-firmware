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

/// Stage a real File+Stream+Name set for `name` in root slots 2..4, with the
/// real set checksum stamped over the three entries.
fn stageSet(name: []const u8, file_attr: u8, first: u32, dlen: u64) void {
    fake.dir[2][0] = 0x85;
    fake.dir[2][1] = 2; // Stream + Name
    fake.dir[2][4] = file_attr;
    fake.dir[3][0] = 0xC0;
    fake.dir[3][3] = @intCast(name.len);
    std.mem.writeInt(u64, fake.dir[3][8..16], dlen, .little);
    std.mem.writeInt(u32, fake.dir[3][0x14..0x18], first, .little);
    std.mem.writeInt(u64, fake.dir[3][0x18..0x20], dlen, .little);
    fake.dir[4][0] = 0xC1;
    fake.dir[4][1] = 0;
    for (name, 0..) |ch, i| fake.dir[4][2 + i * 2] = ch;
    fake.dir[4][2 + name.len * 2] = 0xFF;
    fake.dir[4][3 + name.len * 2] = 0xFF;
    _ = fs.c.priv_exfat_set_checksum(@ptrCast(&fake.dir[2]), 96);
}
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
    // the real 8.3 find stops at the first 0x00 slot, so "a.txt" must be the
    // first live entry of the walk buffer
    fake.lay83(0, "A\x20\x20\x20\x20\x20\x20\x20TXT", 0, 0);
    fake.sector[11] = archive;
    try std.testing.expectEqual(attr.ok, setAttr("/A.TXT", read_only | hidden, archive));
    try std.testing.expectEqual(read_only | hidden, fake.sector[11]);
    try std.testing.expectEqual(@as(?u64, fake.walk_lba), fake.sector_written);
}

test "FAT falls back to the long name when the 8.3 lookup misses" {
    fake.reset(fat16);
    // "long name" never packs to 8.3, so the lookup goes straight to the chain
    fake.layLongName("long name.txt", "LONGNA~1TXT", 0);
    try std.testing.expectEqual(attr.ok, setAttr("/long name.txt", hidden, 0));
    try std.testing.expectEqual(archive | hidden, fake.sector[32 + 11]);
    try std.testing.expectEqual(@as(?u64, fake.walk_lba), fake.sector_written);
    fake.reset(fat16);
    fake.layLongName("long name.txt", "LONGNA~1TXT", 0);
    try std.testing.expectEqual(attr.ok, setAttr("/long name.txt", hidden, 0));
    try std.testing.expectEqual(@as(?u64, fake.walk_lba), fake.sector_written);
}

test "FAT errors propagate without a write" {
    fake.reset(fat16);
    fake.sector_read_err = 0x201; // the "d" component lookup's read fails
    try std.testing.expectEqual(@as(u16, 0x201), setAttr("/d/A.TXT", hidden, 0));
    fake.reset(fat16);
    try std.testing.expectEqual(attr.err_not_found, setAttr("/A.TXT", hidden, 0));
    fake.reset(fat16);
    fake.sector_read_err = 0x202;
    try std.testing.expectEqual(@as(u16, 0x202), setAttr("/A.TXT", hidden, 0));
    try std.testing.expectEqual(@as(?u64, null), fake.sector_written);
}

test "exFAT patches FileAttributes and refreshes the set checksum" {
    fake.reset(exfat);
    stageSet("book.epub", archive, 0, 0);
    try std.testing.expectEqual(attr.ok, setAttr("/book.epub", read_only, archive));
    try std.testing.expectEqual(read_only, fake.dir[2][attr.off_attr]);
    // the checksum was refreshed over the real set
    var probe: [96]u8 = undefined;
    @memcpy(&probe, @as([*]const u8, @ptrCast(&fake.dir[2]))[0..96]);
    std.mem.writeInt(u16, probe[attr.off_csum..][0..2], 0, .little);
    const csum = fs.c.priv_exfat_set_checksum(@ptrCast(&probe), 96);
    try std.testing.expectEqual(csum, std.mem.readInt(u16, fake.dir[2][attr.off_csum..][0..2], .little));
    try std.testing.expectEqual(@as(u8, 0xC0), fake.dir[3][0]); // stream entry untouched
}

test "exFAT errors propagate without a write" {
    fake.reset(exfat);
    fake.find_set_err = attr.err_not_found;
    try std.testing.expectEqual(attr.err_not_found, setAttr("/nope", hidden, 0));
    fake.reset(exfat);
    fake.sector_read_err = 0x203;
    try std.testing.expectEqual(@as(u16, 0x203), setAttr("/book.epub", hidden, 0));
    try std.testing.expectEqual(@as(?u32, null), fake.written_index);
}
