//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The format registry (RA8FW-738): built-in registration, the type map,
//! probe order, the register-time contract checks, and the built-in
//! directory wrappers over a stubbed ra8_fs.

const std = @import("std");
const io = @import("ra8_io");
const fsfmt = io.fsfmt;
const ns = io.vfs_namespace;
const fs = @import("fs_stubs.zig");

var errors_logged: u32 = 0;
var backend: [5]usize = .{0} ** 5;

fn probe_op(_: *const anyopaque) callconv(.c) bool {
    return true;
}
fn noop_op() callconv(.c) c_int {
    return 0;
}
fn path_op(_: ?*anyopaque, _: [*:0]const u8) callconv(.c) c_int {
    return 0;
}
fn stat_op(_: ?*anyopaque, _: [*:0]const u8, _: *ns.FsStat) callconv(.c) c_int {
    return 0;
}
fn listdir_op(_: ?*anyopaque, _: [*:0]const u8, _: ns.ListdirCb, _: ?*anyopaque) callconv(.c) c_int {
    return 0;
}
const base_ops = ns.Ops{
    .probe = @ptrCast(&probe_op),
    .mount = @ptrCast(&noop_op),
    .unmount = @ptrCast(&noop_op),
    .open = @ptrCast(&noop_op),
    .close = @ptrCast(&noop_op),
    .read = @ptrCast(&noop_op),
    .seek = @ptrCast(&noop_op),
    .tell = @ptrCast(&noop_op),
    .size = @ptrCast(&noop_op),
    .stat = &stat_op,
    .listdir = &listdir_op,
};
const fake = ns.Format{ .name = "fake", .ops = &base_ops };

var seen: u32 = 0;
var seen_size: u64 = 0;
fn count_cb(name: ?[*:0]const u8, attr: u8, size: u64, _: ?*anyopaque) callconv(.c) void {
    std.debug.assert(name.?[0] == 'a' and attr == 0x20);
    seen += 1;
    seen_size += size;
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
    fs.reset();
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_init());
    errors_logged = 0;
}

fn builtin(t: c_int) !*const ns.Format {
    var out: ?*const ns.Format = null;
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_get_builtin(t, &out));
    return out.?;
}

test "the type map hands back the shared FAT format and exFAT" {
    const fat = try builtin(fsfmt.fs_type_fat12);
    try std.testing.expectEqual(fat, try builtin(fsfmt.fs_type_fat16));
    try std.testing.expectEqual(fat, try builtin(fsfmt.fs_type_fat32));
    const exfat = try builtin(fsfmt.fs_type_exfat);
    try std.testing.expect(fat != exfat);
    try std.testing.expectEqualStrings("fat", std.mem.span(fat.name.?));
    try std.testing.expectEqual(fsfmt.fat_max_name_utf8, fat.caps.max_name_len);
    try std.testing.expectEqual(fsfmt.exfat_max_name_utf8, exfat.caps.max_name_len);
    try std.testing.expectEqual(@as(u32, 648), exfat.caps.directory_workspace_bytes);
    try std.testing.expectEqual(@as(u8, 8), exfat.caps.directory_workspace_align);
    var out: ?*const ns.Format = null;
    try std.testing.expectEqual(ns.err_invalid_arg, fsfmt.ra8_io_fsfmt_get_builtin(0, &out));
    try std.testing.expectEqual(ns.err_null_ptr, fsfmt.ra8_io_fsfmt_get_builtin(fsfmt.fs_type_exfat, null));
}

test "probe asks exFAT, then FAT, then registered formats" {
    try reset();
    var out: ?*const ns.Format = null;
    try std.testing.expectEqual(ns.err_not_found, fsfmt.ra8_io_fsfmt_probe(&backend, &out));
    fs.probe_type = fsfmt.fs_type_exfat;
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_probe(&backend, &out));
    try std.testing.expectEqual(try builtin(fsfmt.fs_type_exfat), out.?);
    fs.probe_type = fsfmt.fs_type_fat16;
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_probe(&backend, &out));
    try std.testing.expectEqual(try builtin(fsfmt.fs_type_fat16), out.?);
    fs.probe_type = fsfmt.fs_type_unknown;
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_register(&fake));
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_probe(&backend, &out));
    try std.testing.expectEqual(&fake, out.?);
    try std.testing.expectEqual(ns.err_null_ptr, fsfmt.ra8_io_fsfmt_probe(null, &out));
    try std.testing.expectEqual(ns.err_null_ptr, fsfmt.ra8_io_fsfmt_probe(&backend, null));
}

test "the registry holds eight formats" {
    try reset();
    for (2..fsfmt.max_formats) |_| try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_register(&fake));
    try std.testing.expectEqual(ns.err_no_mem, fsfmt.ra8_io_fsfmt_register(&fake));
}

fn expectRefused(rc: c_int, f: ns.Format) !void {
    try reset();
    try std.testing.expectEqual(rc, fsfmt.ra8_io_fsfmt_register(&f));
    try std.testing.expect(errors_logged > 0 or rc == ns.err_invalid_arg);
}

test "register refuses missing operations and capability mismatches" {
    try expectRefused(ns.err_null_ptr, .{ .ops = &base_ops });
    var ops = base_ops;
    ops.probe = null;
    try expectRefused(ns.err_null_ptr, .{ .name = "x", .ops = &ops });
    ops = base_ops;
    ops.listdir = null;
    try expectRefused(ns.err_null_ptr, .{ .name = "x", .ops = &ops });
    try expectRefused(ns.err_invalid_arg, .{ .name = "x", .ops = &base_ops, .caps = .{ .supports_streaming_write = true } });
    try expectRefused(ns.err_invalid_arg, .{ .name = "x", .ops = &base_ops, .caps = .{ .supports_mkdir = true } });
    try expectRefused(ns.err_invalid_arg, .{ .name = "x", .ops = &base_ops, .caps = .{ .atomic_rename = true } });
    try expectRefused(ns.err_invalid_arg, .{ .name = "x", .ops = &base_ops, .caps = .{ .supports_dir_cursor = true } });
    ops = base_ops;
    ops.sync = @ptrCast(&noop_op);
    try expectRefused(ns.err_invalid_arg, .{ .name = "x", .ops = &ops, .caps = .{ .durable_sync = true } });
    try reset();
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_register(&.{ .name = "x", .ops = &ops, .caps = .{ .supports_sync = true, .durable_sync = true } }));
    try std.testing.expectEqual(ns.err_null_ptr, fsfmt.ra8_io_fsfmt_register(null));
}

test "a directory cursor needs a power-of-two workspace alignment" {
    var ops = base_ops;
    ops.dir_open = (try builtin(fsfmt.fs_type_exfat)).ops.dir_open;
    ops.dir_next = (try builtin(fsfmt.fs_type_exfat)).ops.dir_next;
    ops.dir_close = (try builtin(fsfmt.fs_type_exfat)).ops.dir_close;
    const caps = ns.Caps{ .supports_dir_cursor = true, .directory_workspace_bytes = 64, .max_open_directories = 1, .directory_workspace_align = 3 };
    try expectRefused(ns.err_invalid_arg, .{ .name = "x", .ops = &ops, .caps = caps });
    var good = caps;
    good.directory_workspace_align = 4;
    try reset();
    try std.testing.expectEqual(ns.ok, fsfmt.ra8_io_fsfmt_register(&.{ .name = "x", .ops = &ops, .caps = good }));
}

test "the built-in listdir walks every entry and closes the cursor" {
    try reset();
    const ops = (try builtin(fsfmt.fs_type_fat32)).ops;
    fs.dir_entries = 3;
    seen = 0;
    seen_size = 0;
    try std.testing.expectEqual(ns.ok, ops.listdir.?(null, "/", &count_cb, null));
    try std.testing.expectEqual(@as(u32, 3), seen);
    try std.testing.expectEqual(@as(u64, 21), seen_size);
    try std.testing.expectEqual(@as(u32, 1), fs.dir_closes);
}

test "the built-in dir_open needs room for a whole cursor" {
    try reset();
    const ops = (try builtin(fsfmt.fs_type_exfat)).ops;
    var state: fsfmt.FsDir = .{};
    try std.testing.expectEqual(ns.err_no_mem, ops.dir_open.?(null, "/", &state, 64));
    try std.testing.expectEqual(ns.ok, ops.dir_open.?(null, "/", &state, @sizeOf(fsfmt.FsDir)));
    try std.testing.expect(state.is_open);
    try std.testing.expectEqual(ns.ok, ops.dir_close.?(&state));
    try std.testing.expect(!state.is_open);
    var mount: ?*anyopaque = null;
    const mount_fn: *align(1) const fn (*const anyopaque, *?*anyopaque) callconv(.c) c_int = @ptrCast(ops.mount.?);
    try std.testing.expectEqual(ns.err_not_supported, mount_fn(&backend, &mount));
}
