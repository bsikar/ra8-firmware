//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_log_attach/detach against fake ra8_log and ra8_io_stream entry
//! points (RA8FW-654).

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

export fn ra8_io_stream_write(_: *log.Stream, buf: [*]const u8, len: u32, _: ?*u32) c_int {
    for (buf[0..len]) |byte| {
        written[written_len] = byte;
        written_len += 1;
    }
    return write_status;
}

// The stream_ram unit in the same archive needs this to link; unused here.
export fn ra8_io_stream_bind(_: *log.Stream, _: *const anyopaque, _: ?*anyopaque) c_int {
    return log.ok;
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

const vtable: u8 = 0;

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
    var s = log.Stream{ .iface = &vtable, .ctx = null };
    try std.testing.expectEqual(log.ok, ra8_io_log_attach(&s));
    try std.testing.expect(installed_sink == log.sink);
    try std.testing.expect(installed_ctx == @as(?*anyopaque, &s));
}

test "the sink forwards each byte to the stream" {
    reset();
    var s = log.Stream{ .iface = &vtable, .ctx = null };
    _ = ra8_io_log_attach(&s);
    installed_sink.?(installed_ctx, 'o');
    installed_sink.?(installed_ctx, 'k');
    try std.testing.expectEqualSlices(u8, "ok", written[0..written_len]);
}

test "the sink ignores a stream write error" {
    reset();
    var s = log.Stream{ .iface = &vtable, .ctx = null };
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
    var s = log.Stream{ .iface = &vtable, .ctx = null };
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
