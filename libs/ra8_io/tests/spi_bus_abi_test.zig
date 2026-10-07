//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_io SPI bus front end (RA8FW-707): validation, dispatch through a
//! bound backend, and the as_ops bridge, against a fake backend vtable.

const std = @import("std");
const io = @import("ra8_io");
const front = io.spi_bus;
const Bus = front.Bus;
const Iface = front.Iface;

const err_busy: c_int = 0x10B;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_channel: u8 = 0xFF;
var last_tx: u8 = 0;
var last_rx: ?*u8 = null;
var last_tx_buf: ?*const anyopaque = null;
var last_rx_buf: ?*anyopaque = null;
var last_len: u32 = 0;
var last_width: u8 = 0;
var last_baud: u32 = 0;
var last_pclk: u32 = 0;
var result: c_int = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_spi_xfer8(channel: u8, tx: u8, rx: ?*u8) c_int {
    calls += 1;
    last_channel = channel;
    last_tx = tx;
    last_rx = rx;
    return result;
}

export fn ra8_spi_write_read(channel: u8, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) c_int {
    calls += 1;
    last_channel = channel;
    last_tx_buf = tx;
    last_rx_buf = rx;
    last_len = len;
    last_width = width;
    return result;
}

export fn ra8_spi_set_clock(channel: u8, baud_hz: u32, pclka_hz: u32) c_int {
    calls += 1;
    last_channel = channel;
    last_baud = baud_hz;
    last_pclk = pclka_hz;
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

// The SCI Simple-SPI unit in the same archive needs these to link.
export fn ra8_sci_spi_xfer8(_: u8, _: u8, _: ?*u8) c_int {
    return 0;
}
export fn ra8_sci_spi_xfer(_: u8, _: ?[*]const u8, _: ?[*]u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_spi_set_clock(_: u8, _: u32, _: u32) c_int {
    return 0;
}

extern fn ra8_io_spi_bus_xfer8(bus: ?*const Bus, tx: u8, rx: ?*u8) c_int;
extern fn ra8_io_spi_bus_write_read(bus: ?*const Bus, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) c_int;
extern fn ra8_io_spi_bus_set_clock(bus: ?*const Bus, baud_hz: u32, pclk_hz: u32) c_int;
extern fn ra8_io_spi_bus_as_ops(bus: ?*const Bus, out: ?*front.Ops) c_int;

var backend_ctx: ?*anyopaque = null;

fn fakeXfer8(ctx: ?*anyopaque, tx: u8, rx: ?*u8) callconv(.c) c_int {
    calls += 1;
    backend_ctx = ctx;
    last_tx = tx;
    last_rx = rx;
    return result;
}

fn fakeWriteRead(ctx: ?*anyopaque, tx: ?*const anyopaque, rx: ?*anyopaque, len: u32, width: u8) callconv(.c) c_int {
    calls += 1;
    backend_ctx = ctx;
    last_tx_buf = tx;
    last_rx_buf = rx;
    last_len = len;
    last_width = width;
    return result;
}

fn fakeSetClock(ctx: ?*anyopaque, baud_hz: u32, pclk_hz: u32) callconv(.c) c_int {
    calls += 1;
    backend_ctx = ctx;
    last_baud = baud_hz;
    last_pclk = pclk_hz;
    return result;
}

const full = Iface{ .xfer8 = &fakeXfer8, .write_read = &fakeWriteRead, .set_clock = &fakeSetClock };
const empty = Iface{ .xfer8 = null, .write_read = null, .set_clock = null };
const cookie: *anyopaque = @ptrFromInt(0x1234);

fn reset() void {
    errors_logged = 0;
    calls = 0;
    backend_ctx = null;
    result = 0;
}

test "a null bus is a null pointer on every entry point, unlogged" {
    reset();
    var ops: front.Ops = undefined;
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_xfer8(null, 0, null));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_write_read(null, null, null, 0, 7));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_set_clock(null, 1, 2));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_as_ops(null, &ops));
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "an unbound bus reports not initialized on every entry point" {
    reset();
    const b = Bus{ .iface = null, .ctx = cookie };
    var ops: front.Ops = undefined;
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_spi_bus_xfer8(&b, 0, null));
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_spi_bus_write_read(&b, null, null, 0, 7));
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_spi_bus_set_clock(&b, 1, 2));
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_spi_bus_as_ops(&b, &ops));
    try std.testing.expectEqual(@as(u32, 0), calls);
}

test "a missing backend op logs once and reports a null pointer" {
    reset();
    const b = Bus{ .iface = &empty, .ctx = cookie };
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_xfer8(&b, 0, null));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_write_read(&b, null, null, 0, 7));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_set_clock(&b, 1, 2));
    try std.testing.expectEqual(@as(u32, 3), errors_logged);
}

test "xfer8 hands ctx, byte and rx pointer to the backend" {
    reset();
    const b = Bus{ .iface = &full, .ctx = cookie };
    var rx: u8 = 0;
    try std.testing.expectEqual(front.ok, ra8_io_spi_bus_xfer8(&b, 0xA5, &rx));
    try std.testing.expectEqual(@as(u32, 1), calls);
    try std.testing.expectEqual(@as(?*anyopaque, cookie), backend_ctx);
    try std.testing.expectEqual(@as(u8, 0xA5), last_tx);
    try std.testing.expectEqual(@as(?*u8, &rx), last_rx);
}

test "write_read and set_clock forward every argument" {
    reset();
    const b = Bus{ .iface = &full, .ctx = cookie };
    var tx = [_]u8{ 1, 2, 3 };
    var rx = [_]u8{ 0, 0, 0 };
    try std.testing.expectEqual(front.ok, ra8_io_spi_bus_write_read(&b, &tx, &rx, 3, 15));
    try std.testing.expectEqual(@as(?*const anyopaque, &tx), last_tx_buf);
    try std.testing.expectEqual(@as(?*anyopaque, &rx), last_rx_buf);
    try std.testing.expectEqual(@as(u32, 3), last_len);
    try std.testing.expectEqual(@as(u8, 15), last_width);
    try std.testing.expectEqual(front.ok, ra8_io_spi_bus_set_clock(&b, 1_000_000, 100_000_000));
    try std.testing.expectEqual(@as(u32, 1_000_000), last_baud);
    try std.testing.expectEqual(@as(u32, 100_000_000), last_pclk);
    try std.testing.expectEqual(@as(?*anyopaque, cookie), backend_ctx);
}

test "a backend error passes straight back" {
    reset();
    result = err_busy;
    const b = Bus{ .iface = &full, .ctx = cookie };
    try std.testing.expectEqual(err_busy, ra8_io_spi_bus_xfer8(&b, 0, null));
    try std.testing.expectEqual(err_busy, ra8_io_spi_bus_write_read(&b, null, null, 1, 7));
    try std.testing.expectEqual(err_busy, ra8_io_spi_bus_set_clock(&b, 1, 2));
}

test "as_ops rejects a null out, then bridges xfer8 through the bus" {
    reset();
    const b = Bus{ .iface = &full, .ctx = cookie };
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_spi_bus_as_ops(&b, null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
    var ops = front.Ops{ .xfer8 = null, .ctx = null };
    try std.testing.expectEqual(front.ok, ra8_io_spi_bus_as_ops(&b, &ops));
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(@constCast(&b))), ops.ctx);
    var rx: u8 = 0;
    try std.testing.expectEqual(front.ok, ops.xfer8.?(ops.ctx, 0x3C, &rx));
    try std.testing.expectEqual(@as(?*anyopaque, cookie), backend_ctx);
    try std.testing.expectEqual(@as(u8, 0x3C), last_tx);
    try std.testing.expectEqual(front.err_null_ptr, ops.xfer8.?(null, 0, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
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
