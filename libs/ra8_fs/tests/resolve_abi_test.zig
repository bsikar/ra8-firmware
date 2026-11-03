//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Contract tests for the FAT path resolver (`ra8_fs_resolve_abi.zig`):
//! parent/leaf splitting, per-component directory descent through the
//! lookup stand-in, the attribute and cluster guards, the depth and name
//! caps, and error propagation.

const std = @import("std");
const fs = @import("ra8_fs");
const c = fs.c;
const fake = @import("fs_walker_fake.zig");
const resolve = fs.resolve;

const ok: u16 = c.k_ra8_ok;
const err_invalid_arg: u16 = c.k_ra8_err_invalid_arg;
const err_protocol_error: u16 = c.k_ra8_err_protocol_error;
const err_not_found: u16 = c.k_ra8_err_not_found;

const entry_bytes: usize = @intCast(c.k_ra8_fs_dir_entry_bytes);
const off_attr: usize = @intCast(c.k_dir_off_attr);
const off_hi: usize = @intCast(c.k_dir_off_fst_clus_hi);
const off_lo: usize = @intCast(c.k_dir_off_fst_clus_lo);
const attr_directory: u8 = @intCast(c.k_ra8_fs_attr_directory);
const attr_archive: u8 = @intCast(c.k_ra8_fs_attr_archive);

fn layDir(entry: *[entry_bytes]u8, cluster: u32) void {
    entry.* = @splat(0);
    entry[0] = 'D';
    entry[off_attr] = attr_directory;
    entry[off_hi] = @intCast((cluster >> 16) & 0xFF);
    entry[off_hi + 1] = @intCast((cluster >> 24) & 0xFF);
    entry[off_lo] = @intCast(cluster & 0xFF);
    entry[off_lo + 1] = @intCast((cluster >> 8) & 0xFF);
}

fn setup() void {
    fake.reset(0);
    fake.lookup_any_rc = 0;
    fake.lookup_any_lba = 8;
    fake.lookup_any_off = 32;
    layDir(&fake.lookup_any_entry, 0x21);
}

test "a single component resolves to the root with the full leaf" {
    setup();
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    try std.testing.expectEqual(ok, resolve.priv_resolve_parent(m, "a.txt", &parent, &leaf));
    try std.testing.expectEqual(@as(u8, 1), parent.is_root);
    try std.testing.expectEqualStrings("a.txt", std.mem.span(leaf));
}

test "leading slashes are skipped" {
    setup();
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    try std.testing.expectEqual(ok, resolve.priv_resolve_parent(m, "//a.txt", &parent, &leaf));
    try std.testing.expectEqual(@as(u8, 1), parent.is_root);
    try std.testing.expectEqualStrings("a.txt", std.mem.span(leaf));
}

test "a nested path descends through the directory lookup" {
    setup();
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    try std.testing.expectEqual(ok, resolve.priv_resolve_parent(m, "/d/f.txt", &parent, &leaf));
    try std.testing.expectEqual(@as(u8, 0), parent.is_root);
    try std.testing.expectEqual(@as(u32, 0x21), parent.cluster);
    try std.testing.expectEqualStrings("f.txt", std.mem.span(leaf));
}

test "a non-directory component is refused" {
    setup();
    fake.lookup_any_entry[off_attr] = attr_archive;
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    try std.testing.expectEqual(err_invalid_arg, resolve.priv_resolve_parent(m, "/d/f.txt", &parent, &leaf));
}

test "a directory entry without a data cluster is refused" {
    setup();
    layDir(&fake.lookup_any_entry, 0);
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    try std.testing.expectEqual(err_protocol_error, resolve.priv_resolve_parent(m, "/d/f.txt", &parent, &leaf));
}

test "a failing component lookup propagates" {
    setup();
    fake.lookup_any_rc = err_not_found;
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    try std.testing.expectEqual(err_not_found, resolve.priv_resolve_parent(m, "/d/f.txt", &parent, &leaf));
}

test "a path deeper than the component cap is refused" {
    setup();
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    var deep_buf: [80]u8 = undefined;
    for (0..32) |i| {
        deep_buf[i * 2] = 'a';
        deep_buf[i * 2 + 1] = '/';
    }
    @memcpy(deep_buf[64..69], "z.txt");
    deep_buf[69] = 0;
    const deep: [*:0]const u8 = @ptrCast(&deep_buf);
    try std.testing.expectEqual(err_invalid_arg, resolve.priv_resolve_parent(m, deep, &parent, &leaf));
}

test "a component longer than the name cap is refused" {
    setup();
    const m = fake.mount();
    var parent: c.dir_loc_t = undefined;
    var leaf: [*:0]const u8 = undefined;
    var long_buf: [750]u8 = undefined;
    @memset(long_buf[0..742], 'x');
    @memcpy(long_buf[742..748], "/f.txt");
    long_buf[748] = 0;
    const long: [*:0]const u8 = @ptrCast(&long_buf);
    try std.testing.expectEqual(err_invalid_arg, resolve.priv_resolve_parent(m, long, &parent, &leaf));
}

test "resolve_dir on the root returns the root" {
    setup();
    const m = fake.mount();
    var out: c.dir_loc_t = undefined;
    try std.testing.expectEqual(ok, resolve.priv_resolve_dir(m, "/", &out));
    try std.testing.expectEqual(@as(u8, 1), out.is_root);
}

test "resolve_dir on an existing subdirectory returns its cluster" {
    setup();
    const m = fake.mount();
    var out: c.dir_loc_t = undefined;
    try std.testing.expectEqual(ok, resolve.priv_resolve_dir(m, "/d", &out));
    try std.testing.expectEqual(@as(u8, 0), out.is_root);
    try std.testing.expectEqual(@as(u32, 0x21), out.cluster);
}

test "a trailing slash resolves to the parent directory" {
    setup();
    const m = fake.mount();
    var out: c.dir_loc_t = undefined;
    try std.testing.expectEqual(ok, resolve.priv_resolve_dir(m, "/d/", &out));
    try std.testing.expectEqual(@as(u8, 0), out.is_root);
    try std.testing.expectEqual(@as(u32, 0x21), out.cluster);
}

test "a failed leaf lookup surfaces through resolve_dir" {
    setup();
    const m = fake.mount();
    var out: c.dir_loc_t = undefined;
    fake.lookup_any_entry[off_attr] = attr_archive; // leaf resolves to a file
    try std.testing.expectEqual(err_invalid_arg, resolve.priv_resolve_dir(m, "/d", &out));
}

export fn priv_alloc_eoc_cluster(m: [*c]const u8, out_c: [*c]u32) callconv(.c) u16 {
    _ = m;
    out_c.* = 6;
    return 0;
}
