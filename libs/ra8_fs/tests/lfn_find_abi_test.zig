//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! VFAT long-name directory scan (RA8FW-1086), against the walker fakes.

const std = @import("std");
const fs = @import("ra8_fs");
const find = fs.lfn_find;
const lf = fs.lfn;
const utf = fs.utf;
const fake = @import("fs_walker_fake.zig");
const c = fs.c;

const fat16: u8 = c.k_ra8_fs_type_fat16;
const sfn: *const [11]u8 = "LONGNA~1TXT";

/// Needle for `name`, as UTF-16 units.
fn needleFor(comptime name: []const u8) struct { units: [247]u16, n: u32 } {
    var units: [247]u16 = @splat(0);
    for (name, 0..) |ch, i| units[i] = ch;
    return .{ .units = units, .n = @intCast(name.len) };
}

fn scan(buf: []const u8, cur_lba: u64, s: *find.LfnState, want: []const u16, nneedle: u32) find.Scan {
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    return find.scanSector(&fake.mount_store, want.ptr, nneedle, buf.ptr, cur_lba, s, &lba, &off, &entry);
}

test "find_long matches a one-entry chain and reports the 8.3 entry" {
    fake.reset(fat16);
    fake.layLongName("long name.txt", sfn, 0);
    var loc = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    const err = find.priv_dir_find_long(&fake.mount_store, &loc, "long name.txt", &lba, &off, &entry);
    try std.testing.expectEqual(find.ok, err);
    try std.testing.expectEqual(fake.walk_lba, lba);
    try std.testing.expectEqual(@as(u32, 32), off);
    try std.testing.expectEqualSlices(u8, sfn, entry[0..11]);
    try std.testing.expectEqual(@as(u8, 0x20), entry[11]);
}

test "a leading slash is ignored" {
    fake.reset(fat16);
    fake.layLongName("long name.txt", sfn, 0);
    var loc = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    const err = find.priv_dir_find_long(&fake.mount_store, &loc, "/long name.txt", &lba, &off, &entry);
    try std.testing.expectEqual(find.ok, err);
    try std.testing.expectEqual(fake.walk_lba, lba);
}

test "an empty directory reports not found" {
    fake.reset(fat16);
    var loc = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    const err = find.priv_dir_find_long(&fake.mount_store, &loc, "long name.txt", &lba, &off, &entry);
    try std.testing.expectEqual(find.err_not_found, err);
}

test "a read error propagates" {
    fake.reset(fat16);
    fake.sector_read_err = 0x202;
    var loc = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    const err = find.priv_dir_find_long(&fake.mount_store, &loc, "long name.txt", &lba, &off, &entry);
    try std.testing.expectEqual(@as(u16, 0x202), err);
}

test "a name past the LFN cap never matches" {
    fake.reset(fat16);
    fake.layLongName("long name.txt", sfn, 0);
    var long: [249:0]u8 = @splat('a');
    long[248] = 0; // 248 chars, one past the 247-unit cap
    var loc = std.mem.zeroes(c.dir_loc_t);
    loc.is_root = 1;
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    const err = find.priv_dir_find_long(&fake.mount_store, &loc, &long, &lba, &off, &entry);
    try std.testing.expectEqual(find.err_not_found, err);
}

test "scan: the free-permanent marker ends the sector" {
    fake.reset(fat16);
    var buf: [512]u8 = @splat(0); // entry 0's name byte is 0x00
    var s: find.LfnState = std.mem.zeroes(find.LfnState);
    const n = needleFor("long name.txt");
    try std.testing.expectEqual(find.Scan.eod, scan(&buf, 77, &s, &n.units, n.n));
}

test "scan: a deleted slot resets the chain" {
    fake.reset(fat16);
    fake.layLongName("long name.txt", sfn, 0); // LFN at 0, 8.3 at 32
    fake.sector[32] = 0xE5; // the 8.3 entry slot is marked deleted
    @memcpy(fake.sector[64..75], sfn); // another 8.3 entry further on
    fake.sector[75] = 0x20;
    var s: find.LfnState = std.mem.zeroes(find.LfnState);
    const n = needleFor("long name.txt");
    // Without the reset the surviving chain would match the entry at 64.
    try std.testing.expectEqual(find.Scan.eod, scan(fake.sector, 77, &s, &n.units, n.n));
}

test "scan: a matching chain reports the entry" {
    fake.reset(fat16);
    fake.layLongName("long name.txt", sfn, 0);
    var s: find.LfnState = std.mem.zeroes(find.LfnState);
    const n = needleFor("long name.txt");
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    const got = find.scanSector(&fake.mount_store, &n.units, n.n, fake.sector, 77, &s, &lba, &off, &entry);
    try std.testing.expectEqual(find.Scan.found, got);
    try std.testing.expectEqual(@as(u64, 77), lba);
    try std.testing.expectEqual(@as(u32, 32), off);
    try std.testing.expectEqualSlices(u8, sfn, entry[0..11]);
}

test "scan: a different chain never matches the checksum of another entry" {
    fake.reset(fat16);
    fake.layLongName("other name.txt", "OTHERN~1TXT", 0);
    var s: find.LfnState = std.mem.zeroes(find.LfnState);
    const n = needleFor("long name.txt");
    try std.testing.expectEqual(find.Scan.eod, scan(fake.sector, 77, &s, &n.units, n.n));
}

test "scan: the laid name matches itself" {
    fake.reset(fat16);
    fake.layLongName("other name.txt", "OTHERN~1TXT", 0);
    var s: find.LfnState = std.mem.zeroes(find.LfnState);
    const n = needleFor("other name.txt");
    var lba: u64 = 0;
    var off: u32 = 0;
    var entry: [32]u8 = @splat(0);
    const got = find.scanSector(&fake.mount_store, &n.units, n.n, fake.sector, 77, &s, &lba, &off, &entry);
    try std.testing.expectEqual(find.Scan.found, got);
    // "other name.txt" is 14 units, two LFN sub-entries before the 8.3 entry.
    try std.testing.expectEqual(@as(u32, 64), off);
}

comptime {
    _ = utf;
}
