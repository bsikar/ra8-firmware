//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_blockdev_sdhi_init and its vtable against fake ra8_sdcard and
//! ra8_log entry points (RA8FW-715).

const std = @import("std");
const io = @import("ra8_io");
const sdhi = io.blockdev_sdhi;
const log = io.log;

const err_io: c_int = 0x10A;

var read_calls: u32 = 0;
var write_calls: u32 = 0;
var cap_calls: u32 = 0;
var last_lba: u32 = 0;
var last_count: u32 = 0;
var last_buf: usize = 0;
var io_status: c_int = sdhi.ok;
var capacity: u32 = 0;
var errors_logged: u32 = 0;

export fn ra8_sdcard_read_blocks(lba: u32, buf: [*]u8, count: u32) c_int {
    read_calls += 1;
    last_lba = lba;
    last_count = count;
    last_buf = @intFromPtr(buf);
    return io_status;
}

export fn ra8_sdcard_write_blocks(lba: u32, buf: [*]const u8, count: u32) c_int {
    write_calls += 1;
    last_lba = lba;
    last_count = count;
    last_buf = @intFromPtr(buf);
    return io_status;
}

export fn ra8_sdcard_get_capacity(out_blocks: *u32) c_int {
    cap_calls += 1;
    if (io_status == sdhi.ok) out_blocks.* = capacity;
    return io_status;
}

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

// The archive root also emits the log unit; satisfy its imports.
export fn ra8_log_set_byte_sink(_: ?log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_io_stream_write(_: *log.Stream, _: [*]const u8, _: u32, _: ?*u32) c_int {
    return 0;
}

// The archive root emits every other unit too; satisfy their imports.
export fn ra8_sdramc_init() c_int {
    return 0;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}

// The stream_ram unit in the same archive needs this to link; unused here.
export fn ra8_io_stream_bind(_: *anyopaque, _: *const anyopaque, _: ?*anyopaque) c_int {
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

extern fn ra8_io_blockdev_sdhi_init(bd: ?*sdhi.Device) c_int;

var block: [512]u8 = undefined;

fn reset() void {
    read_calls = 0;
    write_calls = 0;
    cap_calls = 0;
    last_lba = 0;
    last_count = 0;
    last_buf = 0;
    io_status = sdhi.ok;
    capacity = 0;
    errors_logged = 0;
}

fn bound() !*const sdhi.Iface {
    var device: sdhi.Device = .{ .iface = null, .ctx = @ptrFromInt(0x1000) };
    try std.testing.expectEqual(sdhi.ok, ra8_io_blockdev_sdhi_init(&device));
    try std.testing.expectEqual(@as(?*anyopaque, null), device.ctx);
    return device.iface.?;
}

test "null bd logs and returns null_ptr" {
    reset();
    try std.testing.expectEqual(sdhi.err_null_ptr, ra8_io_blockdev_sdhi_init(null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "init binds the vtable with no erase and no sync" {
    reset();
    const vt = try bound();
    try std.testing.expect(vt.read != null and vt.write != null and vt.get_caps != null);
    try std.testing.expect(vt.erase == null and vt.sync == null);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "read forwards lba, buffer and count and passes the status through" {
    reset();
    const vt = try bound();
    try std.testing.expectEqual(sdhi.ok, vt.read.?(null, 7, 3, &block));
    try std.testing.expectEqual(@as(u32, 7), last_lba);
    try std.testing.expectEqual(@as(u32, 3), last_count);
    try std.testing.expectEqual(@intFromPtr(&block), last_buf);
    io_status = err_io;
    try std.testing.expectEqual(err_io, vt.read.?(null, 0, 1, &block));
}

test "write forwards lba, buffer and count and passes the status through" {
    reset();
    const vt = try bound();
    try std.testing.expectEqual(sdhi.ok, vt.write.?(null, 9, 2, &block));
    try std.testing.expectEqual(@as(u32, 1), write_calls);
    try std.testing.expectEqual(@as(u32, 9), last_lba);
    try std.testing.expectEqual(@as(u32, 2), last_count);
    io_status = err_io;
    try std.testing.expectEqual(err_io, vt.write.?(null, 0, 1, &block));
}

test "null buffers log and never reach the card" {
    reset();
    const vt = try bound();
    try std.testing.expectEqual(sdhi.err_null_ptr, vt.read.?(null, 0, 1, null));
    try std.testing.expectEqual(sdhi.err_null_ptr, vt.write.?(null, 0, 1, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
    try std.testing.expectEqual(@as(u32, 0), read_calls + write_calls);
}

test "caps report the card capacity as 512-byte, zero-erase, writable media" {
    reset();
    const vt = try bound();
    capacity = 0x00EE_0000;
    var caps: sdhi.Caps = undefined;
    try std.testing.expectEqual(sdhi.ok, vt.get_caps.?(null, &caps));
    try std.testing.expectEqual(@as(u32, 0x00EE_0000), caps.block_count);
    try std.testing.expectEqual(@as(u32, 1), caps.erase_unit_blocks);
    try std.testing.expectEqual(@as(u32, 512), caps.program_size_bytes);
    try std.testing.expectEqual(@as(u16, 512), caps.logical_block_bytes);
    try std.testing.expectEqual(@as(u8, 0), caps.erase_value);
    try std.testing.expect(!caps.must_erase_before_write and !caps.read_only);
}

test "caps pass a capacity failure through and reject a null out" {
    reset();
    const vt = try bound();
    var caps: sdhi.Caps = std.mem.zeroes(sdhi.Caps);
    io_status = err_io;
    try std.testing.expectEqual(err_io, vt.get_caps.?(null, &caps));
    try std.testing.expectEqual(@as(u32, 0), caps.block_count);
    try std.testing.expectEqual(sdhi.err_null_ptr, vt.get_caps.?(null, null));
    try std.testing.expectEqual(@as(u32, 1), cap_calls);
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

// The stream_blockdev unit in the same archive needs this to link; unused here.
export fn ra8_io_blockdev_write(_: *const anyopaque, _: u32, _: u32, _: [*]const u8) c_int {
    return 0;
}

// The archive root also emits the SPI-mode SD block device (RA8FW-718).
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

// ra8_io_vfs_namespace_abi.zig reaches these in ra8_io_vfs.c; unused here.
export fn priv_ra8_io_vfs_streq(_: [*:0]const u8, _: [*:0]const u8) bool {
    return false;
}
export fn priv_ra8_io_vfs_find(_: [*:0]const u8, _: ?*u8) ?*io.vfs_namespace.Slot {
    return null;
}
export fn priv_ra8_io_vfs_split(_: [*:0]const u8, _: [*]u8, _: *?[*:0]const u8) c_int {
    return 0;
}
export fn priv_ra8_io_vfs_resolve(_: [*:0]const u8, _: *?*io.vfs_namespace.Slot, _: ?*u8, _: *?[*:0]const u8) c_int {
    return 0;
}
