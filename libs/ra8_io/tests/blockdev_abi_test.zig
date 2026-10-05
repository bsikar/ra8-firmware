//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The block-device front end (RA8FW-723) against a scripted backend:
//! validation order, logged null arguments, optional erase and sync, and the
//! ra8_fs_backend_t adapters with their 32-bit LBA and zero-erase checks.

const std = @import("std");
const io = @import("ra8_io");
const bd = io.blockdev;
const log = io.log;

const err_io: c_int = 0x401;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_lba: u32 = 0;
var last_count: u32 = 0;
var op_status: c_int = bd.ok;
var caps_erase_value: u8 = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}

fn record(lba: u32, count: u32) c_int {
    calls += 1;
    last_lba = lba;
    last_count = count;
    return op_status;
}
fn opRead(_: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    buf.?[0] = 0xA5;
    return record(lba, count);
}
fn opWrite(_: ?*anyopaque, lba: u32, count: u32, _: ?[*]const u8) callconv(.c) c_int {
    return record(lba, count);
}
fn opErase(_: ?*anyopaque, lba: u32, count: u32) callconv(.c) c_int {
    return record(lba, count);
}
fn opCaps(_: ?*const anyopaque, out: ?*bd.Caps) callconv(.c) c_int {
    out.?.* = .{
        .block_count = 64,
        .erase_unit_blocks = 1,
        .program_size_bytes = 512,
        .logical_block_bytes = 512,
        .erase_value = caps_erase_value,
        .must_erase_before_write = false,
        .read_only = false,
    };
    return op_status;
}
fn opSync(_: ?*anyopaque) callconv(.c) c_int {
    calls += 1;
    return op_status;
}

const full: bd.Iface = .{ .read = opRead, .write = opWrite, .erase = opErase, .get_caps = opCaps, .sync = opSync };
const bare: bd.Iface = .{ .read = null, .write = null, .erase = null, .get_caps = null, .sync = null };

extern fn ra8_io_blockdev_read(d: ?*const bd.Device, lba: u32, count: u32, buf: ?[*]u8) c_int;
extern fn ra8_io_blockdev_write(d: ?*const bd.Device, lba: u32, count: u32, buf: ?[*]const u8) c_int;
extern fn ra8_io_blockdev_erase(d: ?*const bd.Device, lba: u32, count: u32) c_int;
extern fn ra8_io_blockdev_get_caps(d: ?*const bd.Device, out: ?*bd.Caps) c_int;
extern fn ra8_io_blockdev_sync(d: ?*const bd.Device) c_int;
extern fn ra8_io_blockdev_as_fs_backend(d: ?*const bd.Device, out: ?*bd.FsBackend) c_int;

fn reset() void {
    errors_logged = 0;
    calls = 0;
    last_lba = 0;
    last_count = 0;
    op_status = bd.ok;
    caps_erase_value = 0;
}

var full_dev: bd.Device = .{ .iface = &full, .ctx = null };
var bare_dev: bd.Device = .{ .iface = &bare, .ctx = null };
var unbound: bd.Device = .{ .iface = null, .ctx = null };
var sector: [512]u8 = undefined;

test "null and unbound devices are rejected without logging" {
    reset();
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_read(null, 0, 1, &sector));
    try std.testing.expectEqual(bd.err_not_initialized, ra8_io_blockdev_write(&unbound, 0, 1, &sector));
    try std.testing.expectEqual(bd.err_not_initialized, ra8_io_blockdev_erase(&unbound, 0, 1));
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_sync(null));
    try std.testing.expectEqual(bd.err_not_initialized, ra8_io_blockdev_as_fs_backend(&unbound, null));
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "read, write and erase dispatch with lba, count and status" {
    reset();
    op_status = err_io;
    try std.testing.expectEqual(err_io, ra8_io_blockdev_read(&full_dev, 7, 2, &sector));
    try std.testing.expectEqual(@as(u8, 0xA5), sector[0]);
    try std.testing.expectEqual(err_io, ra8_io_blockdev_write(&full_dev, 9, 3, &sector));
    try std.testing.expectEqual(err_io, ra8_io_blockdev_erase(&full_dev, 11, 4));
    try std.testing.expectEqual(@as(u32, 3), calls);
    try std.testing.expectEqual(@as(u32, 11), last_lba);
    try std.testing.expectEqual(@as(u32, 4), last_count);
}

test "null buffers, caps out and missing ops log and return null_ptr" {
    reset();
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_read(&full_dev, 0, 1, null));
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_write(&full_dev, 0, 1, null));
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_get_caps(&full_dev, null));
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_read(&bare_dev, 0, 1, &sector));
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_write(&bare_dev, 0, 1, &sector));
    var caps: bd.Caps = undefined;
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_get_caps(&bare_dev, &caps));
    try std.testing.expectEqual(@as(u32, 6), errors_logged);
    try std.testing.expectEqual(@as(u32, 0), calls);
}

test "missing erase is not_supported and missing sync is ok" {
    reset();
    try std.testing.expectEqual(bd.err_not_supported, ra8_io_blockdev_erase(&bare_dev, 0, 1));
    try std.testing.expectEqual(bd.ok, ra8_io_blockdev_sync(&bare_dev));
    op_status = err_io;
    try std.testing.expectEqual(err_io, ra8_io_blockdev_sync(&full_dev));
    try std.testing.expectEqual(@as(u32, 1), calls);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

fn adapter() !bd.FsBackend {
    var fs: bd.FsBackend = undefined;
    try std.testing.expectEqual(bd.ok, ra8_io_blockdev_as_fs_backend(&full_dev, &fs));
    try std.testing.expect(fs.ctx == @as(?*anyopaque, @ptrCast(&full_dev)));
    return fs;
}

test "as_fs_backend logs a null out" {
    reset();
    try std.testing.expectEqual(bd.err_null_ptr, ra8_io_blockdev_as_fs_backend(&full_dev, null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "fs read and write forward 32-bit lbas and reject larger ones" {
    reset();
    const fs = try adapter();
    try std.testing.expectEqual(bd.ok, fs.read_block.?(fs.ctx, 5, 1, &sector));
    try std.testing.expectEqual(bd.ok, fs.write_block.?(fs.ctx, 0xFFFF_FFFF, 1, &sector));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), last_lba);
    try std.testing.expectEqual(bd.err_out_of_range, fs.read_block.?(fs.ctx, 1 << 32, 1, &sector));
    try std.testing.expectEqual(bd.err_out_of_range, fs.write_block.?(fs.ctx, 1 << 32, 1, &sector));
    try std.testing.expectEqual(@as(u32, 2), calls);
    try std.testing.expectEqual(bd.err_null_ptr, fs.read_block.?(null, 0, 1, &sector));
    try std.testing.expectEqual(bd.err_null_ptr, fs.write_block.?(fs.ctx, 0, 1, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "fs capacity reports block count and size or the caps error" {
    reset();
    const fs = try adapter();
    var n: u64 = 0;
    var size: u32 = 0;
    try std.testing.expectEqual(bd.ok, fs.get_capacity.?(fs.ctx, &n, &size));
    try std.testing.expectEqual(@as(u64, 64), n);
    try std.testing.expectEqual(@as(u32, 512), size);
    op_status = err_io;
    try std.testing.expectEqual(err_io, fs.get_capacity.?(fs.ctx, &n, &size));
    try std.testing.expectEqual(bd.err_null_ptr, fs.get_capacity.?(fs.ctx, null, &size));
    try std.testing.expectEqual(bd.err_null_ptr, fs.get_capacity.?(fs.ctx, &n, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "fs erase needs a zero-erase device and 32-bit ranges" {
    reset();
    const fs = try adapter();
    try std.testing.expectEqual(bd.ok, fs.erase_blocks.?(fs.ctx, 3, 2));
    try std.testing.expectEqual(@as(u32, 3), last_lba);
    try std.testing.expectEqual(@as(u32, 2), last_count);
    try std.testing.expectEqual(bd.err_out_of_range, fs.erase_blocks.?(fs.ctx, 1 << 32, 1));
    try std.testing.expectEqual(bd.err_out_of_range, fs.erase_blocks.?(fs.ctx, 0, 1 << 32));
    caps_erase_value = 0xFF;
    try std.testing.expectEqual(bd.err_not_supported, fs.erase_blocks.?(fs.ctx, 0, 1));
    try std.testing.expectEqual(@as(u32, 1), calls);
}

// The archive root emits every ra8_io unit; these satisfy the others' externs.
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

// The archive root also emits the log unit; satisfy its imports.
export fn ra8_log_set_byte_sink(_: ?log.ByteSink, _: ?*anyopaque) void {}

// The archive root emits every other unit too; satisfy their imports.
export fn ra8_sdramc_init() c_int {
    return 0;
}

// The stream_uart unit in the same archive needs these to link; unused here.
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}

export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}

// The stream_usbcdc unit in the same archive needs this to link; unused here.
export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
}

// The spi_b unit in the same archive needs these to link; unused here.
export fn ra8_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}

export fn ra8_spi_write_read(_: u8, _: ?*const anyopaque, _: ?*anyopaque, _: u32, _: u8) c_int {
    return 0;
}

export fn ra8_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}

// The sci_spi unit in the same archive needs these to link; unused here.
export fn ra8_sci_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}

export fn ra8_sci_spi_xfer(_: u8, _: ?[*]const u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_sci_spi_set_clock(_: u8, _: u32, _: u32) c_int {
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

export fn ra8_sdcard_read_blocks(_: u32, _: [*]u8, _: u32) c_int {
    return 0;
}

export fn ra8_sdcard_write_blocks(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}

export fn ra8_sdcard_get_capacity(_: *u32) c_int {
    return 0;
}

// The blockdev_usbmsc unit in the same archive needs these to link; unused here.
export fn ra8_usb_hmsc_read10(_: u8, _: u32, _: u16, _: ?[*]u8) c_int {
    return 0;
}

export fn ra8_usb_hmsc_write10(_: u8, _: u32, _: u16, _: ?[*]const u8) c_int {
    return 0;
}

export fn ra8_usb_hmsc_read_capacity(_: u8, _: *u32, _: *u32) c_int {
    return 0;
}

// The format registry forwards to ra8_fs; these stubs stand in for it.
comptime {
    _ = @import("fs_stubs.zig");
}

// The archive root also emits the MRAM block device (RA8FW-726).
export fn ra8_flash_extra_mram_write(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_flash_extra_mram_erase(_: u32) c_int {
    return 0;
}
