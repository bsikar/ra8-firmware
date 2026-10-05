//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_blockdev_ram_init and its vtable (RA8FW-722) over a static
//! eight-block buffer: argument checks, bounds, read-only rejection, erase
//! value and the reported caps.

const std = @import("std");
const io = @import("ra8_io");
const ram = io.blockdev_ram;
const log = io.log;

var errors_logged: u32 = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}

extern fn ra8_io_blockdev_ram_init(
    bd: ?*ram.Device,
    state: ?*ram.State,
    storage: ?[*]u8,
    block_count: u32,
    read_only: bool,
) c_int;

const blocks: u32 = 8;
var disk: [blocks * 512]u8 = undefined;
var device: ram.Device = .{ .iface = null, .ctx = null };
var state: ram.State = .{ .storage = null, .block_count = 0, .read_only = false };

fn setup(read_only: bool) !*const ram.Iface {
    errors_logged = 0;
    @memset(&disk, 0xA5);
    try std.testing.expectEqual(ram.ok, ra8_io_blockdev_ram_init(&device, &state, &disk, blocks, read_only));
    return device.iface.?;
}

test "init rejects null arguments with a log line each" {
    errors_logged = 0;
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_blockdev_ram_init(null, &state, &disk, blocks, false));
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_blockdev_ram_init(&device, null, &disk, blocks, false));
    try std.testing.expectEqual(ram.err_null_ptr, ra8_io_blockdev_ram_init(&device, &state, null, blocks, false));
    try std.testing.expectEqual(@as(u32, 3), errors_logged);
}

test "init rejects zero blocks without logging" {
    errors_logged = 0;
    try std.testing.expectEqual(ram.err_invalid_size, ra8_io_blockdev_ram_init(&device, &state, &disk, 0, false));
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "init binds the shared vtable and fills the state" {
    const iface = try setup(true);
    try std.testing.expect(iface == &ram.iface);
    try std.testing.expect(device.ctx == @as(?*anyopaque, @ptrCast(&state)));
    try std.testing.expectEqual(@intFromPtr(&disk), @intFromPtr(state.storage.?));
    try std.testing.expectEqual(blocks, state.block_count);
    try std.testing.expect(state.read_only);
    try std.testing.expect(iface.sync == null);
}

test "write then read round-trips one block" {
    const iface = try setup(false);
    var src: [512]u8 = undefined;
    for (&src, 0..) |*b, i| b.* = @truncate(i);
    try std.testing.expectEqual(ram.ok, iface.write.?(device.ctx, 3, 1, &src));
    try std.testing.expectEqualSlices(u8, &src, disk[3 * 512 .. 4 * 512]);
    var dst: [512]u8 = undefined;
    try std.testing.expectEqual(ram.ok, iface.read.?(device.ctx, 3, 1, &dst));
    try std.testing.expectEqualSlices(u8, &src, &dst);
}

test "bounds reject count past the device and lba past the end" {
    const iface = try setup(false);
    var buf: [512]u8 = undefined;
    try std.testing.expectEqual(ram.err_out_of_range, iface.read.?(device.ctx, 0, blocks + 1, &buf));
    try std.testing.expectEqual(ram.err_out_of_range, iface.read.?(device.ctx, blocks, 1, &buf));
    try std.testing.expectEqual(ram.err_out_of_range, iface.erase.?(device.ctx, 7, 2));
    try std.testing.expectEqual(ram.ok, iface.read.?(device.ctx, 7, 1, &buf));
}

test "erase fills with zero and leaves neighbours alone" {
    const iface = try setup(false);
    try std.testing.expectEqual(ram.ok, iface.erase.?(device.ctx, 2, 2));
    for (disk[2 * 512 .. 4 * 512]) |b| try std.testing.expectEqual(@as(u8, 0), b);
    try std.testing.expectEqual(@as(u8, 0xA5), disk[2 * 512 - 1]);
    try std.testing.expectEqual(@as(u8, 0xA5), disk[4 * 512]);
}

test "read-only device rejects write and erase but still reads" {
    const iface = try setup(true);
    var buf: [512]u8 = undefined;
    try std.testing.expectEqual(ram.err_not_supported, iface.write.?(device.ctx, 0, 1, &buf));
    try std.testing.expectEqual(ram.err_not_supported, iface.erase.?(device.ctx, 0, 1));
    try std.testing.expectEqual(ram.ok, iface.read.?(device.ctx, 0, 1, &buf));
    try std.testing.expectEqual(@as(u8, 0xA5), disk[0]);
}

test "null ctx, buffer and caps out log and return null_ptr" {
    const iface = try setup(false);
    var buf: [512]u8 = undefined;
    try std.testing.expectEqual(ram.err_null_ptr, iface.read.?(null, 0, 1, &buf));
    try std.testing.expectEqual(ram.err_null_ptr, iface.read.?(device.ctx, 0, 1, null));
    try std.testing.expectEqual(ram.err_null_ptr, iface.write.?(device.ctx, 0, 1, null));
    try std.testing.expectEqual(ram.err_null_ptr, iface.erase.?(null, 0, 1));
    try std.testing.expectEqual(ram.err_null_ptr, iface.get_caps.?(device.ctx, null));
    try std.testing.expectEqual(@as(u32, 5), errors_logged);
}

test "caps describe a zero-erase, 512-byte, no-erase-before-write device" {
    const iface = try setup(true);
    var caps: ram.Caps = undefined;
    try std.testing.expectEqual(ram.ok, iface.get_caps.?(device.ctx, &caps));
    try std.testing.expectEqual(blocks, caps.block_count);
    try std.testing.expectEqual(@as(u32, 1), caps.erase_unit_blocks);
    try std.testing.expectEqual(@as(u32, 512), caps.program_size_bytes);
    try std.testing.expectEqual(@as(u16, 512), caps.logical_block_bytes);
    try std.testing.expectEqual(@as(u8, 0), caps.erase_value);
    try std.testing.expect(!caps.must_erase_before_write);
    try std.testing.expect(caps.read_only);
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

// The archive root also emits the log unit; satisfy its imports.
export fn ra8_log_set_byte_sink(_: ?log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_io_stream_write(_: *log.Stream, _: [*]const u8, _: u32, _: ?*u32) c_int {
    return 0;
}

// The archive root emits every other unit too; satisfy their imports.
export fn ra8_sdramc_init() c_int {
    return 0;
}

// The stream_ram unit in the same archive needs this to link; unused here.
export fn ra8_io_stream_bind(_: *anyopaque, _: *const anyopaque, _: ?*anyopaque) c_int {
    return 0;
}

// The stream_uart unit in the same archive needs these to link; unused here.
export fn ra8_sci_write_polling(_: u8, _: [*]const u8, _: u32) c_int {
    return 0;
}

export fn ra8_sci_flush(_: u8) c_int {
    return 0;
}

// The stream_usbcdc unit in the same archive needs this to link; unused here.
export fn ra8_usb_pal_ep_send(_: u8, _: [*]const u8, _: u16) c_int {
    return 0;
}

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

// The stream_blockdev unit in the same archive needs this to link; unused here.
export fn ra8_io_blockdev_write(_: *const anyopaque, _: u32, _: u32, _: [*]const u8) c_int {
    return 0;
}
