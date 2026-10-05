//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_stream_uart_init and its bound write/flush against fake ra8_sci,
//! ra8_log and ra8_io_stream entry points (RA8FW-699).

const std = @import("std");
const io = @import("ra8_io");
const uart = io.stream_uart;
const Stream = io.log.Stream;
const Iface = io.stream_ram.Iface;

const err_busy: c_int = 0x10B;

var errors_logged: u32 = 0;
var bound_iface: ?*const Iface = null;
var bound_ctx: ?*anyopaque = null;
var sci_status: c_int = 0;
var sci_channel: u8 = 0xFF;
var sci_len: u32 = 0;
var flushed_channel: u8 = 0xFF;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_sci_write_polling(channel: u8, _: [*]const u8, len: u32) c_int {
    sci_channel = channel;
    sci_len = len;
    return sci_status;
}

export fn ra8_sci_flush(channel: u8) c_int {
    flushed_channel = channel;
    return sci_status;
}

// The log unit in the same archive needs these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}

// The usbcdc unit in the same archive needs this to link; unused here.
export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
}

extern fn ra8_io_stream_uart_init(s: ?*Stream, state: ?*uart.State, channel: u8) c_int;

fn reset() void {
    errors_logged = 0;
    bound_iface = null;
    bound_ctx = null;
    sci_status = uart.ok;
    sci_channel = 0xFF;
    sci_len = 0;
    flushed_channel = 0xFF;
}

/// Reads back what the real ra8_io_stream_bind stored in the handle.
fn capture(s: *const Stream) void {
    bound_iface = @ptrCast(@alignCast(s.iface));
    bound_ctx = s.ctx;
}

test "init rejects each null argument and logs once per call" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: uart.State = undefined;
    try std.testing.expectEqual(uart.err_null_ptr, ra8_io_stream_uart_init(null, &st, 2));
    try std.testing.expectEqual(uart.err_null_ptr, ra8_io_stream_uart_init(&s, null, 2));
    capture(&s);
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
    try std.testing.expect(bound_iface == null);
}

test "init records the channel and binds the uart vtable" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: uart.State = undefined;
    try std.testing.expectEqual(uart.ok, ra8_io_stream_uart_init(&s, &st, 3));
    capture(&s);
    try std.testing.expectEqual(@as(u8, 3), st.channel);
    try std.testing.expect(bound_iface == &uart.iface);
    try std.testing.expect(bound_ctx == @as(?*anyopaque, &st));
}

test "write forwards to the SCI channel and publishes len" {
    reset();
    var st = uart.State{ .channel = 4 };
    var n: u32 = 0;
    try std.testing.expectEqual(uart.ok, uart.iface.write.?(&st, "hey", 3, &n));
    try std.testing.expectEqual(@as(u8, 4), sci_channel);
    try std.testing.expectEqual(@as(u32, 3), sci_len);
    try std.testing.expectEqual(@as(u32, 3), n);
    try std.testing.expectEqual(uart.ok, uart.iface.write.?(&st, "x", 1, null));
}

test "a failed SCI write passes its error through and publishes nothing" {
    reset();
    var st = uart.State{ .channel = 1 };
    sci_status = err_busy;
    var n: u32 = 77;
    try std.testing.expectEqual(err_busy, uart.iface.write.?(&st, "ab", 2, &n));
    try std.testing.expectEqual(@as(u32, 77), n);
}

test "write rejects a null context or buffer" {
    reset();
    var st = uart.State{ .channel = 0 };
    try std.testing.expectEqual(uart.err_null_ptr, uart.iface.write.?(null, "a", 1, null));
    try std.testing.expectEqual(uart.err_null_ptr, uart.iface.write.?(&st, null, 1, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
    try std.testing.expectEqual(@as(u8, 0xFF), sci_channel);
}

test "flush forwards to the SCI channel and returns its status" {
    reset();
    var st = uart.State{ .channel = 5 };
    try std.testing.expectEqual(uart.ok, uart.iface.flush.?(&st));
    try std.testing.expectEqual(@as(u8, 5), flushed_channel);
    sci_status = err_busy;
    try std.testing.expectEqual(err_busy, uart.iface.flush.?(&st));
}

test "flush rejects a null context" {
    reset();
    try std.testing.expectEqual(uart.err_null_ptr, uart.iface.flush.?(null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
    try std.testing.expectEqual(@as(u8, 0xFF), flushed_channel);
}

// The SDRAM block-device unit in the same archive needs these to link; unused here.
export fn ra8_sdramc_init() c_int {
    return 0;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}

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
