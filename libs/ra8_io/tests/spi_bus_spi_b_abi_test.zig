//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_spi_bus_bind_spi_b and its trampolines against fake ra8_spi_*
//! and ra8_log entry points (RA8FW-701).

const std = @import("std");
const io = @import("ra8_io");
const spib = io.spi_bus_spi_b;
const Bus = spib.Bus;

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
export fn ra8_io_blockdev_ram_init(_: *anyopaque, _: *anyopaque, _: [*]u8, _: u32, _: bool) c_int {
    return 0;
}

extern fn ra8_io_spi_bus_bind_spi_b(bus: ?*Bus, channel: u8) c_int;

fn reset() void {
    errors_logged = 0;
    calls = 0;
    last_channel = 0xFF;
    result = 0;
}

fn bound(channel: u8) !Bus {
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(spib.ok, ra8_io_spi_bus_bind_spi_b(&b, channel));
    return b;
}

test "bind rejects a null bus and logs once" {
    reset();
    try std.testing.expectEqual(spib.err_null_ptr, ra8_io_spi_bus_bind_spi_b(null, 0));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "bind rejects channel 2 and up without touching the bus" {
    reset();
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(spib.err_invalid_arg, ra8_io_spi_bus_bind_spi_b(&b, spib.channel_count));
    try std.testing.expectEqual(spib.err_invalid_arg, ra8_io_spi_bus_bind_spi_b(&b, 0xFF));
    try std.testing.expect(b.iface == null and b.ctx == null);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "bind fills the vtable and packs the channel into ctx" {
    reset();
    const b0 = try bound(0);
    const b1 = try bound(1);
    try std.testing.expect(b0.iface == &spib.iface);
    try std.testing.expectEqual(@as(u8, 0), spib.channelOf(b0.ctx));
    try std.testing.expectEqual(@as(u8, 1), spib.channelOf(b1.ctx));
}

test "xfer8 forwards channel, byte and rx pointer" {
    reset();
    const b = try bound(1);
    var rx: u8 = 0;
    try std.testing.expectEqual(spib.ok, b.iface.?.xfer8.?(b.ctx, 0xA5, &rx));
    try std.testing.expectEqual(@as(u8, 1), last_channel);
    try std.testing.expectEqual(@as(u8, 0xA5), last_tx);
    try std.testing.expect(last_rx == &rx);
    _ = b.iface.?.xfer8.?(b.ctx, 0x5A, null);
    try std.testing.expect(last_rx == null);
}

test "write_read forwards buffers, length and frame width" {
    reset();
    const b = try bound(0);
    var tx = [_]u8{ 1, 2, 3, 4 };
    var rx = [_]u8{0} ** 4;
    try std.testing.expectEqual(spib.ok, b.iface.?.write_read.?(b.ctx, &tx, &rx, 2, 15));
    try std.testing.expectEqual(@as(u8, 0), last_channel);
    try std.testing.expect(last_tx_buf == @as(?*const anyopaque, &tx));
    try std.testing.expect(last_rx_buf == @as(?*anyopaque, &rx));
    try std.testing.expectEqual(@as(u32, 2), last_len);
    try std.testing.expectEqual(@as(u8, 15), last_width);
}

test "set_clock forwards baud and pclk" {
    reset();
    const b = try bound(1);
    try std.testing.expectEqual(spib.ok, b.iface.?.set_clock.?(b.ctx, 4_000_000, 120_000_000));
    try std.testing.expectEqual(@as(u8, 1), last_channel);
    try std.testing.expectEqual(@as(u32, 4_000_000), last_baud);
    try std.testing.expectEqual(@as(u32, 120_000_000), last_pclk);
}

test "a driver error passes straight back through every trampoline" {
    reset();
    const b = try bound(0);
    result = err_busy;
    try std.testing.expectEqual(err_busy, b.iface.?.xfer8.?(b.ctx, 0, null));
    try std.testing.expectEqual(err_busy, b.iface.?.write_read.?(b.ctx, null, null, 0, 7));
    try std.testing.expectEqual(err_busy, b.iface.?.set_clock.?(b.ctx, 1, 1));
    try std.testing.expectEqual(@as(u32, 3), calls);
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
