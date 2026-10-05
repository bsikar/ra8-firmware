//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! exFAT volume-label get/set (RA8FW-729). The C directory walkers are faked
//! over an in-memory root directory of one cluster.

const std = @import("std");
const fs = @import("ra8_fs");
const lbl = fs.exfat_label;

const fake = @import("fs_walker_fake.zig");
const dir = &fake.dir;
const mount = fake.mount;

fn reset() void {
    fake.reset(0);
}

fn getLabel(buf: []u8) ![]const u8 {
    try std.testing.expectEqual(lbl.ok, lbl.priv_exfat_get_label(mount(), buf.ptr, @intCast(buf.len)));
    return std.mem.sliceTo(buf, 0);
}

test "unlabelled volume reads as empty" {
    reset();
    var buf = [_]u8{'x'} ** 16;
    try std.testing.expectEqualStrings("", try getLabel(&buf));
}

test "set then get round-trips and writes at the end-of-directory slot" {
    reset();
    try std.testing.expectEqual(lbl.ok, lbl.priv_exfat_set_label(mount(), "RA8BOOK"));
    try std.testing.expectEqual(@as(?u32, 2), fake.written_index);
    try std.testing.expectEqual(@as(u32, 5), fake.written_cluster);
    var buf: [16]u8 = undefined;
    try std.testing.expectEqualStrings("RA8BOOK", try getLabel(&buf));
}

test "set rewrites an existing entry in place, cleared entries read empty" {
    reset();
    dir[3][0] = 0x03; // cleared label after a file entry
    dir[2][0] = 0x85;
    var buf: [16]u8 = undefined;
    try std.testing.expectEqualStrings("", try getLabel(&buf));
    try std.testing.expectEqual(lbl.ok, lbl.priv_exfat_set_label(mount(), "NEW"));
    try std.testing.expectEqual(@as(?u32, 3), fake.written_index);
    try std.testing.expectEqualStrings("NEW", try getLabel(&buf));
}

test "label is capped at 11 characters and by the output buffer" {
    reset();
    try std.testing.expectEqual(lbl.ok, lbl.priv_exfat_set_label(mount(), "ABCDEFGHIJKLMNOP"));
    try std.testing.expectEqual(@as(u8, 11), dir[2][lbl.lbl_cnt]);
    var big: [32]u8 = undefined;
    try std.testing.expectEqualStrings("ABCDEFGHIJK", try getLabel(&big));
    var small: [4]u8 = undefined;
    try std.testing.expectEqualStrings("ABC", try getLabel(&small));
}

test "null label writes an empty in-use entry" {
    reset();
    try std.testing.expectEqual(lbl.ok, lbl.priv_exfat_set_label(mount(), null));
    try std.testing.expectEqual(lbl.entry_label, dir[2][0]);
    try std.testing.expectEqual(@as(u8, 0), dir[2][lbl.lbl_cnt]);
}

test "read errors propagate and nothing is written" {
    reset();
    fake.read_error = 0x200;
    var buf: [8]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0x200), lbl.priv_exfat_get_label(mount(), &buf, buf.len));
    try std.testing.expectEqual(@as(u16, 0x200), lbl.priv_exfat_set_label(mount(), "X"));
    try std.testing.expectEqual(@as(?u32, null), fake.written_index);
}

test "a directory with no label and no end marker hits the scan limit" {
    reset();
    for (dir) |*e| e[0] = 0x85;
    var buf: [8]u8 = undefined;
    try std.testing.expectEqual(lbl.err_not_found, lbl.priv_exfat_get_label(mount(), &buf, buf.len));
}
