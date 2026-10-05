//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The VFS path-namespace calls (RA8FW-721) against a fake mount table and a
//! fake format: null guards, resolve failures, read-only and capability
//! refusals, same-mount rename, stat translation, and the directory cursor
//! lifecycle.

const std = @import("std");
const io = @import("ra8_io");
const ns = io.vfs_namespace;
const Stream = io.log.Stream;
const Iface = io.stream_ram.Iface;

var errors_logged: u32 = 0;
var resolve_rc: c_int = 0;
var split_rc: c_int = 0;
var calls: u32 = 0;
var last_sub: [32]u8 = undefined;
var last_new: [32]u8 = undefined;
var stat_rc: c_int = 0;
var fake_ctx: u8 = 0;

fn remember(dst: *[32]u8, s: [*:0]const u8) void {
    const n = std.mem.len(s);
    @memset(dst, 0);
    @memcpy(dst[0..n], s[0..n]);
}
fn path_op(ctx: ?*anyopaque, sub: [*:0]const u8) callconv(.c) c_int {
    std.debug.assert(ctx == @as(?*anyopaque, &fake_ctx));
    calls += 1;
    remember(&last_sub, sub);
    return 0;
}
fn rename_op(_: ?*anyopaque, old: [*:0]const u8, new: [*:0]const u8) callconv(.c) c_int {
    calls += 1;
    remember(&last_sub, old);
    remember(&last_new, new);
    return 0;
}
fn stat_op(_: ?*anyopaque, sub: [*:0]const u8, out: *ns.FsStat) callconv(.c) c_int {
    remember(&last_sub, sub);
    if (stat_rc != 0) return stat_rc;
    out.size_bytes = 1234;
    out.first_cluster = 7;
    out.modified = .{ .year = 2026, .valid = true };
    out.attr = 0x20;
    out.is_directory = false;
    return 0;
}
fn listdir_op(_: ?*anyopaque, _: [*:0]const u8, cb: ns.ListdirCb, ctx: ?*anyopaque) callconv(.c) c_int {
    cb("a.txt", 0x20, 5, ctx);
    return 0;
}
fn dir_open_op(_: ?*anyopaque, _: [*:0]const u8, _: *anyopaque, _: u32) callconv(.c) c_int {
    calls += 1;
    return 0;
}
fn dir_next_op(_: ?*anyopaque, out: *anyopaque, entry: *bool) callconv(.c) c_int {
    @as([*]u8, @ptrCast(out))[0] = 'x';
    entry.* = true;
    return 0;
}
fn dir_close_op(_: ?*anyopaque) callconv(.c) c_int {
    calls += 1;
    return 0;
}

const full_ops = ns.Ops{
    .stat = &stat_op,
    .listdir = &listdir_op,
    .dir_open = &dir_open_op,
    .dir_next = &dir_next_op,
    .dir_close = &dir_close_op,
    .unlink = &path_op,
    .rename = &rename_op,
    .mkdir = &path_op,
    .rmdir = &path_op,
};
const bare_ops = ns.Ops{ .stat = &stat_op, .listdir = &listdir_op };
const rw_caps = ns.Caps{
    .supports_mkdir = true,
    .supports_rmdir = true,
    .supports_dir_cursor = true,
    .directory_workspace_bytes = 64,
    .directory_workspace_align = 8,
    .max_open_directories = 2,
};
var format = ns.Format{ .caps = rw_caps, .ops = &full_ops };
var slot = ns.Slot{ .format = &format, .mount_ctx = &fake_ctx, .in_use = true };

/// The fake mount table holds one mount, "sd"; split takes "name:sub".
fn splitPath(path: [*:0]const u8, name: [*]u8, sub: *?[*:0]const u8) c_int {
    const s = std.mem.span(path);
    const colon = std.mem.indexOfScalar(u8, s, ':') orelse return 0x103;
    @memcpy(name[0..colon], s[0..colon]);
    name[colon] = 0;
    sub.* = path + colon + 1;
    return 0;
}

export fn priv_ra8_io_vfs_streq(a: [*:0]const u8, b: [*:0]const u8) bool {
    return std.mem.orderZ(u8, a, b) == .eq;
}
export fn priv_ra8_io_vfs_find(name: [*:0]const u8, _: ?*u8) ?*ns.Slot {
    return if (std.mem.orderZ(u8, name, "sd") == .eq) &slot else null;
}
export fn priv_ra8_io_vfs_split(path: [*:0]const u8, name: [*]u8, sub: *?[*:0]const u8) c_int {
    if (split_rc != 0) return split_rc;
    return splitPath(path, name, sub);
}
export fn priv_ra8_io_vfs_resolve(path: [*:0]const u8, out: *?*ns.Slot, _: ?*u8, sub: *?[*:0]const u8) c_int {
    if (resolve_rc != 0) return resolve_rc;
    var name: [16]u8 = undefined;
    const rc = splitPath(path, &name, sub);
    if (rc != 0) return rc;
    out.* = priv_ra8_io_vfs_find(@ptrCast(&name), null) orelse return 0x106;
    return 0;
}

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}
export fn ra8_io_stream_bind(_: *Stream, _: *const Iface, _: ?*anyopaque) c_int {
    return 0;
}
export fn ra8_io_blockdev_write(_: *const anyopaque, _: u32, _: u32, _: [*]const u8) c_int {
    return 0;
}
export fn ra8_usb_hmsc_read10(_: u8, _: u32, _: u16, _: ?[*]u8) c_int {
    return 0;
}
export fn ra8_usb_hmsc_write10(_: u8, _: u32, _: u16, _: ?[*]const u8) c_int {
    return 0;
}
export fn ra8_usb_hmsc_read_capacity(_: u8, _: *u32, _: *u32) c_int {
    return 0;
}

fn resetFakes() void {
    errors_logged = 0;
    resolve_rc = 0;
    split_rc = 0;
    calls = 0;
    stat_rc = 0;
    format = .{ .caps = rw_caps, .ops = &full_ops };
}
fn lastSub() []const u8 {
    return std.mem.sliceTo(&last_sub, 0);
}

test "path mutations dispatch the sub-path and log null paths" {
    resetFakes();
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_unlink("sd:/a.txt"));
    try std.testing.expectEqualStrings("/a.txt", lastSub());
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_mkdir("sd:/d"));
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_rmdir("sd:/d"));
    try std.testing.expectEqual(@as(u32, 3), calls);
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_unlink(null));
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_mkdir(null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "a resolve failure is returned and logged with its code" {
    resetFakes();
    resolve_rc = 0x106;
    try std.testing.expectEqual(@as(c_int, 0x106), ns.ra8_io_vfs_rmdir("zz:/d"));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "read-only mounts, missing capabilities and missing ops are refused" {
    resetFakes();
    format.caps.read_only = true;
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_unlink("sd:/a"));
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_rename("sd:/a", "sd:/b"));
    format.caps = .{};
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_mkdir("sd:/d"));
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_rmdir("sd:/d"));
    format = .{ .caps = rw_caps, .ops = &bare_ops };
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_unlink("sd:/a"));
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_mkdir("sd:/d"));
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_rename("sd:/a", "sd:/b"));
    try std.testing.expectEqual(@as(u32, 0), calls);
}

test "rename stays inside one mount" {
    resetFakes();
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_rename("sd:/a", "sd:/b"));
    try std.testing.expectEqualStrings("/a", lastSub());
    try std.testing.expectEqualStrings("/b", std.mem.sliceTo(&last_new, 0));
    try std.testing.expectEqual(ns.err_invalid_arg, ns.ra8_io_vfs_rename("sd:/a", "fl:/b"));
    try std.testing.expectEqual(ns.err_not_found, ns.ra8_io_vfs_rename("fl:/a", "fl:/b"));
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_rename(null, "sd:/b"));
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_rename("sd:/a", null));
    split_rc = 0x103;
    try std.testing.expectEqual(@as(c_int, 0x103), ns.ra8_io_vfs_rename("sd:/a", "sd:/b"));
    try std.testing.expectEqual(@as(u32, 1), calls);
}

test "stat copies the entry and reports a missing one as not existing" {
    resetFakes();
    var st: ns.VfsStat = .{ .exists = true, .size_bytes = 9 };
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_stat("sd:/a.txt", &st));
    try std.testing.expect(st.exists);
    try std.testing.expectEqual(@as(u64, 1234), st.size_bytes);
    try std.testing.expectEqual(@as(u16, 2026), st.modified.year);
    try std.testing.expect(st.modified.valid);
    try std.testing.expectEqual(@as(u8, 0x20), st.attr);
    stat_rc = ns.err_not_found;
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_stat("sd:/gone", &st));
    try std.testing.expect(!st.exists);
    try std.testing.expectEqual(@as(u64, 0), st.size_bytes);
    stat_rc = 0x401;
    try std.testing.expectEqual(@as(c_int, 0x401), ns.ra8_io_vfs_stat("sd:/a", &st));
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_stat("sd:/a", null));
}

var listed: u32 = 0;
fn onEntry(_: ?[*:0]const u8, _: u8, size: u64, ctx: ?*anyopaque) callconv(.c) void {
    std.debug.assert(ctx == @as(?*anyopaque, &listed));
    listed += @intCast(size);
}

test "listdir hands the callback through" {
    resetFakes();
    listed = 0;
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_listdir("sd:/", &onEntry, &listed));
    try std.testing.expectEqual(@as(u32, 5), listed);
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_listdir("sd:/", null, null));
}

test "dir requirements report the format's cursor needs" {
    resetFakes();
    var bytes: u32 = 1;
    var alignment: u8 = 1;
    var max_open: u16 = 1;
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_dir_requirements("sd:/", &bytes, &alignment, &max_open));
    try std.testing.expectEqual(@as(u32, 64), bytes);
    try std.testing.expectEqual(@as(u8, 8), alignment);
    try std.testing.expectEqual(@as(u16, 2), max_open);
    format.caps.supports_dir_cursor = false;
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_dir_requirements("sd:/", &bytes, &alignment, &max_open));
    try std.testing.expectEqual(@as(u32, 0), bytes);
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_dir_requirements("sd:/", null, &alignment, &max_open));
    resolve_rc = 0x106;
    try std.testing.expectEqual(@as(c_int, 0x106), ns.ra8_io_vfs_dir_requirements("sd:/", &bytes, &alignment, &max_open));
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "a directory cursor opens, steps and closes" {
    resetFakes();
    var ws: [64]u8 align(8) = undefined;
    var dir: ns.Dir = .{};
    var entry: [ns.dirent_bytes]u8 align(8) = undefined;
    var got = false;
    try std.testing.expectEqual(ns.err_invalid_state, ns.ra8_io_vfs_dir_next(&dir, &entry, &got));
    try std.testing.expectEqual(ns.err_no_mem, ns.ra8_io_vfs_dir_open("sd:/", &dir, &ws, 63));
    try std.testing.expectEqual(ns.err_invalid_arg, ns.ra8_io_vfs_dir_open("sd:/", &dir, @ptrCast(&ws[1]), 64));
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_dir_open("sd:/", &dir, &ws, 64));
    try std.testing.expect(dir.is_open);
    try std.testing.expectEqual(@as(?*const ns.Format, &format), dir.format);
    try std.testing.expectEqual(ns.err_busy, ns.ra8_io_vfs_dir_open("sd:/", &dir, &ws, 64));
    @memset(&entry, 0xAA);
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_dir_next(&dir, &entry, &got));
    try std.testing.expect(got);
    try std.testing.expectEqual(@as(u8, 'x'), entry[0]);
    try std.testing.expectEqual(@as(u8, 0), entry[ns.dirent_bytes - 1]);
    try std.testing.expectEqual(ns.ok, ns.ra8_io_vfs_dir_close(&dir));
    try std.testing.expect(!dir.is_open);
    try std.testing.expectEqual(ns.err_invalid_state, ns.ra8_io_vfs_dir_close(&dir));
    try std.testing.expectEqual(@as(u32, 2), calls);
    format.caps.supports_dir_cursor = false;
    try std.testing.expectEqual(ns.err_not_supported, ns.ra8_io_vfs_dir_open("sd:/", &dir, &ws, 64));
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_dir_open("sd:/", &dir, null, 64));
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_dir_next(&dir, null, &got));
    try std.testing.expectEqual(ns.err_null_ptr, ns.ra8_io_vfs_dir_close(null));
}

// The other units in the same archive need these to link; unused here.
// The other units in the same archive need these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_io_stream_write(_: *io.log.Stream, _: [*]const u8, _: u32, _: ?*u32) c_int {
    return 0;
}
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}
export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}
export fn ra8_sdramc_init() c_int {
    return 0;
}
// The i2c_bus_riic unit in the same archive needs these to link; unused here.
export fn ra8_i2c_write(_: u8, _: u8, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i2c_read(_: u8, _: u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_i2c_transfer(_: u8, _: u8, _: ?[*]const u8, _: u32, _: ?[*]u8, _: u32) c_int {
    return 0;
}
// The SPI units in the same archive need these to link; unused here.
export fn ra8_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}
export fn ra8_spi_write_read(_: u8, _: ?*const anyopaque, _: ?*anyopaque, _: u32, _: u8) c_int {
    return 0;
}
export fn ra8_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}
export fn ra8_sci_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}
export fn ra8_sci_spi_xfer(_: u8, _: ?[*]const u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}
// The i2c_bus_i3c_compat unit in the same archive needs these to link; unused here.
export fn ra8_i3c_write(_: u8, _: u8, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i3c_read(_: u8, _: u8, _: ?[*]u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i3c_transfer(_: u8, _: u8, _: ?[*]const u8, _: u32, _: ?[*]u8, _: u32) c_int {
    return 0;
}
// The archive root also emits the SDHI block device (RA8FW-715).
export fn ra8_sdcard_read_blocks(_: u32, _: [*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_sdcard_write_blocks(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_sdcard_get_capacity(_: *u32) c_int {
    return 0;
}

export fn ra8_sdmmc_spi_read_blocks(_: u32, _: [*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_sdmmc_spi_write_blocks(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_sdmmc_spi_erase_blocks(_: u32, _: u32) c_int {
    return 0;
}
export fn ra8_sdmmc_spi_get_capacity(_: *u32) c_int {
    return 0;
}
