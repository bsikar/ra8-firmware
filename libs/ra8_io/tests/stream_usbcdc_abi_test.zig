//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_stream_usbcdc_init and its bound write against fake USB PAL,
//! ra8_log and ra8_io_stream entry points (RA8FW-700).

const std = @import("std");
const io = @import("ra8_io");
const cdc = io.stream_usbcdc;
const Stream = io.log.Stream;
const Iface = io.stream_ram.Iface;

const err_busy: c_int = 0x10B;

var errors_logged: u32 = 0;
var bound_iface: ?*const Iface = null;
var bound_ctx: ?*anyopaque = null;
var sends: u32 = 0;
var fail_on_send: u32 = 0;
var last_ep: u8 = 0xFF;
var sent_lens: [4]u16 = .{ 0, 0, 0, 0 };
var sent_ptrs: [4]usize = .{ 0, 0, 0, 0 };

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_usb_pal_ep_send(ep_addr: u8, data: [*]const u8, len: u16) c_int {
    sends += 1;
    if (sends == fail_on_send) return err_busy;
    last_ep = ep_addr;
    if (sends <= sent_lens.len) {
        sent_lens[sends - 1] = len;
        sent_ptrs[sends - 1] = @intFromPtr(data);
    }
    return cdc.ok;
}

// The other units in the same archive need these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return cdc.ok;
}
export fn ra8_sci_flush(_: u8) c_int {
    return cdc.ok;
}

extern fn ra8_io_stream_usbcdc_init(s: ?*Stream, state: ?*cdc.State, ep_addr: u8) c_int;

fn reset() void {
    errors_logged = 0;
    bound_iface = null;
    bound_ctx = null;
    sends = 0;
    fail_on_send = 0;
    last_ep = 0xFF;
    sent_lens = .{ 0, 0, 0, 0 };
    sent_ptrs = .{ 0, 0, 0, 0 };
}

/// Reads back what the real ra8_io_stream_bind stored in the handle.
fn capture(s: *const Stream) void {
    bound_iface = @ptrCast(@alignCast(s.iface));
    bound_ctx = s.ctx;
}

var big: [cdc.max_chunk + 10]u8 = undefined;

test "init rejects each null argument and logs once per call" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: cdc.State = undefined;
    try std.testing.expectEqual(cdc.err_null_ptr, ra8_io_stream_usbcdc_init(null, &st, 0x81));
    try std.testing.expectEqual(cdc.err_null_ptr, ra8_io_stream_usbcdc_init(&s, null, 0x81));
    capture(&s);
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
    try std.testing.expect(bound_iface == null);
}

test "init records the endpoint and binds a write-only vtable" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    var st: cdc.State = undefined;
    try std.testing.expectEqual(cdc.ok, ra8_io_stream_usbcdc_init(&s, &st, 0x82));
    capture(&s);
    try std.testing.expectEqual(@as(u8, 0x82), st.ep_addr);
    try std.testing.expect(bound_iface == &cdc.iface);
    try std.testing.expect(bound_ctx == @as(?*anyopaque, &st));
    try std.testing.expect(cdc.iface.flush == null);
}

test "a short write is one send and publishes len" {
    reset();
    var st = cdc.State{ .ep_addr = 0x81 };
    var n: u32 = 0;
    try std.testing.expectEqual(cdc.ok, cdc.iface.write.?(&st, "hello", 5, &n));
    try std.testing.expectEqual(@as(u32, 1), sends);
    try std.testing.expectEqual(@as(u8, 0x81), last_ep);
    try std.testing.expectEqual(@as(u16, 5), sent_lens[0]);
    try std.testing.expectEqual(@as(u32, 5), n);
}

test "a write past 65535 bytes splits into a full chunk and the rest" {
    reset();
    var st = cdc.State{ .ep_addr = 0x81 };
    var n: u32 = 0;
    try std.testing.expectEqual(cdc.ok, cdc.iface.write.?(&st, &big, big.len, &n));
    try std.testing.expectEqual(@as(u32, 2), sends);
    try std.testing.expectEqual(@as(u16, 65535), sent_lens[0]);
    try std.testing.expectEqual(@as(u16, 10), sent_lens[1]);
    try std.testing.expectEqual(@intFromPtr(&big) + cdc.max_chunk, sent_ptrs[1]);
    try std.testing.expectEqual(@as(u32, big.len), n);
}

test "a failed second chunk publishes the bytes already sent" {
    reset();
    var st = cdc.State{ .ep_addr = 0x81 };
    fail_on_send = 2;
    var n: u32 = 0;
    try std.testing.expectEqual(err_busy, cdc.iface.write.?(&st, &big, big.len, &n));
    try std.testing.expectEqual(cdc.max_chunk, n);
    fail_on_send = 3;
    try std.testing.expectEqual(err_busy, cdc.iface.write.?(&st, "a", 1, null));
}

test "a zero-length write sends nothing and publishes zero" {
    reset();
    var st = cdc.State{ .ep_addr = 0x81 };
    var n: u32 = 9;
    try std.testing.expectEqual(cdc.ok, cdc.iface.write.?(&st, "", 0, &n));
    try std.testing.expectEqual(@as(u32, 0), sends);
    try std.testing.expectEqual(@as(u32, 0), n);
}

test "write rejects a null context or buffer" {
    reset();
    var st = cdc.State{ .ep_addr = 0x81 };
    try std.testing.expectEqual(cdc.err_null_ptr, cdc.iface.write.?(null, "a", 1, null));
    try std.testing.expectEqual(cdc.err_null_ptr, cdc.iface.write.?(&st, null, 1, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
    try std.testing.expectEqual(@as(u32, 0), sends);
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

// The VFS mount table reaches the C format registry (ra8_io_fsfmt.c); unused here.
export fn ra8_io_fsfmt_get_builtin(_: u8, _: *?*const anyopaque) c_int {
    return 0x107;
}
export fn ra8_io_fsfmt_probe(_: *const anyopaque, _: *?*const anyopaque) c_int {
    return 0x107;
}

// The archive root also emits the MRAM block device (RA8FW-726).
export fn ra8_flash_extra_mram_write(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_flash_extra_mram_erase(_: u32) c_int {
    return 0;
}
