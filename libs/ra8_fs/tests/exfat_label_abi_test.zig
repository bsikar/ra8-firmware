//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The exFAT volume label (RA8FW-730) over a fake root directory: a flat
//! list of 32-byte entries the cursor walks, eight to a cluster.

const std = @import("std");
const fs = @import("ra8_fs");
const label = fs.exfat_label;

const per_cluster: u32 = 8;
const root_cluster: u32 = 5;

var dir: [24][32]u8 = undefined;
var fail_read: bool = false;
var endless: bool = false;
var writes: u32 = 0;
var written_cluster: u32 = 0;
var written_index: u32 = 0;
var written: [32]u8 = undefined;

export fn priv_exfat_dir_root(_: ?*const label.Mount, out: *label.Dir) void {
    out.* = .{ .cluster = root_cluster, .contig_end = 0, .self_cluster = 0, .self_index = 0 };
}
export fn priv_exfat_cursor_init(d: *const label.Dir, out: *label.Cursor) void {
    out.* = .{ .cluster = d.cluster, .entry_in_cluster = 0, .scanned = 0, .contig_end = 0 };
}
export fn priv_exfat_next_entry(_: ?*const label.Mount, cur: *label.Cursor, out: [*]u8) c_int {
    if (fail_read) return 0x401;
    const flat = if (endless) 1 else cur.scanned;
    @memcpy(out[0..32], &dir[flat]);
    cur.scanned += 1;
    cur.entry_in_cluster += 1;
    if (cur.entry_in_cluster == per_cluster) {
        cur.entry_in_cluster = 0;
        cur.cluster += 1;
    }
    return 0;
}
export fn priv_exfat_write_dir_set(_: ?*const label.Mount, cluster: u32, idx: u32, set: [*]const u8, bytes: u32) c_int {
    writes += 1;
    written_cluster = cluster;
    written_index = idx;
    @memcpy(&written, set[0..bytes]);
    return 0;
}

fn reset() void {
    for (&dir) |*e| e.* = [_]u8{0} ** 32;
    fail_read = false;
    endless = false;
    writes = 0;
    written_cluster = 0;
    written_index = 0;
}

fn putLabel(at: usize, kind: u8, text: []const u8) void {
    dir[at][0] = kind;
    dir[at][1] = @intCast(text.len);
    for (text, 0..) |c, i| dir[at][2 + i * 2] = c;
}

fn get(out: []u8) !c_int {
    return label.priv_exfat_get_label(null, out.ptr, @intCast(out.len));
}

test "mirrors match the C layout" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(label.Dir));
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(label.Cursor));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(label.Cursor, "scanned"));
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(label.SetPos));
}

test "an in-use label entry decodes" {
    reset();
    dir[0][0] = 0x81; // bitmap
    dir[1][0] = 0x82; // up-case table
    putLabel(2, 0x83, "RA8BOOK");
    var out: [16]u8 = undefined;
    try std.testing.expectEqual(label.ok, try get(&out));
    try std.testing.expectEqualStrings("RA8BOOK", std.mem.sliceTo(&out, 0));
}

test "a missing or cleared label reads as empty" {
    reset();
    var out = [_]u8{'x'} ** 8;
    try std.testing.expectEqual(label.ok, try get(&out));
    try std.testing.expectEqual(@as(u8, 0), out[0]);
    putLabel(0, 0x03, "OLD");
    out[0] = 'x';
    try std.testing.expectEqual(label.ok, try get(&out));
    try std.testing.expectEqual(@as(u8, 0), out[0]);
}

test "decode caps at eleven units and the output length" {
    reset();
    putLabel(0, 0x83, "ABCDEFGHIJK");
    dir[0][1] = 15;
    var wide: [32]u8 = undefined;
    try std.testing.expectEqual(label.ok, try get(&wide));
    try std.testing.expectEqualStrings("ABCDEFGHIJK", std.mem.sliceTo(&wide, 0));
    var narrow: [4]u8 = undefined;
    try std.testing.expectEqual(label.ok, try get(&narrow));
    try std.testing.expectEqualStrings("ABC", std.mem.sliceTo(&narrow, 0));
}

test "set rewrites an existing entry in place" {
    reset();
    for (0..10) |i| dir[i][0] = 0x85;
    putLabel(10, 0x03, "OLD");
    try std.testing.expectEqual(label.ok, label.priv_exfat_set_label(null, "NEW"));
    try std.testing.expectEqual(@as(u32, 1), writes);
    try std.testing.expectEqual(root_cluster + 1, written_cluster);
    try std.testing.expectEqual(@as(u32, 2), written_index);
    try std.testing.expectEqual(@as(u8, 0x83), written[0]);
    try std.testing.expectEqual(@as(u8, 3), written[1]);
    try std.testing.expectEqual(@as(u8, 'N'), written[2]);
    try std.testing.expectEqual(@as(u8, 0), written[3]);
    try std.testing.expectEqual(@as(u8, 'W'), written[6]);
}

test "set with no entry uses the end-of-directory slot" {
    reset();
    dir[0][0] = 0x81;
    dir[1][0] = 0x82;
    try std.testing.expectEqual(label.ok, label.priv_exfat_set_label(null, "ABCDEFGHIJKLMN"));
    try std.testing.expectEqual(root_cluster, written_cluster);
    try std.testing.expectEqual(@as(u32, 2), written_index);
    try std.testing.expectEqual(@as(u8, 11), written[1]);
}

test "set with null clears to an empty in-use entry" {
    reset();
    try std.testing.expectEqual(label.ok, label.priv_exfat_set_label(null, null));
    try std.testing.expectEqual(@as(u8, 0x83), written[0]);
    try std.testing.expectEqual(@as(u8, 0), written[1]);
}

test "read errors and the scan bound pass through" {
    reset();
    fail_read = true;
    var out: [8]u8 = undefined;
    try std.testing.expectEqual(@as(c_int, 0x401), try get(&out));
    try std.testing.expectEqual(@as(c_int, 0x401), label.priv_exfat_set_label(null, "X"));
    fail_read = false;
    endless = true;
    dir[1][0] = 0x85;
    try std.testing.expectEqual(label.err_not_found, try get(&out));
    try std.testing.expectEqual(label.err_not_found, label.priv_exfat_set_label(null, "X"));
    try std.testing.expectEqual(@as(u32, 0), writes);
}
