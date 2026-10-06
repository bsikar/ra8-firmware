//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_i2c_bus_bind_riic and its trampolines against fake ra8_i2c_*
//! and ra8_log entry points (RA8FW-709).

const std = @import("std");
const io = @import("ra8_io");
const riic = io.i2c_bus_riic;
const Bus = riic.Bus;

const err_nack: c_int = 0x10C;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_channel: u8 = 0xFF;
var last_addr: u8 = 0;
var last_wr: ?[*]const u8 = null;
var last_wr_len: u32 = 0;
var last_rd: ?[*]u8 = null;
var last_rd_len: u32 = 0;
var last_stop: bool = false;
var result: c_int = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_i2c_write(channel: u8, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) c_int {
    calls += 1;
    last_channel = channel;
    last_addr = addr;
    last_wr = data;
    last_wr_len = len;
    last_stop = send_stop;
    return result;
}

export fn ra8_i2c_read(channel: u8, addr: u8, data: ?[*]u8, len: u32) c_int {
    calls += 1;
    last_channel = channel;
    last_addr = addr;
    last_rd = data;
    last_rd_len = len;
    return result;
}

export fn ra8_i2c_transfer(channel: u8, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) c_int {
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

extern fn ra8_io_i2c_bus_bind_riic(bus: ?*Bus, channel: u8) c_int;

fn reset() void {
    errors_logged = 0;
    calls = 0;
    last_channel = 0xFF;
    result = 0;
}

fn bound(channel: u8) !Bus {
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(riic.ok, ra8_io_i2c_bus_bind_riic(&b, channel));
    return b;
}

test "bind rejects a null bus and logs once" {
    reset();
    try std.testing.expectEqual(riic.err_null_ptr, ra8_io_i2c_bus_bind_riic(null, 0));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "bind rejects channel 3 and up without touching the bus" {
    reset();
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(riic.err_invalid_arg, ra8_io_i2c_bus_bind_riic(&b, riic.channel_count));
    try std.testing.expectEqual(riic.err_invalid_arg, ra8_io_i2c_bus_bind_riic(&b, 0xFF));
    try std.testing.expect(b.iface == null and b.ctx == null);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "bind fills the vtable and packs the channel into ctx" {
    reset();
    const b0 = try bound(0);
    const b2 = try bound(2);
    try std.testing.expect(b0.iface == &riic.iface);
    try std.testing.expectEqual(@as(u8, 0), riic.channelOf(b0.ctx));
    try std.testing.expectEqual(@as(u8, 2), riic.channelOf(b2.ctx));
}

test "write forwards channel, address, payload and the stop flag" {
    reset();
    const b = try bound(1);
    const tx = [_]u8{ 0x10, 0x20, 0x30 };
    try std.testing.expectEqual(riic.ok, b.iface.?.write.?(b.ctx, 0x5D, &tx, 3, false));
    try std.testing.expectEqual(@as(u8, 1), last_channel);
    try std.testing.expectEqual(@as(u8, 0x5D), last_addr);
    try std.testing.expect(last_wr == @as(?[*]const u8, &tx));
    try std.testing.expectEqual(@as(u32, 3), last_wr_len);
    try std.testing.expect(!last_stop);
    _ = b.iface.?.write.?(b.ctx, 0x5D, &tx, 1, true);
    try std.testing.expect(last_stop);
}

test "read forwards channel, address, buffer and length" {
    reset();
    const b = try bound(2);
    var rx: [6]u8 = @splat(0);
    try std.testing.expectEqual(riic.ok, b.iface.?.read.?(b.ctx, 0x36, &rx, 6));
    try std.testing.expectEqual(@as(u8, 2), last_channel);
    try std.testing.expectEqual(@as(u8, 0x36), last_addr);
    try std.testing.expect(last_rd == @as(?[*]u8, &rx));
    try std.testing.expectEqual(@as(u32, 6), last_rd_len);
}

test "transfer forwards both phases" {
    reset();
    const b = try bound(0);
    const reg = [_]u8{0x0F};
    var rx: [2]u8 = @splat(0);
    try std.testing.expectEqual(riic.ok, b.iface.?.transfer.?(b.ctx, 0x6A, &reg, 1, &rx, 2));
    try std.testing.expectEqual(@as(u8, 0), last_channel);
    try std.testing.expectEqual(@as(u8, 0x6A), last_addr);
    try std.testing.expect(last_wr == @as(?[*]const u8, &reg));
    try std.testing.expectEqual(@as(u32, 1), last_wr_len);
    try std.testing.expect(last_rd == @as(?[*]u8, &rx));
    try std.testing.expectEqual(@as(u32, 2), last_rd_len);
}

test "a driver error passes straight back through every trampoline" {
    reset();
    const b = try bound(1);
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
