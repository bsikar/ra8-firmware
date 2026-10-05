//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The VFS mount table and file handles (RA8FW-736) against a fake format:
//! mount and unmount bookkeeping, the native gate on raw opens, the file
//! handle lifecycle, and capability refusals.

const std = @import("std");
const io = @import("ra8_io");
const vfs = io.vfs;
const ns = io.vfs_namespace;

var fake_ctx: u8 = 0;
var file_ctx: u8 = 0;
var pos: u64 = 0;
var unmounts: u32 = 0;
var errors_logged: u32 = 0;

fn mount_op(_: *const anyopaque, out: *?*anyopaque) callconv(.c) c_int {
    out.* = &fake_ctx;
    return 0;
}
fn unmount_op(_: ?*anyopaque) callconv(.c) c_int {
    unmounts += 1;
    return 0;
}
fn open_op(_: ?*anyopaque, _: [*:0]const u8, _: u8, out: *?*anyopaque) callconv(.c) c_int {
    out.* = &file_ctx;
    return 0;
}
fn ctx_op(ctx: ?*anyopaque) callconv(.c) c_int {
    std.debug.assert(ctx == @as(?*anyopaque, &file_ctx));
    return 0;
}
fn read_op(_: ?*anyopaque, _: *anyopaque, bytes: u32, out: *u32) callconv(.c) c_int {
    out.* = bytes / 2;
    return 0;
}
fn write_op(_: ?*anyopaque, _: *const anyopaque, bytes: u32) callconv(.c) c_int {
    pos += bytes;
    return 0;
}
fn seek_op(_: ?*anyopaque, offset: u64) callconv(.c) c_int {
    pos = offset;
    return 0;
}
fn query_op(_: ?*anyopaque, out: *u64) callconv(.c) c_int {
    out.* = pos;
    return 0;
}

const ops = ns.Ops{
    .mount = @ptrCast(&mount_op),
    .unmount = @ptrCast(&unmount_op),
    .open = @ptrCast(&open_op),
    .close = @ptrCast(&ctx_op),
    .read = @ptrCast(&read_op),
    .write = @ptrCast(&write_op),
    .seek = @ptrCast(&seek_op),
    .tell = @ptrCast(&query_op),
    .size = @ptrCast(&query_op),
    .sync = @ptrCast(&ctx_op),
};
const rw_caps = ns.Caps{ .supports_streaming_write = true, .supports_sync = true };
var format = ns.Format{ .caps = rw_caps, .ops = &ops };
var backend: [5]usize = .{0} ** 5;

export fn ra8_io_fsfmt_probe(_: *const anyopaque, out: *?*const ns.Format) c_int {
    out.* = &format;
    return 0;
}
export fn ra8_io_fsfmt_get_builtin(fs_type: u8, out: *?*const ns.Format) c_int {
    if (fs_type != vfs.fs_type_exfat) return ns.err_not_supported;
    out.* = &format;
    return 0;
}
export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
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
// The other units in the same archive need these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}
export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
}
export fn ra8_sdramc_init() c_int {
    return 0;
}
export fn ra8_i2c_write(_: u8, _: u8, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i2c_read(_: u8, _: u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_i2c_transfer(_: u8, _: u8, _: ?[*]const u8, _: u32, _: ?[*]u8, _: u32) c_int {
    return 0;
}
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
export fn ra8_i3c_write(_: u8, _: u8, _: ?[*]const u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i3c_read(_: u8, _: u8, _: ?[*]u8, _: u32, _: bool) c_int {
    return 0;
}
export fn ra8_i3c_transfer(_: u8, _: u8, _: ?[*]const u8, _: u32, _: ?[*]u8, _: u32) c_int {
    return 0;
}
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
export fn ra8_flash_extra_mram_write(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_flash_extra_mram_erase(_: u32) c_int {
    return 0;
}

fn reset() !void {
    format = .{ .caps = rw_caps, .ops = &ops };
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_init());
    unmounts = 0;
    errors_logged = 0;
    pos = 0;
}

test "mount_auto fills the table and refuses duplicates and overflow" {
    try reset();
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_mount_auto("sd", &backend));
    try std.testing.expectEqual(vfs.err_exists, vfs.ra8_io_vfs_mount_auto("sd", &backend));
    try std.testing.expectEqual(ns.err_invalid_arg, vfs.ra8_io_vfs_mount_auto("", &backend));
    try std.testing.expectEqual(ns.err_null_ptr, vfs.ra8_io_vfs_mount_auto(null, &backend));
    try std.testing.expectEqual(ns.err_null_ptr, vfs.ra8_io_vfs_mount_auto("x", null));
    for ([_][*:0]const u8{ "a", "b", "c" }) |n| {
        try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_mount_auto(n, &backend));
    }
    try std.testing.expectEqual(ns.err_no_mem, vfs.ra8_io_vfs_mount_auto("d", &backend));
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_unmount("a"));
    try std.testing.expectEqual(@as(u32, 1), unmounts);
    try std.testing.expectEqual(ns.err_not_found, vfs.ra8_io_vfs_unmount("a"));
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_init());
    try std.testing.expectEqual(@as(u32, 4), unmounts);
}

test "a native mount needs a built-in type and is not unmounted by the table" {
    try reset();
    var m = vfs.FsMount{ .backend = .{0} ** 5, .type = vfs.fs_type_fat16 };
    try std.testing.expectEqual(ns.err_not_supported, vfs.ra8_io_vfs_mount("fl", &m));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
    m.type = vfs.fs_type_exfat;
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_mount("fl", &m));
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_unmount("fl"));
    try std.testing.expectEqual(@as(u32, 0), unmounts);
}

test "a file handle reads, writes, seeks and closes" {
    try reset();
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_mount_auto("sd", &backend));
    var f: ?*vfs.File = null;
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_open("sd:/a", vfs.mode_write, &f));
    try std.testing.expectEqual(ns.err_busy, vfs.ra8_io_vfs_unmount("sd"));
    var buf: [8]u8 = undefined;
    var got: u32 = 0;
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_read(f, &buf, 8, &got));
    try std.testing.expectEqual(@as(u32, 4), got);
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_write(f, &buf, 5));
    var at: u64 = 0;
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_tell(f, &at));
    try std.testing.expectEqual(@as(u64, 5), at);
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_seek(f, 2));
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_size(f, &at));
    try std.testing.expectEqual(@as(u64, 2), at);
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_sync(f));
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_close(f));
    try std.testing.expectEqual(ns.err_invalid_state, vfs.ra8_io_vfs_file_close(f));
    try std.testing.expectEqual(ns.err_null_ptr, vfs.ra8_io_vfs_file_close(null));
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_unmount("sd"));
}

test "open refuses bad modes, read-only writes and a full handle table" {
    try reset();
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_mount_auto("sd", &backend));
    var f: ?*vfs.File = null;
    try std.testing.expectEqual(ns.err_invalid_arg, vfs.ra8_io_vfs_file_open("sd:/a", 7, &f));
    try std.testing.expectEqual(ns.err_not_found, vfs.ra8_io_vfs_file_open("zz:/a", vfs.mode_read, &f));
    format.caps.read_only = true;
    try std.testing.expectEqual(ns.err_not_supported, vfs.ra8_io_vfs_file_open("sd:/a", vfs.mode_append, &f));
    for (0..vfs.max_files) |_| {
        try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_file_open("sd:/a", vfs.mode_read, &f));
    }
    try std.testing.expectEqual(ns.err_no_mem, vfs.ra8_io_vfs_file_open("sd:/a", vfs.mode_read, &f));
    try std.testing.expectEqual(ns.err_not_supported, vfs.ra8_io_vfs_file_write(f, &backend, 1));
}

test "raw open is native-only and caps and free space follow the format" {
    try reset();
    var m = vfs.FsMount{ .backend = .{0} ** 5, .type = vfs.fs_type_exfat };
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_mount("fl", &m));
    var raw: ?*anyopaque = null;
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_open("fl:/a", vfs.mode_read, &raw));
    try std.testing.expectEqual(@as(?*anyopaque, &file_ctx), raw);
    var caps: ns.Caps = .{};
    try std.testing.expectEqual(ns.ok, vfs.ra8_io_vfs_get_caps("fl", &caps));
    try std.testing.expect(caps.supports_sync);
    try std.testing.expectEqual(ns.err_not_found, vfs.ra8_io_vfs_get_caps("zz", &caps));
    var space: [16]u8 = undefined;
    try std.testing.expectEqual(ns.err_not_supported, vfs.ra8_io_vfs_free_space("fl", &space));
}
