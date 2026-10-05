//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_stream bind/write/flush and the formatted helpers against a
//! capturing sink (RA8FW-725).

const std = @import("std");
const io = @import("ra8_io");
const st = io.stream;
const Stream = io.log.Stream;
const Iface = io.stream_ram.Iface;

var errors_logged: u32 = 0;
var captured: [64]u8 = undefined;
var captured_len: u32 = 0;
var flushes: u32 = 0;
/// What the sink reports accepting, relative to len: null means all of it.
var accept_override: ?u32 = null;
var sink_status: c_int = st.ok;
var sink_ctx: u8 = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

fn sinkWrite(ctx: ?*anyopaque, buf: ?[*]const u8, len: u32, out_written: ?*u32) callconv(.c) c_int {
    std.debug.assert(ctx == @as(?*anyopaque, &sink_ctx));
    const accepted = accept_override orelse len;
    const kept = @min(accepted, len);
    @memcpy(captured[captured_len..][0..kept], buf.?[0..kept]);
    captured_len += kept;
    out_written.?.* = accepted;
    return sink_status;
}

fn sinkFlush(_: ?*anyopaque) callconv(.c) c_int {
    flushes += 1;
    return sink_status;
}

const full = Iface{ .write = &sinkWrite, .flush = &sinkFlush };
const no_flush = Iface{ .write = &sinkWrite, .flush = null };
const no_write = Iface{ .write = null, .flush = &sinkFlush };

extern fn ra8_io_stream_bind(stream: ?*Stream, iface: ?*const Iface, context: ?*anyopaque) c_int;
extern fn ra8_io_stream_write(s: ?*Stream, buf: ?[*]const u8, len: u32, out_written: ?*u32) c_int;
extern fn ra8_io_stream_flush(s: ?*Stream) c_int;
extern fn ra8_io_stream_putc(s: ?*Stream, c: u8) c_int;
extern fn ra8_io_stream_puts(s: ?*Stream, str: ?[*]const u8) c_int;
extern fn ra8_io_stream_put_u32(s: ?*Stream, value: u32) c_int;
extern fn ra8_io_stream_put_u64(s: ?*Stream, value: u64) c_int;
extern fn ra8_io_stream_put_hex(s: ?*Stream, value: u32, min_digits: u8) c_int;

fn reset() void {
    errors_logged = 0;
    captured_len = 0;
    flushes = 0;
    accept_override = null;
    sink_status = st.ok;
}

fn bound(iface: *const Iface) !Stream {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    try std.testing.expectEqual(st.ok, ra8_io_stream_bind(&s, iface, &sink_ctx));
    return s;
}

fn out() []const u8 {
    return captured[0..captured_len];
}

test "bind rejects null arguments and a vtable without write" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    try std.testing.expectEqual(st.err_null_ptr, ra8_io_stream_bind(null, &full, &sink_ctx));
    try std.testing.expectEqual(st.err_null_ptr, ra8_io_stream_bind(&s, null, &sink_ctx));
    try std.testing.expectEqual(st.err_null_ptr, ra8_io_stream_bind(&s, &full, null));
    try std.testing.expectEqual(st.err_invalid_arg, ra8_io_stream_bind(&s, &no_write, &sink_ctx));
    try std.testing.expect(s.iface == null);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "bind stores the vtable and context" {
    const s = try bound(&full);
    try std.testing.expect(s.iface == @as(?*const anyopaque, &full));
    try std.testing.expect(s.ctx == @as(?*anyopaque, &sink_ctx));
}

test "every entry point rejects a null or unbound handle" {
    reset();
    var s = Stream{ .iface = null, .ctx = null };
    const handles = [_]?*Stream{ null, &s };
    const want = [_]c_int{ st.err_null_ptr, st.err_not_initialized };
    for (handles, want) |h, w| {
        try std.testing.expectEqual(w, ra8_io_stream_write(h, "a", 1, null));
        try std.testing.expectEqual(w, ra8_io_stream_flush(h));
        try std.testing.expectEqual(w, ra8_io_stream_putc(h, 'a'));
        try std.testing.expectEqual(w, ra8_io_stream_puts(h, "a"));
        try std.testing.expectEqual(w, ra8_io_stream_put_u32(h, 1));
        try std.testing.expectEqual(w, ra8_io_stream_put_u64(h, 1));
        try std.testing.expectEqual(w, ra8_io_stream_put_hex(h, 1, 1));
    }
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "write forwards bytes and reports the accepted count" {
    var s = try bound(&full);
    var n: u32 = 99;
    try std.testing.expectEqual(st.ok, ra8_io_stream_write(&s, "abc", 3, &n));
    try std.testing.expectEqual(@as(u32, 3), n);
    try std.testing.expectEqualSlices(u8, "abc", out());
    try std.testing.expectEqual(st.ok, ra8_io_stream_write(&s, "d", 1, null));
}

test "write logs and rejects a null buffer" {
    var s = try bound(&full);
    try std.testing.expectEqual(st.err_null_ptr, ra8_io_stream_write(&s, null, 1, null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
}

test "a short accept is a protocol error that still publishes the count" {
    var s = try bound(&full);
    accept_override = 2;
    var n: u32 = 0;
    try std.testing.expectEqual(st.err_protocol_error, ra8_io_stream_write(&s, "abc", 3, &n));
    try std.testing.expectEqual(@as(u32, 2), n);
}

test "an over-accept is a protocol error and publishes nothing" {
    var s = try bound(&full);
    accept_override = 4;
    var n: u32 = 7;
    try std.testing.expectEqual(st.err_protocol_error, ra8_io_stream_write(&s, "abc", 3, &n));
    try std.testing.expectEqual(@as(u32, 7), n);
}

test "a sink error passes through with the accepted count" {
    var s = try bound(&full);
    sink_status = 0x10A;
    accept_override = 1;
    var n: u32 = 0;
    try std.testing.expectEqual(@as(c_int, 0x10A), ra8_io_stream_write(&s, "abc", 3, &n));
    try std.testing.expectEqual(@as(u32, 1), n);
}

test "flush calls the sink, or succeeds when it has none" {
    var s = try bound(&full);
    try std.testing.expectEqual(st.ok, ra8_io_stream_flush(&s));
    try std.testing.expectEqual(@as(u32, 1), flushes);
    var t = try bound(&no_flush);
    try std.testing.expectEqual(st.ok, ra8_io_stream_flush(&t));
    try std.testing.expectEqual(@as(u32, 0), flushes);
}

test "putc and puts write the bytes" {
    var s = try bound(&full);
    try std.testing.expectEqual(st.ok, ra8_io_stream_putc(&s, 'x'));
    try std.testing.expectEqual(st.ok, ra8_io_stream_puts(&s, "yz"));
    try std.testing.expectEqual(st.ok, ra8_io_stream_puts(&s, ""));
    try std.testing.expectEqualSlices(u8, "xyz", out());
}

test "puts logs a null string and rejects an unterminated one" {
    var s = try bound(&full);
    try std.testing.expectEqual(st.err_null_ptr, ra8_io_stream_puts(&s, null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
    const long = try std.testing.allocator.alloc(u8, st.puts_max + 1);
    defer std.testing.allocator.free(long);
    @memset(long, 'a');
    long[st.puts_max] = 0;
    try std.testing.expectEqual(st.err_invalid_size, ra8_io_stream_puts(&s, long.ptr));
}

test "put_u32 and put_u64 render decimal" {
    var s = try bound(&full);
    try std.testing.expectEqual(st.ok, ra8_io_stream_put_u32(&s, 0));
    try std.testing.expectEqual(st.ok, ra8_io_stream_putc(&s, ' '));
    try std.testing.expectEqual(st.ok, ra8_io_stream_put_u32(&s, std.math.maxInt(u32)));
    try std.testing.expectEqual(st.ok, ra8_io_stream_putc(&s, ' '));
    try std.testing.expectEqual(st.ok, ra8_io_stream_put_u64(&s, std.math.maxInt(u64)));
    try std.testing.expectEqualSlices(u8, "0 4294967295 18446744073709551615", out());
}

test "put_hex renders lowercase with zero padding" {
    var s = try bound(&full);
    try std.testing.expectEqual(st.ok, ra8_io_stream_put_hex(&s, 0xAB, 1));
    try std.testing.expectEqual(st.ok, ra8_io_stream_put_hex(&s, 0xAB, 4));
    try std.testing.expectEqual(st.ok, ra8_io_stream_put_hex(&s, 0xDEADBEEF, 8));
    try std.testing.expectEqualSlices(u8, "ab00abdeadbeef", out());
}

test "put_hex rejects zero and over-wide min_digits" {
    var s = try bound(&full);
    try std.testing.expectEqual(st.err_invalid_arg, ra8_io_stream_put_hex(&s, 1, 0));
    try std.testing.expectEqual(st.err_invalid_arg, ra8_io_stream_put_hex(&s, 1, 9));
    try std.testing.expectEqual(@as(u32, 0), captured_len);
}

// The log unit in the same archive needs these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
// The uart unit in the same archive needs these to link; unused here.
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}
// The usbcdc unit in the same archive needs this to link; unused here.
export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
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
