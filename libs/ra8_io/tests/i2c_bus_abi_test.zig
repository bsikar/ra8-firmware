//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_io I2C bus front end (RA8FW-714): validation, dispatch through a
//! bound backend, and the as_ops bridge, against a fake backend vtable.

const std = @import("std");
const io = @import("ra8_io");
const front = io.i2c_bus;
const Bus = front.Bus;
const Iface = front.Iface;
const Ops = front.Ops;

const err_nack: c_int = 0x10C;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_ctx: ?*anyopaque = null;
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

fn fakeWrite(ctx: ?*anyopaque, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) callconv(.c) c_int {
    calls += 1;
    last_ctx = ctx;
    last_addr = addr;
    last_wr = data;
    last_wr_len = len;
    last_stop = send_stop;
    return result;
}

fn fakeRead(ctx: ?*anyopaque, addr: u8, data: ?[*]u8, len: u32) callconv(.c) c_int {
    calls += 1;
    last_ctx = ctx;
    last_addr = addr;
    last_rd = data;
    last_rd_len = len;
    return result;
}

fn fakeTransfer(ctx: ?*anyopaque, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) callconv(.c) c_int {
    calls += 1;
    last_ctx = ctx;
    last_addr = addr;
    last_wr = wr;
    last_wr_len = wr_len;
    last_rd = rd;
    last_rd_len = rd_len;
    return result;
}

const fake = Iface{ .write = &fakeWrite, .read = &fakeRead, .transfer = &fakeTransfer };
const no_ops = Iface{ .write = null, .read = null, .transfer = null };
var cookie: u32 = 0xC0FFEE;

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

extern fn ra8_io_i2c_bus_write(bus: ?*const Bus, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) c_int;
extern fn ra8_io_i2c_bus_read(bus: ?*const Bus, addr: u8, data: ?[*]u8, len: u32) c_int;
extern fn ra8_io_i2c_bus_transfer(bus: ?*const Bus, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) c_int;
extern fn ra8_io_i2c_bus_as_ops(bus: ?*const Bus, out: ?*Ops) c_int;

fn reset() void {
    errors_logged = 0;
    calls = 0;
    last_ctx = null;
    result = 0;
}

fn boundBus() Bus {
    return .{ .iface = &fake, .ctx = &cookie };
}

test "a null bus is a null pointer and an unbound bus is not initialized" {
    reset();
    const unbound = Bus{ .iface = null, .ctx = null };
    var rx = [_]u8{0};
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_write(null, 0x50, null, 0, true));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_read(null, 0x50, &rx, 1));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_transfer(null, 0x50, null, 0, &rx, 1));
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_i2c_bus_write(&unbound, 0x50, null, 0, true));
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_i2c_bus_read(&unbound, 0x50, &rx, 1));
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_i2c_bus_transfer(&unbound, 0x50, null, 0, &rx, 1));
    try std.testing.expectEqual(@as(u32, 0), calls);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "a missing backend op is logged and reported as a null pointer" {
    reset();
    const b = Bus{ .iface = &no_ops, .ctx = &cookie };
    var rx = [_]u8{0};
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_write(&b, 0x50, null, 0, true));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_read(&b, 0x50, &rx, 1));
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_transfer(&b, 0x50, null, 0, &rx, 1));
    try std.testing.expectEqual(@as(u32, 3), errors_logged);
}

test "write, read and transfer dispatch to the backend with its ctx" {
    reset();
    const b = boundBus();
    const tx = [_]u8{ 0x0F, 0x01 };
    var rx = [_]u8{0} ** 4;
    try std.testing.expectEqual(front.ok, ra8_io_i2c_bus_write(&b, 0x5D, &tx, 2, false));
    try std.testing.expect(last_ctx == @as(?*anyopaque, &cookie));
    try std.testing.expectEqual(@as(u8, 0x5D), last_addr);
    try std.testing.expectEqual(@as(u32, 2), last_wr_len);
    try std.testing.expect(!last_stop);
    try std.testing.expectEqual(front.ok, ra8_io_i2c_bus_read(&b, 0x36, &rx, 4));
    try std.testing.expectEqual(@as(u8, 0x36), last_addr);
    try std.testing.expect(last_rd == @as(?[*]u8, &rx));
    try std.testing.expectEqual(@as(u32, 4), last_rd_len);
    try std.testing.expectEqual(front.ok, ra8_io_i2c_bus_transfer(&b, 0x6A, &tx, 1, &rx, 2));
    try std.testing.expectEqual(@as(u8, 0x6A), last_addr);
    try std.testing.expectEqual(@as(u32, 1), last_wr_len);
    try std.testing.expectEqual(@as(u32, 2), last_rd_len);
    try std.testing.expectEqual(@as(u32, 3), calls);
}

test "a backend error passes straight back" {
    reset();
    const b = boundBus();
    result = err_nack;
    try std.testing.expectEqual(err_nack, ra8_io_i2c_bus_write(&b, 0x50, null, 0, true));
    try std.testing.expectEqual(err_nack, ra8_io_i2c_bus_read(&b, 0x50, null, 0));
    try std.testing.expectEqual(err_nack, ra8_io_i2c_bus_transfer(&b, 0x50, null, 0, null, 0));
}

test "as_ops validates the bus first, then the out pointer" {
    reset();
    var ops: Ops = undefined;
    const unbound = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_as_ops(null, &ops));
    try std.testing.expectEqual(front.err_not_initialized, ra8_io_i2c_bus_as_ops(&unbound, &ops));
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
    const b = boundBus();
    try std.testing.expectEqual(front.err_null_ptr, ra8_io_i2c_bus_as_ops(&b, null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "as_ops trampolines forward through the bus to the backend" {
    reset();
    const b = boundBus();
    var ops: Ops = undefined;
    try std.testing.expectEqual(front.ok, ra8_io_i2c_bus_as_ops(&b, &ops));
    try std.testing.expect(ops.ctx == @as(?*anyopaque, @constCast(@ptrCast(&b))));
    const tx = [_]u8{0xAA};
    var rx = [_]u8{0} ** 2;
    try std.testing.expectEqual(front.ok, ops.write.?(ops.ctx, 0x48, &tx, 1, true));
    try std.testing.expect(last_stop);
    try std.testing.expect(last_ctx == @as(?*anyopaque, &cookie));
    try std.testing.expectEqual(front.ok, ops.read.?(ops.ctx, 0x48, &rx, 2));
    try std.testing.expectEqual(@as(u32, 2), last_rd_len);
    try std.testing.expectEqual(front.ok, ops.transfer.?(ops.ctx, 0x48, &tx, 1, &rx, 2));
    try std.testing.expectEqual(@as(u32, 3), calls);
}

test "as_ops trampolines reject a null ctx and log it" {
    reset();
    const b = boundBus();
    var ops: Ops = undefined;
    try std.testing.expectEqual(front.ok, ra8_io_i2c_bus_as_ops(&b, &ops));
    try std.testing.expectEqual(front.err_null_ptr, ops.write.?(null, 0x48, null, 0, true));
    try std.testing.expectEqual(front.err_null_ptr, ops.read.?(null, 0x48, null, 0));
    try std.testing.expectEqual(front.err_null_ptr, ops.transfer.?(null, 0x48, null, 0, null, 0));
    try std.testing.expectEqual(@as(u32, 3), errors_logged);
    try std.testing.expectEqual(@as(u32, 0), calls);
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
