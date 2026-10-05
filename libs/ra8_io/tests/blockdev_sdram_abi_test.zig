//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_blockdev_sdram_init against fake ra8_sdramc, RAM-backend and
//! ra8_log entry points (RA8FW-697). The window pointer is only compared,
//! never dereferenced.

const std = @import("std");
const io = @import("ra8_io");
const sdram = io.blockdev_sdram;
const log = io.log;

const err_io: c_int = 0x10A;

var sdramc_calls: u32 = 0;
var sdramc_status: c_int = sdram.ok;
var ram_calls: u32 = 0;
var ram_status: c_int = sdram.ok;
var ram_storage: usize = 0;
var ram_blocks: u32 = 0;
var ram_read_only: bool = true;
var errors_logged: u32 = 0;
var error_value: u32 = 0;

export fn ra8_sdramc_init() c_int {
    sdramc_calls += 1;
    return sdramc_status;
}

export fn ra8_io_blockdev_ram_init(_: *anyopaque, _: *anyopaque, storage: [*]u8, block_count: u32, read_only: bool) c_int {
    ram_calls += 1;
    ram_storage = @intFromPtr(storage);
    ram_blocks = block_count;
    ram_read_only = read_only;
    return ram_status;
}

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, value: u32) void {
    error_value = value;
}

// The archive root also emits the log unit; satisfy its imports.
export fn ra8_log_set_byte_sink(_: ?log.ByteSink, _: ?*anyopaque) void {}
export fn ra8_io_stream_write(_: *log.Stream, _: [*]const u8, _: u32, _: ?*u32) c_int {
    return 0;
}

extern fn ra8_io_blockdev_sdram_init(bd: ?*anyopaque, state: ?*anyopaque, block_count: u32) c_int;

var device: [16]u8 = undefined;
var state: [16]u8 = undefined;

fn reset() void {
    sdramc_calls = 0;
    sdramc_status = sdram.ok;
    ram_calls = 0;
    ram_status = sdram.ok;
    ram_storage = 0;
    ram_blocks = 0;
    ram_read_only = true;
    errors_logged = 0;
    error_value = 0;
}

fn expectRejectedEarly(expected: c_int, rc: c_int, logged: u32) !void {
    try std.testing.expectEqual(expected, rc);
    try std.testing.expectEqual(logged, errors_logged);
    try std.testing.expectEqual(@as(u32, 0), sdramc_calls);
    try std.testing.expectEqual(@as(u32, 0), ram_calls);
}

test "null bd logs and returns null_ptr before touching the controller" {
    reset();
    try expectRejectedEarly(sdram.err_null_ptr, ra8_io_blockdev_sdram_init(null, &state, 64), 1);
}

test "null state logs and returns null_ptr" {
    reset();
    try expectRejectedEarly(sdram.err_null_ptr, ra8_io_blockdev_sdram_init(&device, null, 64), 1);
}

test "zero blocks is invalid_size" {
    reset();
    try expectRejectedEarly(sdram.err_invalid_size, ra8_io_blockdev_sdram_init(&device, &state, 0), 0);
}

test "one block past the window is invalid_size" {
    reset();
    const rc = ra8_io_blockdev_sdram_init(&device, &state, sdram.max_blocks + 1);
    try expectRejectedEarly(sdram.err_invalid_size, rc, 0);
}

test "the whole window is accepted" {
    reset();
    try std.testing.expectEqual(sdram.ok, ra8_io_blockdev_sdram_init(&device, &state, 131072));
    try std.testing.expectEqual(@as(u32, 131072), ram_blocks);
}

test "controller failure is logged and returned, RAM backend untouched" {
    reset();
    sdramc_status = err_io;
    try std.testing.expectEqual(err_io, ra8_io_blockdev_sdram_init(&device, &state, 64));
    try std.testing.expectEqual(@as(u32, 1), errors_logged);
    try std.testing.expectEqual(@as(u32, 0x10A), error_value);
    try std.testing.expectEqual(@as(u32, 0), ram_calls);
}

test "success binds the window as a writable RAM device" {
    reset();
    try std.testing.expectEqual(sdram.ok, ra8_io_blockdev_sdram_init(&device, &state, 64));
    try std.testing.expectEqual(@as(u32, 1), sdramc_calls);
    try std.testing.expectEqual(@as(u32, 1), ram_calls);
    try std.testing.expectEqual(@as(usize, 0x68000000), ram_storage);
    try std.testing.expectEqual(@as(u32, 64), ram_blocks);
    try std.testing.expect(!ram_read_only);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "a RAM backend error comes back unchanged" {
    reset();
    ram_status = sdram.err_invalid_size;
    try std.testing.expectEqual(sdram.err_invalid_size, ra8_io_blockdev_sdram_init(&device, &state, 64));
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
