//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_fs_utime on FAT and exFAT (RA8FW-737), against the walker fakes.

const std = @import("std");
const fs = @import("ra8_fs");
const ut = fs.utime;
const fake = @import("fs_walker_fake.zig");

const fat32: u8 = fs.c.k_ra8_fs_type_fat32;
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

fn foldRef(cs: u16, b: u8) u16 {
    const hi: u16 = if ((cs & 1) != 0) 0x8000 else 0;
    return hi +% (cs >> 1) +% @as(u16, b);
}

fn when(minute: u8) ut.DateTime {
    var t = std.mem.zeroes(ut.DateTime);
    t.year = 2026;
    t.month = 10;
    t.day = 5;
    t.minute = minute;
    return t;
}

fn utime(path: ?[*:0]const u8, create: ?*const ut.DateTime, modify: ?*const ut.DateTime) u16 {
    return ut.ra8_fs_utime(fake.mount(), path, create, modify, null);
}

test "argument checks: null, unmounted, volume root" {
    fake.reset(fat32);
    const t = when(1);
    try std.testing.expectEqual(ut.err_null_ptr, ut.ra8_fs_utime(null, "/A.TXT", &t, null, null));
    try std.testing.expectEqual(ut.err_null_ptr, utime(null, &t, null));
    try std.testing.expectEqual(ut.err_invalid_arg, utime("/", &t, null));
    fake.mount_store.in_use = 0;
    try std.testing.expectEqual(ut.err_invalid_state, utime("/A.TXT", &t, null));
}

test "FAT stamps the 8.3 entry in its sector and passes the stamps through" {
    fake.reset(fat32);
    // the resolver walks "/DIR": the walk fake serves one buffer for every
    // component, so both the "DIR" parent and the "A.TXT" leaf live in it
    // DIR re-points at the root cluster so both component reads land on
    // the one served buffer (walk_lba), as the comment below documents.
    fake.lay83(0, "DIR\x20\x20\x20\x20\x20\x20\x20\x20", @intCast(fs.c.k_ra8_fs_attr_directory), fake.mount_store.root_cluster);
    fake.lay83(1, "A\x20\x20\x20\x20\x20\x20\x20TXT", 0, 0);
    const cr = when(3);
    const md = when(42);
    try std.testing.expectEqual(ut.ok, utime("/DIR/A.TXT", &cr, &md));
    try std.testing.expectEqual(fake.walk_lba, fake.sector_written.?);
    // the real stamper packed the readings: minute 3 create, minute 42 write
    try std.testing.expectEqual(@as(u16, 3 << 5), std.mem.readInt(u16, fake.sector[46..48], .little));
    try std.testing.expectEqual(@as(u16, 42 << 5), std.mem.readInt(u16, fake.sector[54..56], .little));
}

test "FAT falls back to the long name" {
    fake.reset(fat32);
    // "a long name" never packs to 8.3, so utime goes straight to the VFAT chain
    fake.layLongName("a long name.txt", "ALONGN~1TXT", 0);
    const md = when(9);
    try std.testing.expectEqual(ut.ok, utime("/a long name.txt", null, &md));
    try std.testing.expectEqual(@as(u16, 9 << 5), std.mem.readInt(u16, fake.sector[86..88], .little));
    try std.testing.expectEqual(@as(?u64, fake.walk_lba), fake.sector_written);
}

test "FAT errors propagate without a write" {
    fake.reset(fat32);
    try std.testing.expectEqual(ut.err_not_found, utime("/A.TXT", null, null));
    fake.reset(fat32);
    fake.sector_read_err = 0x204;
    try std.testing.expectEqual(@as(u16, 0x204), utime("/A.TXT", null, null));
    try std.testing.expectEqual(@as(?u64, null), fake.sector_written);
}

test "exFAT stamps the File entry and refreshes the checksum" {
    fake.reset(exfat);
    stageSet("book.epub", 0x20, 0, 0);
    const md = when(17);
    try std.testing.expectEqual(ut.ok, utime("/book.epub", null, &md));
    // the real stamper packed minute 17 and the whole-minute UTC byte
    try std.testing.expectEqual(@as(u32, (@as(u32, 0x5D45) << 16) | (17 << 5)), std.mem.readInt(u32, fake.dir[2][12..16], .little));
    try std.testing.expectEqual(@as(u8, 0x80), fake.dir[2][23]);
    // the checksum refresh folded the post-stamp bytes, checksum field skipped
    var probe: [96]u8 = undefined;
    @memcpy(&probe, @as([*]const u8, @ptrCast(&fake.dir[2]))[0..96]);
    std.mem.writeInt(u16, probe[ut.off_csum..][0..2], 0, .little);
    const csum = fs.c.priv_exfat_set_checksum(@ptrCast(&probe), 96);
    try std.testing.expectEqual(csum, std.mem.readInt(u16, fake.dir[2][ut.off_csum..][0..2], .little));
    try std.testing.expectEqual(@as(u8, 0xC0), fake.dir[3][0]);
}

test "exFAT errors propagate without a write" {
    fake.reset(exfat);
    fake.find_set_err = ut.err_not_found;
    try std.testing.expectEqual(ut.err_not_found, utime("/nope", null, null));
    fake.reset(exfat);
    fake.sector_read_err = 0x205;
    try std.testing.expectEqual(@as(u16, 0x205), utime("/book.epub", null, null));
    try std.testing.expectEqual(@as(?u32, null), fake.written_index);
}
