//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_i2c_bus_bind_i3c_compat and its trampolines against fake ra8_i3c_*
//! and ra8_log entry points (RA8FW-711).

const std = @import("std");
const io = @import("ra8_io");
const i3c = io.i2c_bus_i3c_compat;
const Bus = i3c.Bus;

const err_nack: c_int = 0x10C;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_channel: u8 = 0xFF;
var last_addr: u8 = 0;
var last_wr: ?[*]const u8 = null;
var last_wr_len: u32 = 0;
var last_rd: ?[*]u8 = null;
var last_rd_len: u32 = 0;
var last_restart: bool = false;
var result: c_int = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_i3c_write(channel: u8, addr: u8, data: ?[*]const u8, len: u32, restart: bool) c_int {
    calls += 1;
    last_channel = channel;
    last_addr = addr;
    last_wr = data;
    last_wr_len = len;
    last_restart = restart;
    return result;
}

export fn ra8_i3c_read(channel: u8, addr: u8, data: ?[*]u8, len: u32, restart: bool) c_int {
    calls += 1;
    last_channel = channel;
    last_addr = addr;
    last_rd = data;
    last_rd_len = len;
    last_restart = restart;
    return result;
}

export fn ra8_i3c_transfer(channel: u8, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) c_int {
    calls += 1;
    last_channel = channel;
    last_addr = addr;
    last_wr = wr;
    last_wr_len = wr_len;
    last_rd = rd;
    last_rd_len = rd_len;
    return result;
}

// The other units in the same archive need these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_io_stream_write(_: *io.log.Stream, _: [*]const u8, _: u32, _: ?*u32) c_int {
    return 0;
}
export fn ra8_io_stream_bind(_: *io.log.Stream, _: *const io.stream_ram.Iface, _: ?*anyopaque) c_int {
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

extern fn ra8_io_i2c_bus_bind_i3c_compat(bus: ?*Bus, channel: u8) c_int;

fn reset() void {
    errors_logged = 0;
    calls = 0;
    last_channel = 0xFF;
    result = 0;
}

fn bound(channel: u8) !Bus {
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(i3c.ok, ra8_io_i2c_bus_bind_i3c_compat(&b, channel));
    return b;
}

test "bind rejects a null bus and logs once" {
    reset();
    try std.testing.expectEqual(i3c.err_null_ptr, ra8_io_i2c_bus_bind_i3c_compat(null, 0));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "bind rejects channel 1 and up without touching the bus" {
    reset();
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(i3c.err_invalid_arg, ra8_io_i2c_bus_bind_i3c_compat(&b, i3c.channel_count));
    try std.testing.expectEqual(i3c.err_invalid_arg, ra8_io_i2c_bus_bind_i3c_compat(&b, 0xFF));
    try std.testing.expect(b.iface == null and b.ctx == null);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "bind fills the vtable and packs channel 0 into ctx" {
    reset();
    const b0 = try bound(0);
    try std.testing.expect(b0.iface == &i3c.iface);
    try std.testing.expectEqual(@as(u8, 0), i3c.channelOf(b0.ctx));
}

test "write inverts send_stop into the driver's restart flag" {
    reset();
    const b = try bound(0);
    const tx = [_]u8{ 0x81, 0x40 };
    try std.testing.expectEqual(i3c.ok, b.iface.?.write.?(b.ctx, 0x5D, &tx, 2, false));
    try std.testing.expectEqual(@as(u8, 0), last_channel);
    try std.testing.expectEqual(@as(u8, 0x5D), last_addr);
    try std.testing.expect(last_wr == @as(?[*]const u8, &tx));
    try std.testing.expectEqual(@as(u32, 2), last_wr_len);
    try std.testing.expect(last_restart);
    _ = b.iface.?.write.?(b.ctx, 0x5D, &tx, 1, true);
    try std.testing.expect(!last_restart);
}

test "read forwards address, buffer and length and never asks for a restart" {
    reset();
    const b = try bound(0);
    var rx = [_]u8{0} ** 6;
    last_restart = true;
    try std.testing.expectEqual(i3c.ok, b.iface.?.read.?(b.ctx, 0x36, &rx, 6));
    try std.testing.expect(!last_restart);
    try std.testing.expectEqual(@as(u8, 0), last_channel);
    try std.testing.expectEqual(@as(u8, 0x36), last_addr);
    try std.testing.expect(last_rd == @as(?[*]u8, &rx));
    try std.testing.expectEqual(@as(u32, 6), last_rd_len);
}

test "transfer forwards both phases" {
    reset();
    const b = try bound(0);
    const reg = [_]u8{0x0F};
    var rx = [_]u8{0} ** 2;
    try std.testing.expectEqual(i3c.ok, b.iface.?.transfer.?(b.ctx, 0x6A, &reg, 1, &rx, 2));
    try std.testing.expectEqual(@as(u8, 0), last_channel);
    try std.testing.expectEqual(@as(u8, 0x6A), last_addr);
    try std.testing.expect(last_wr == @as(?[*]const u8, &reg));
    try std.testing.expectEqual(@as(u32, 1), last_wr_len);
    try std.testing.expect(last_rd == @as(?[*]u8, &rx));
    try std.testing.expectEqual(@as(u32, 2), last_rd_len);
}

test "a driver error passes straight back through every trampoline" {
    reset();
    const b = try bound(0);
    result = err_nack;
    try std.testing.expectEqual(err_nack, b.iface.?.write.?(b.ctx, 0x50, null, 0, true));
    try std.testing.expectEqual(err_nack, b.iface.?.read.?(b.ctx, 0x50, null, 0));
    try std.testing.expectEqual(err_nack, b.iface.?.transfer.?(b.ctx, 0x50, null, 0, null, 0));
    try std.testing.expectEqual(@as(u32, 3), calls);
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
