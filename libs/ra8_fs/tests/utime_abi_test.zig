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
    try std.testing.expectEqual(@as(u32, 0), fake.stamp_calls);
}

test "FAT stamps the 8.3 entry in its sector and passes the stamps through" {
    fake.reset(fat32);
    const cr = when(3);
    const md = when(42);
    try std.testing.expectEqual(ut.ok, utime("/DIR/A.TXT", &cr, &md));
    try std.testing.expectEqual(@as(u8, 42), fake.sector[64 + 22]);
    try std.testing.expectEqual(@as(?u64, 7), fake.sector_written);
    try std.testing.expectEqual(@as(?*const fs.c.ra8_fs_datetime_t, &cr), fake.stamped[0]);
    try std.testing.expectEqual(@as(?*const fs.c.ra8_fs_datetime_t, null), fake.stamped[2]);
}

test "FAT falls back to the long name" {
    fake.reset(fat32);
    fake.find83_err = ut.err_not_found;
    const md = when(9);
    try std.testing.expectEqual(ut.ok, utime("/a long name.txt", null, &md));
    try std.testing.expectEqual(@as(u8, 9), fake.sector[96 + 22]);
    try std.testing.expectEqual(@as(?u64, 9), fake.sector_written);
}

test "FAT errors propagate without a write" {
    fake.reset(fat32);
    fake.find83_err = ut.err_not_found;
    fake.find_long_err = ut.err_not_found;
    try std.testing.expectEqual(ut.err_not_found, utime("/A.TXT", null, null));
    fake.reset(fat32);
    fake.sector_read_err = 0x204;
    try std.testing.expectEqual(@as(u16, 0x204), utime("/A.TXT", null, null));
    try std.testing.expectEqual(@as(?u64, null), fake.sector_written);
    try std.testing.expectEqual(@as(u32, 0), fake.stamp_calls);
}

test "exFAT stamps the File entry and refreshes the checksum" {
    fake.reset(exfat);
    fake.dir[2][0] = 0x85;
    fake.dir[2][4] = 0x20;
    fake.dir[3][0] = 0xC0;
    const md = when(17);
    try std.testing.expectEqual(ut.ok, utime("/book.epub", null, &md));
    try std.testing.expectEqual(@as(?u32, 2), fake.written_index);
    try std.testing.expectEqual(@as(u8, 17), fake.dir[2][22]);
    try std.testing.expectEqual(@as(u32, 64), fake.checksum_bytes);
    const csum = std.mem.readInt(u16, fake.dir[2][ut.off_csum..][0..2], .little);
    try std.testing.expectEqual(@as(u16, 0xA020), csum);
    try std.testing.expectEqual(@as(u8, 0xC0), fake.dir[3][0]);
}

test "exFAT errors propagate without a write" {
    fake.reset(exfat);
    fake.find_set_err = ut.err_not_found;
    try std.testing.expectEqual(ut.err_not_found, utime("/nope", null, null));
    fake.reset(exfat);
    fake.read_error = 0x205;
    try std.testing.expectEqual(@as(u16, 0x205), utime("/book.epub", null, null));
    try std.testing.expectEqual(@as(?u32, null), fake.written_index);
    try std.testing.expectEqual(@as(u32, 0), fake.stamp_calls);
}
