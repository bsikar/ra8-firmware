//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the FAT directory verbs (`ra8_fs_fat_dir_abi.zig`):
//! listdir reports each visible entry once in order, skips deleted slots and
//! the dot entries, and falls back to the 8.3 alias when an LFN chain does
//! not convert; the independent dir cursor iterates the same sequence and
//! closes cleanly; unlink frees the chain then erases the whole entry while
//! refusing directories and read-only files before touching anything; and
//! rename re-files the entry under the new name before erasing the old,
//! refusing existing targets and cross-directory moves.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const dir_ops = fs.fat_dir;

const fat16: u8 = c.k_ra8_fs_type_fat16;
const exfat: u8 = c.k_ra8_fs_type_exfat;

const ok: u16 = c.k_ra8_ok;
const err_not_found: u16 = c.k_ra8_err_not_found;
const err_invalid_state: u16 = c.k_ra8_err_invalid_state;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_access_denied: u16 = c.k_ra8_err_access_denied;
const err_exists: u16 = c.k_ra8_err_exists;
const err_not_supported: u16 = c.k_ra8_err_not_supported;
const err_busy: u16 = c.k_ra8_err_busy;

const attr_read_only: u8 = 0x01;
const attr_directory: u8 = 0x10;
const attr_archive: u8 = 0x20;

const Collector = struct {
    names: [4][64]u8 = @splat(@splat(0)),
    attrs: [4]u8 = @splat(0),
    sizes: [4]u64 = @splat(0),
    n: usize = 0,
};

fn collect_cb(name: [*c]const u8, attr: u8, size: u64, ctx: ?*anyopaque) callconv(.c) void {
    const col: *Collector = @ptrCast(@alignCast(ctx.?));
    if (col.n >= col.names.len) return;
    var len: usize = 0;
    while (name[len] != 0) : (len += 1) {}
    @memcpy(col.names[col.n][0..len], name[0..len]);
    col.attrs[col.n] = attr;
    col.sizes[col.n] = size;
    col.n += 1;
}

fn expectName(col: *const Collector, i: usize, want: []const u8) !void {
    try std.testing.expectEqualStrings(want, std.mem.sliceTo(&col.names[i], 0));
}

/// Root image: "long name.txt" as one LFN chain + short alias at slot 1,
/// then "ONE.TXT" at slot 2, a deleted slot at 3, end-of-directory at 4.
fn layRoot() void {
    fake.layLongName("long name.txt", "LONGNA~1TXT", 0);
    fake.sector[32 + 28] = 0x00;
    fake.sector[32 + 29] = 0x00;
    fake.sector[32 + 30] = 0x01;
    fake.sector[32 + 31] = 0x00; // 65536 bytes
    fake.lay83(2, "ONE     TXT", attr_archive, 7);
    fake.sector[64 + 28] = 0xD2;
    fake.sector[64 + 29] = 0x04; // 1234 bytes
    fake.sector[3 * 32] = 0xE5;
}

test "listdir reports each visible entry once, long name first" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    layRoot();
    var col: Collector = .{};
    const err = dir_ops.ra8_fs_listdir(fake.mount(), "/", collect_cb, &col);
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(usize, 2), col.n);
    try expectName(&col, 0, "long name.txt");
    try std.testing.expectEqual(attr_archive, col.attrs[0]);
    try std.testing.expectEqual(@as(u64, 65536), col.sizes[0]);
    try expectName(&col, 1, "ONE.TXT");
    try std.testing.expectEqual(@as(u64, 1234), col.sizes[1]);
}

test "listdir skips the dot entries inside a subdirectory" {
    fake.reset(fat16);
    fake.lay83(0, "A        ", attr_directory, 4);
    fake.dir_lba = 1 + (4 - 2); // cluster 4 lands here under the fake's mapping
    fake.dir[0][0] = '.';
    fake.dir[1][0] = '.';
    fake.lay83(2, "B        ", attr_archive, 0);
    var col: Collector = .{};
    const err = dir_ops.ra8_fs_listdir(fake.mount(), "/a", collect_cb, &col);
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(usize, 1), col.n);
    try expectName(&col, 0, "B");
}

test "listdir propagates a read error" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.sector_read_err = 0x202;
    var col: Collector = .{};
    const err = dir_ops.ra8_fs_listdir(fake.mount(), "/", collect_cb, &col);
    try std.testing.expectEqual(@as(u16, 0x202), err);
}

test "listdir refuses an unmounted handle" {
    fake.reset(fat16);
    fake.mount_store.in_use = 0;
    var col: Collector = .{};
    const err = dir_ops.ra8_fs_listdir(fake.mount(), "/", collect_cb, &col);
    try std.testing.expectEqual(err_invalid_state, err);
}

test "listdir on exFAT dispatches to the exFAT resolver" {
    fake.reset(exfat);
    var col: Collector = .{};
    const err = dir_ops.ra8_fs_listdir(fake.mount(), "/nope", collect_cb, &col);
    try std.testing.expectEqual(err_not_found, err);
}

test "the dir cursor walks the same sequence and closes" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    layRoot();
    var directory: c.ra8_fs_dir_t = std.mem.zeroes(c.ra8_fs_dir_t);
    try std.testing.expectEqual(ok, dir_ops.ra8_fs_dir_open(fake.mount(), "/", &directory));
    var out: c.ra8_fs_dirent_t = std.mem.zeroes(c.ra8_fs_dirent_t);
    var got: bool = false;
    try std.testing.expectEqual(ok, dir_ops.ra8_fs_dir_next(&directory, &out, &got));
    try std.testing.expect(got);
    try std.testing.expectEqualStrings("long name.txt", std.mem.sliceTo(&out.name, 0));
    try std.testing.expectEqual(ok, dir_ops.ra8_fs_dir_next(&directory, &out, &got));
    try std.testing.expect(got);
    try std.testing.expectEqualStrings("ONE.TXT", std.mem.sliceTo(&out.name, 0));
    try std.testing.expectEqual(ok, dir_ops.ra8_fs_dir_next(&directory, &out, &got));
    try std.testing.expect(!got);
    // A cursor at clean end keeps answering end, not an error.
    try std.testing.expectEqual(ok, dir_ops.ra8_fs_dir_next(&directory, &out, &got));
    try std.testing.expect(!got);
    try std.testing.expectEqual(ok, dir_ops.ra8_fs_dir_close(&directory));
    try std.testing.expectEqual(err_invalid_state, dir_ops.ra8_fs_dir_next(&directory, &out, &got));
}

test "dir_open refuses a handle that is already open and clears state on failure" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    var directory: c.ra8_fs_dir_t = std.mem.zeroes(c.ra8_fs_dir_t);
    try std.testing.expectEqual(ok, dir_ops.ra8_fs_dir_open(fake.mount(), "/", &directory));
    try std.testing.expectEqual(err_busy, dir_ops.ra8_fs_dir_open(fake.mount(), "/", &directory));
    fake.mount_store.in_use = 0;
    var other: c.ra8_fs_dir_t = std.mem.zeroes(c.ra8_fs_dir_t);
    try std.testing.expectEqual(err_invalid_state, dir_ops.ra8_fs_dir_open(fake.mount(), "/", &other));
    var bytes: [c.k_ra8_fs_dir_state_bytes]u8 = @splat(0);
    @memcpy(&bytes, other.state[0..]);
    for (bytes) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "unlink frees the chain and erases the whole entry" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.lay83(0, "ONE     TXT", attr_archive, 7);
    fake.fat[7] = 0xFFFF; // FAT16 end-of-chain
    const err = dir_ops.ra8_fs_unlink(fake.mount(), "/ONE.TXT");
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqual(@as(u32, 0), fake.fat[7]); // the chain is free
    try std.testing.expectEqual(fake.walk_lba, fake.sector_written.?);
    try std.testing.expectEqual(@as(u8, 0xE5), fake.sector[0]); // entry erased
}

test "unlink refuses a directory before touching anything" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.lay83(0, "SUBDIR   ", attr_directory, 7);
    const err = dir_ops.ra8_fs_unlink(fake.mount(), "/SUBDIR");
    try std.testing.expectEqual(err_invalid_arg, err);
    try std.testing.expect(fake.sector_written == null);
}

test "unlink refuses a read-only file before touching anything" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.lay83(0, "ONE     TXT", attr_read_only | attr_archive, 7);
    const err = dir_ops.ra8_fs_unlink(fake.mount(), "/ONE.TXT");
    try std.testing.expectEqual(err_access_denied, err);
    try std.testing.expect(fake.sector_written == null);
    try std.testing.expectEqual(@as(u32, 0), fake.fat[7]);
}

test "unlink reports a missing file" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    const err = dir_ops.ra8_fs_unlink(fake.mount(), "/GONE.TXT");
    try std.testing.expectEqual(err_not_found, err);
}

test "unlink on exFAT dispatches to the exFAT resolver" {
    fake.reset(exfat);
    const err = dir_ops.ra8_fs_unlink(fake.mount(), "/nope");
    try std.testing.expectEqual(err_not_found, err);
}

test "rename re-files the entry under the new name, then erases the old" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.sector[0] = 0xE5; // slot 0 deleted: room for the new entry
    fake.lay83(1, "ONE     TXT", attr_archive, 7);
    fake.fat[7] = 0xFFFF;
    const err = dir_ops.ra8_fs_rename(fake.mount(), "/ONE.TXT", "/TWO.TXT");
    try std.testing.expectEqual(ok, err);
    try std.testing.expectEqualStrings("TWO", std.mem.sliceTo(fake.sector[0..8], 0)[0..3]);
    try std.testing.expectEqual(@as(u8, 'T'), fake.sector[8]);
    try std.testing.expectEqual(@as(u8, 'X'), fake.sector[9]);
    try std.testing.expectEqual(@as(u8, 'T'), fake.sector[10]);
    try std.testing.expectEqual(@as(u8, 0xE5), fake.sector[32]); // the old slot is gone
    try std.testing.expectEqual(@as(u32, 0xFFFF), fake.fat[7]); // the chain stays
}

test "rename refuses a target that already exists" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.lay83(0, "ONE     TXT", attr_archive, 7);
    fake.lay83(1, "TWO     TXT", attr_archive, 8);
    const err = dir_ops.ra8_fs_rename(fake.mount(), "/ONE.TXT", "/TWO.TXT");
    try std.testing.expectEqual(err_exists, err);
    try std.testing.expect(fake.sector_written == null);
}

test "rename refuses a move across directories" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.lay83(0, "A        ", attr_directory, 4);
    fake.lay83(1, "ONE     TXT", attr_archive, 7);
    const err = dir_ops.ra8_fs_rename(fake.mount(), "/ONE.TXT", "/A/TWO.TXT");
    try std.testing.expectEqual(err_not_supported, err);
    try std.testing.expect(fake.sector_written == null);
}

test "rename refuses a read-only file before filing anything" {
    fake.reset(fat16);
    fake.dir_lba = fake.walk_lba;
    fake.lay83(0, "ONE     TXT", attr_read_only | attr_archive, 7);
    const err = dir_ops.ra8_fs_rename(fake.mount(), "/ONE.TXT", "/TWO.TXT");
    try std.testing.expectEqual(err_access_denied, err);
    try std.testing.expect(fake.sector_written == null);
}

test "rename on exFAT dispatches to the exFAT path" {
    fake.reset(exfat);
    const err = dir_ops.ra8_fs_rename(fake.mount(), "/nope", "/alsonope");
    try std.testing.expectEqual(err_not_found, err);
}
