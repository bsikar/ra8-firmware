//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! VFAT long-name layout helpers (RA8FW-745).

const std = @import("std");
const fs = @import("ra8_fs");
const lfn = fs.lfn;
comptime {
    _ = @import("fs_walker_fake.zig");
}

/// The checksum as the MS FAT spec writes it: rotate right, then add.
fn refChecksum(name: *const [11]u8) u8 {
    var sum: u8 = 0;
    for (name) |ch| sum = std.math.rotr(u8, sum, 1) +% ch;
    return sum;
}

fn unitAt(ent: []const u8, i: usize) u16 {
    return std.mem.readInt(u16, ent[lfn.char_off[i]..][0..2], .little);
}

test "8.3 checksum matches the spec's rotate-and-add" {
    const names = [_]*const [11]u8{ "README  TXT", "A          ", "\xE5BC     DAT", "LONGNA~1JPG" };
    for (names) |n| try std.testing.expectEqual(refChecksum(n), lfn.priv_sfn_checksum(n));
}

test "fill_slot lays out the first group, NUL then 0xFFFF padding" {
    var ent: [32]u8 = @splat(0xAA);
    const name = [_]u16{ 'h', 'e', 'l', 'l', 'o', 0x00E9 };
    lfn.priv_lfn_fill_slot(&ent, &name, name.len, 1, 1, 0x5C);
    try std.testing.expectEqual(@as(u8, 0x41), ent[0]);
    try std.testing.expectEqual(@as(u8, 0x0F), ent[11]);
    try std.testing.expectEqual(@as(u8, 0), ent[12]);
    try std.testing.expectEqual(@as(u8, 0x5C), ent[13]);
    try std.testing.expectEqual(@as(u16, 0), std.mem.readInt(u16, ent[26..28], .little));
    for (name, 0..) |u, i| try std.testing.expectEqual(u, unitAt(&ent, i));
    try std.testing.expectEqual(@as(u16, 0), unitAt(&ent, 6));
    for (7..13) |i| try std.testing.expectEqual(lfn.unicode_pad, unitAt(&ent, i));
}

test "fill_slot second group of a 15-unit name, not last" {
    var ent: [32]u8 = undefined;
    var name: [15]u16 = undefined;
    for (&name, 0..) |*u, i| u.* = @intCast('a' + i);
    lfn.priv_lfn_fill_slot(&ent, &name, name.len, 2, 0, 0x11);
    try std.testing.expectEqual(@as(u8, 2), ent[0]);
    try std.testing.expectEqual(@as(u16, 'n'), unitAt(&ent, 0));
    try std.testing.expectEqual(@as(u16, 'o'), unitAt(&ent, 1));
    try std.testing.expectEqual(@as(u16, 0), unitAt(&ent, 2));
    try std.testing.expectEqual(lfn.unicode_pad, unitAt(&ent, 3));
}

test "fill then add round-trips a two-group name and binds by checksum" {
    const sfn = "HELLOW~1TXT";
    const csum = lfn.priv_sfn_checksum(sfn);
    var name: [20]u16 = undefined;
    for (&name, 0..) |*u, i| u.* = @intCast(0x0400 + i); // Cyrillic, kept as stored
    var e1: [32]u8 = undefined;
    var e2: [32]u8 = undefined;
    lfn.priv_lfn_fill_slot(&e2, &name, name.len, 2, 1, csum);
    lfn.priv_lfn_fill_slot(&e1, &name, name.len, 1, 0, csum);
    var s: lfn.LfnState = undefined;
    lfn.priv_lfn_reset(&s);
    lfn.priv_lfn_add(&s, &e2); // physically first on disk
    lfn.priv_lfn_add(&s, &e1);
    var n: u32 = 99;
    const units = lfn.priv_lfn_units_for(&s, sfn, &n) orelse return error.NoName;
    try std.testing.expectEqual(@as(u32, 20), n);
    try std.testing.expectEqualSlices(u16, &name, units[0..20]);
    try std.testing.expectEqual(@as(?[*]const u16, null), lfn.priv_lfn_units_for(&s, "OTHER   TXT", &n));
    try std.testing.expectEqual(@as(u32, 0), n);
}

test "add ignores out-of-range orders; units_for needs a collected chain" {
    var s: lfn.LfnState = undefined;
    lfn.priv_lfn_reset(&s);
    var ent: [32]u8 = @splat(0);
    ent[1] = 'x';
    ent[0] = 0x40; // order 0
    lfn.priv_lfn_add(&s, &ent);
    ent[0] = 0x40 | 20; // order 20 > 19
    lfn.priv_lfn_add(&s, &ent);
    try std.testing.expectEqual(@as(u8, 0), s.have);
    var n: u32 = 7;
    try std.testing.expectEqual(@as(?[*]const u16, null), lfn.priv_lfn_units_for(&s, "A          ", &n));
    try std.testing.expectEqual(@as(u32, 0), n);
}

test "reset clears units, checksum and have" {
    var s: lfn.LfnState = undefined;
    @memset(std.mem.asBytes(&s), 0x5A);
    lfn.priv_lfn_reset(&s);
    try std.testing.expectEqual(@as(u8, 0), s.have);
    try std.testing.expectEqual(@as(u8, 0), s.checksum);
    for (s.units) |u| try std.testing.expectEqual(@as(u16, 0), u);
}
