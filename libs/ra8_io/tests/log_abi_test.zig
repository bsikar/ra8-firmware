//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_log_attach/detach against fake ra8_log entry points and a capturing
//! sink behind the real ra8_io_stream_write (RA8FW-654).

const std = @import("std");
const io = @import("ra8_io");
const log = io.log;

var installed_sink: ?log.ByteSink = null;
var installed_ctx: ?*anyopaque = null;
var errors_logged: u32 = 0;
var written: [8]u8 = undefined;
var written_len: usize = 0;
var write_status: c_int = log.ok;

export fn ra8_log_set_byte_sink(sink: ?log.ByteSink, ctx: ?*anyopaque) void {
    installed_sink = sink;
    installed_ctx = ctx;
}

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

// The archive root also emits the SDRAM block-device unit; satisfy its imports.
export fn ra8_sdramc_init() c_int {
    return 0;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}

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

extern fn ra8_io_log_attach(s: ?*log.Stream) c_int;
extern fn ra8_io_log_detach() void;

/// Capturing sink bound through the real ra8_io_stream_write.
fn captureWrite(_: ?*anyopaque, buf: ?[*]const u8, len: u32, out_written: ?*u32) callconv(.c) c_int {
    for (buf.?[0..len]) |byte| {
        written[written_len] = byte;
        written_len += 1;
    }
    if (out_written) |out| out.* = len;
    return write_status;
}

const vtable = io.stream_ram.Iface{ .write = &captureWrite, .flush = null };
var sink_ctx: u8 = 0;

fn reset() void {
    installed_sink = null;
    installed_ctx = null;
    errors_logged = 0;
    written_len = 0;
    write_status = log.ok;
}

test "attach rejects a null stream and logs once" {
    reset();
    try std.testing.expectEqual(log.err_null_ptr, ra8_io_log_attach(null));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
    try std.testing.expect(installed_sink == null);
}

test "attach rejects an unbound stream" {
    reset();
    var s = log.Stream{ .iface = null, .ctx = null };
    try std.testing.expectEqual(log.err_not_initialized, ra8_io_log_attach(&s));
    try std.testing.expect(installed_sink == null);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "attach installs the sink with the stream as its context" {
    reset();
    var s = log.Stream{ .iface = &vtable, .ctx = &sink_ctx };
    try std.testing.expectEqual(log.ok, ra8_io_log_attach(&s));
    try std.testing.expect(installed_sink == log.sink);
    try std.testing.expect(installed_ctx == @as(?*anyopaque, &s));
}

test "the sink forwards each byte to the stream" {
    reset();
    var s = log.Stream{ .iface = &vtable, .ctx = &sink_ctx };
    _ = ra8_io_log_attach(&s);
    installed_sink.?(installed_ctx, 'o');
    installed_sink.?(installed_ctx, 'k');
    try std.testing.expectEqualSlices(u8, "ok", written[0..written_len]);
}

test "the sink ignores a stream write error" {
    reset();
    var s = log.Stream{ .iface = &vtable, .ctx = &sink_ctx };
    _ = ra8_io_log_attach(&s);
    write_status = err_io;
    installed_sink.?(installed_ctx, 'x');
    try std.testing.expectEqual(@as(usize, 1), written_len);
}

test "the sink drops a byte with no context" {
    reset();
    log.sink(null, 'x');
    try std.testing.expectEqual(@as(usize, 0), written_len);
}

test "detach clears the sink" {
    reset();
    var s = log.Stream{ .iface = &vtable, .ctx = &sink_ctx };
    _ = ra8_io_log_attach(&s);
    ra8_io_log_detach();
    try std.testing.expect(installed_sink == null);
    try std.testing.expect(installed_ctx == null);
}

const err_io: c_int = 0x10A;

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
