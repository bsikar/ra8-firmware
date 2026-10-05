//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_spi_bus_bind_sci_spi and its trampolines against fake
//! ra8_sci_spi_* and ra8_log entry points (RA8FW-703).

const std = @import("std");
const io = @import("ra8_io");
const sci = io.spi_bus_sci_spi;
const Bus = sci.Bus;

const err_busy: c_int = 0x10B;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_channel: u8 = 0xFF;
var last_tx: u8 = 0;
var last_rx: ?*u8 = null;
var last_tx_buf: ?[*]const u8 = null;
var last_rx_buf: ?[*]u8 = null;
var last_len: u32 = 0;
var last_baud: u32 = 0;
var last_pclk: u32 = 0;
var result: c_int = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_sci_spi_xfer8(channel: u8, tx: u8, rx: ?*u8) c_int {
    calls += 1;
    last_channel = channel;
    last_tx = tx;
    last_rx = rx;
    return result;
}

export fn ra8_sci_spi_xfer(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32) c_int {
    calls += 1;
    last_channel = channel;
    last_tx_buf = tx;
    last_rx_buf = rx;
    last_len = len;
    return result;
}

export fn ra8_sci_spi_set_clock(channel: u8, baud_hz: u32, pclk_hz: u32) c_int {
    calls += 1;
    last_channel = channel;
    last_baud = baud_hz;
    last_pclk = pclk_hz;
    return result;
}

// The other units in the same archive need these to link; unused here.
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}
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
export fn ra8_sdramc_init() c_int {
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

extern fn ra8_io_spi_bus_bind_sci_spi(bus: ?*Bus, channel: u8) c_int;

fn reset() void {
    errors_logged = 0;
    calls = 0;
    last_channel = 0xFF;
    result = 0;
}

fn bound(channel: u8) !Bus {
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(sci.ok, ra8_io_spi_bus_bind_sci_spi(&b, channel));
    return b;
}

test "bind rejects a null bus and logs once" {
    reset();
    try std.testing.expectEqual(sci.err_null_ptr, ra8_io_spi_bus_bind_sci_spi(null, 0));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "bind rejects channel 10 and up without touching the bus" {
    reset();
    var b = Bus{ .iface = null, .ctx = null };
    try std.testing.expectEqual(sci.err_invalid_arg, ra8_io_spi_bus_bind_sci_spi(&b, sci.channel_count));
    try std.testing.expect(b.iface == null and b.ctx == null);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "bind fills the SCI vtable and packs the channel into ctx" {
    reset();
    const b = try bound(9);
    try std.testing.expect(b.iface == &sci.iface);
    try std.testing.expect(b.iface != &io.spi_bus_spi_b.iface);
    try std.testing.expectEqual(@as(u8, 9), io.spi_bus_spi_b.channelOf(b.ctx));
}

test "xfer8 forwards channel, byte and rx pointer" {
    reset();
    const b = try bound(3);
    var rx: u8 = 0;
    try std.testing.expectEqual(sci.ok, b.iface.?.xfer8.?(b.ctx, 0xC3, &rx));
    try std.testing.expectEqual(@as(u8, 3), last_channel);
    try std.testing.expectEqual(@as(u8, 0xC3), last_tx);
    try std.testing.expect(last_rx == &rx);
}

test "an 8-bit write_read forwards buffers and length" {
    reset();
    const b = try bound(2);
    var tx = [_]u8{ 1, 2, 3 };
    var rx = [_]u8{0} ** 3;
    try std.testing.expectEqual(sci.ok, b.iface.?.write_read.?(b.ctx, &tx, &rx, 3, sci.width_8));
    try std.testing.expectEqual(@as(u8, 2), last_channel);
    try std.testing.expect(last_tx_buf.? == @as([*]const u8, &tx));
    try std.testing.expect(last_rx_buf.? == @as([*]u8, &rx));
    try std.testing.expectEqual(@as(u32, 3), last_len);
}

test "16- and 32-bit frames are not supported and never reach the driver" {
    reset();
    const b = try bound(0);
    try std.testing.expectEqual(sci.err_not_supported, b.iface.?.write_read.?(b.ctx, null, null, 4, 15));
    try std.testing.expectEqual(sci.err_not_supported, b.iface.?.write_read.?(b.ctx, null, null, 4, 31));
    try std.testing.expectEqual(@as(u32, 0), calls);
}

test "set_clock forwards and a driver error passes straight back" {
    reset();
    const b = try bound(5);
    try std.testing.expectEqual(sci.ok, b.iface.?.set_clock.?(b.ctx, 1_000_000, 100_000_000));
    try std.testing.expectEqual(@as(u8, 5), last_channel);
    try std.testing.expectEqual(@as(u32, 1_000_000), last_baud);
    try std.testing.expectEqual(@as(u32, 100_000_000), last_pclk);
    result = err_busy;
    try std.testing.expectEqual(err_busy, b.iface.?.xfer8.?(b.ctx, 0, null));
    try std.testing.expectEqual(err_busy, b.iface.?.write_read.?(b.ctx, null, null, 0, sci.width_8));
    try std.testing.expectEqual(err_busy, b.iface.?.set_clock.?(b.ctx, 1, 1));
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

// The archive root also emits the MRAM block device (RA8FW-726).
export fn ra8_flash_extra_mram_write(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_flash_extra_mram_erase(_: u32) c_int {
    return 0;
}
