//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The stream-over-block-device sink (RA8FW-720) against a fake
//! ra8_io_blockdev_write: init validation, sector gathering across writes,
//! a write error mid-stream, and zero-padded flush.

const std = @import("std");
const io = @import("ra8_io");
const sbd = io.stream_blockdev;
const State = sbd.State;
const Stream = io.log.Stream;
const Iface = io.stream_ram.Iface;

const err_io: c_int = 0x401;

var errors_logged: u32 = 0;
var bound_iface: ?*const Iface = null;
var bound_ctx: ?*anyopaque = null;
var writes: u32 = 0;
var last_lba: u32 = 0;
var last_sector: [512]u8 = undefined;
var fail_on_write: u32 = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}
export fn ra8_io_stream_bind(_: *Stream, iface: *const Iface, context: ?*anyopaque) c_int {
    bound_iface = iface;
    bound_ctx = context;
    return 0;
}
export fn ra8_io_blockdev_write(_: *const anyopaque, lba: u32, count: u32, buf: [*]const u8) c_int {
    writes += 1;
    if (fail_on_write == writes) return err_io;
    std.debug.assert(count == 1);
    last_lba = lba;
    @memcpy(&last_sector, buf[0..512]);
    return 0;
}
export fn ra8_usb_hmsc_read10(_: u8, _: u32, _: u16, _: ?[*]u8) c_int {
    return 0;
}
export fn ra8_usb_hmsc_write10(_: u8, _: u32, _: u16, _: ?[*]const u8) c_int {
    return 0;
}
export fn ra8_usb_hmsc_read_capacity(_: u8, _: *u32, _: *u32) c_int {
    return 0;
}

// The other units in the same archive need these to link; unused here.
// The other units in the same archive need these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_io_stream_write(_: *io.log.Stream, _: [*]const u8, _: u32, _: ?*u32) c_int {
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

var fake_bd: u32 = 0;

fn reset() void {
    errors_logged = 0;
    bound_iface = null;
    bound_ctx = null;
    writes = 0;
    last_lba = 0;
    fail_on_write = 0;
}

fn bindFresh(st: *State, s: *Stream, lba: u32) !void {
    reset();
    try std.testing.expectEqual(sbd.ok, sbd.ra8_io_stream_blockdev_init(s, st, &fake_bd, lba));
}

fn write(buf: []const u8, out: ?*u32) c_int {
    return bound_iface.?.write.?(bound_ctx, buf.ptr, @intCast(buf.len), out);
}

test "state matches the C layout" {
    try std.testing.expectEqual(@sizeOf(usize) + 8 + 512, @sizeOf(State));
    try std.testing.expectEqual(@sizeOf(usize), @offsetOf(State, "lba"));
    try std.testing.expectEqual(@sizeOf(usize) + 8, @offsetOf(State, "sector"));
}

test "init rejects null arguments and logs" {
    reset();
    var st: State = undefined;
    var s: Stream = .{ .iface = null, .ctx = null };
    try std.testing.expectEqual(sbd.err_null_ptr, sbd.ra8_io_stream_blockdev_init(null, &st, &fake_bd, 0));
    try std.testing.expectEqual(sbd.err_null_ptr, sbd.ra8_io_stream_blockdev_init(&s, null, &fake_bd, 0));
    try std.testing.expectEqual(sbd.err_null_ptr, sbd.ra8_io_stream_blockdev_init(&s, &st, null, 0));
    try std.testing.expectEqual(@as(u32, 3), errors_logged);
    try std.testing.expect(bound_iface == null);
}

test "init records the device and start LBA and binds the sink" {
    var st: State = undefined;
    st.fill = 77;
    var s: Stream = .{ .iface = null, .ctx = null };
    try bindFresh(&st, &s, 40);
    try std.testing.expectEqual(@as(u32, 40), st.lba);
    try std.testing.expectEqual(@as(u32, 0), st.fill);
    try std.testing.expect(bound_ctx == @as(?*anyopaque, @ptrCast(&st)));
    try std.testing.expect(bound_iface.?.flush != null);
}

test "bytes gather into sectors across writes" {
    var st: State = undefined;
    var s: Stream = .{ .iface = null, .ctx = null };
    try bindFresh(&st, &s, 7);
    var a: [300]u8 = undefined;
    @memset(&a, 0xAA);
    var out: u32 = 0;
    try std.testing.expectEqual(sbd.ok, write(&a, &out));
    try std.testing.expectEqual(@as(u32, 300), out);
    try std.testing.expectEqual(@as(u32, 0), writes);
    var b: [300]u8 = undefined;
    @memset(&b, 0xBB);
    try std.testing.expectEqual(sbd.ok, write(&b, null));
    try std.testing.expectEqual(@as(u32, 1), writes);
    try std.testing.expectEqual(@as(u32, 7), last_lba);
    try std.testing.expectEqual(@as(u8, 0xAA), last_sector[299]);
    try std.testing.expectEqual(@as(u8, 0xBB), last_sector[300]);
    try std.testing.expectEqual(@as(u32, 8), st.lba);
    try std.testing.expectEqual(@as(u32, 88), st.fill);
}

test "a device error mid-write reports the bytes taken so far" {
    var st: State = undefined;
    var s: Stream = .{ .iface = null, .ctx = null };
    try bindFresh(&st, &s, 0);
    fail_on_write = 2;
    var buf: [1200]u8 = undefined;
    @memset(&buf, 1);
    var out: u32 = 0;
    try std.testing.expectEqual(err_io, write(&buf, &out));
    try std.testing.expectEqual(@as(u32, 1024), out);
    try std.testing.expectEqual(@as(u32, 1), st.lba);
    try std.testing.expectEqual(@as(u32, 512), st.fill);
}

test "flush pads the partial sector with zeros and is a no-op when empty" {
    var st: State = undefined;
    var s: Stream = .{ .iface = null, .ctx = null };
    try bindFresh(&st, &s, 3);
    const flush = bound_iface.?.flush.?;
    try std.testing.expectEqual(sbd.ok, flush(bound_ctx));
    try std.testing.expectEqual(@as(u32, 0), writes);
    @memset(&st.sector, 0xFF);
    try std.testing.expectEqual(sbd.ok, write("hi", null));
    try std.testing.expectEqual(sbd.ok, flush(bound_ctx));
    try std.testing.expectEqual(@as(u32, 1), writes);
    try std.testing.expectEqual(@as(u32, 3), last_lba);
    try std.testing.expectEqualSlices(u8, "hi", last_sector[0..2]);
    try std.testing.expectEqual(@as(u8, 0), last_sector[2]);
    try std.testing.expectEqual(@as(u8, 0), last_sector[511]);
    try std.testing.expectEqual(@as(u32, 0), st.fill);
}

test "null ctx or buffer logs and returns null_ptr" {
    reset();
    const it = sbd.iface;
    var b = [_]u8{1};
    var st: State = undefined;
    try std.testing.expectEqual(sbd.err_null_ptr, it.write.?(null, &b, 1, null));
    try std.testing.expectEqual(sbd.err_null_ptr, it.write.?(&st, null, 1, null));
    try std.testing.expectEqual(sbd.err_null_ptr, it.flush.?(null));
    try std.testing.expectEqual(@as(u32, 3), errors_logged);
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
