//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The MRAM block device (RA8FW-726): init's window checks against the real
//! extra-MRAM range, and the vtable over a host buffer standing in for MRAM
//! with the flash program and erase calls captured.

const std = @import("std");
const io = @import("ra8_io");
const mram = io.blockdev_mram;
const log = io.log;

const err_io: c_int = 0x401;

var errors_logged: u32 = 0;
var programs: u32 = 0;
var erases: u32 = 0;
var last_addr: u32 = 0;
var last_len: u32 = 0;
var fail_on: u32 = 0;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}
export fn ra8_flash_extra_mram_write(addr: u32, _: [*]const u8, len: u32) c_int {
    programs += 1;
    last_addr = addr;
    last_len = len;
    return if (programs == fail_on) err_io else 0;
}
export fn ra8_flash_extra_mram_erase(addr: u32) c_int {
    erases += 1;
    last_addr = addr;
    return if (erases == fail_on) err_io else 0;
}

fn reset() void {
    errors_logged = 0;
    programs = 0;
    erases = 0;
    last_addr = 0;
    last_len = 0;
    fail_on = 0;
}

const blocks: u32 = 4;
var backing: [blocks * 512]u8 align(32) = undefined;
var sector: [1024]u8 = undefined;

/// A hand-built state over host memory; init only accepts the real window.
fn hostState(read_only: bool) mram.State {
    return .{ .base = @intFromPtr(&backing), .block_count = blocks, .read_only = read_only };
}

test "state matches the C layout" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(mram.State, "base"));
    try std.testing.expectEqual(@sizeOf(usize), @offsetOf(mram.State, "block_count"));
    try std.testing.expectEqual(@sizeOf(usize) + 4, @offsetOf(mram.State, "read_only"));
}

test "init logs null bd and state and rejects zero blocks" {
    reset();
    var bd: mram.Device = .{ .iface = null, .ctx = null };
    var st: mram.State = undefined;
    try std.testing.expectEqual(mram.err_null_ptr, mram.ra8_io_blockdev_mram_init(null, &st, mram.extra_start, 1, false));
    try std.testing.expectEqual(mram.err_null_ptr, mram.ra8_io_blockdev_mram_init(&bd, null, mram.extra_start, 1, false));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
    try std.testing.expectEqual(mram.err_invalid_arg, mram.ra8_io_blockdev_mram_init(&bd, &st, mram.extra_start, 0, false));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "init enforces alignment and the extra-MRAM window" {
    const start = mram.extra_start;
    const end = start + mram.extra_size;
    try std.testing.expectEqual(mram.err_invalid_arg, mram.windowOk(start + 16, 1));
    try std.testing.expectEqual(mram.err_invalid_arg, mram.windowOk(start - 32, 1));
    try std.testing.expectEqual(mram.err_invalid_arg, mram.windowOk(end, 1));
    try std.testing.expectEqual(mram.err_invalid_arg, mram.windowOk(end - 256, 1));
    try std.testing.expectEqual(mram.ok, mram.windowOk(end - 512, 1));
    try std.testing.expectEqual(mram.ok, mram.windowOk(start, @intCast(mram.extra_size / 512)));
}

test "init binds the vtable and fills the state" {
    reset();
    var bd: mram.Device = .{ .iface = null, .ctx = null };
    var st: mram.State = undefined;
    try std.testing.expectEqual(mram.ok, mram.ra8_io_blockdev_mram_init(&bd, &st, mram.extra_start + 512, 3, true));
    try std.testing.expect(bd.iface == &mram.iface);
    try std.testing.expect(bd.ctx == @as(?*anyopaque, @ptrCast(&st)));
    try std.testing.expectEqual(mram.extra_start + 512, st.base);
    try std.testing.expectEqual(@as(u32, 3), st.block_count);
    try std.testing.expect(st.read_only);
    try std.testing.expect(mram.iface.sync == null);
}

test "read copies memory-mapped blocks and checks bounds" {
    reset();
    var st = hostState(false);
    for (&backing, 0..) |*b, i| b.* = @truncate(i);
    try std.testing.expectEqual(mram.ok, mram.iface.read.?(&st, 1, 2, &sector));
    try std.testing.expectEqualSlices(u8, backing[512..1536], sector[0..1024]);
    try std.testing.expectEqual(mram.err_out_of_range, mram.iface.read.?(&st, 3, 2, &sector));
    try std.testing.expectEqual(mram.err_out_of_range, mram.iface.read.?(&st, 0, blocks + 1, &sector));
}

test "write programs 32-byte units from the window address" {
    reset();
    var st = hostState(false);
    try std.testing.expectEqual(mram.ok, mram.iface.write.?(&st, 2, 1, &sector));
    try std.testing.expectEqual(@as(u32, 16), programs);
    try std.testing.expectEqual(@as(u32, 32), last_len);
    const expect: u32 = @truncate(st.base + 2 * 512 + 480);
    try std.testing.expectEqual(expect, last_addr);
}

test "erase walks 32-byte blocks and stops at the first error" {
    reset();
    var st = hostState(false);
    try std.testing.expectEqual(mram.ok, mram.iface.erase.?(&st, 0, 2));
    try std.testing.expectEqual(@as(u32, 32), erases);
    reset();
    fail_on = 3;
    try std.testing.expectEqual(err_io, mram.iface.erase.?(&st, 0, 1));
    try std.testing.expectEqual(@as(u32, 3), erases);
    reset();
    fail_on = 2;
    try std.testing.expectEqual(err_io, mram.iface.write.?(&st, 0, 1, &sector));
    try std.testing.expectEqual(@as(u32, 2), programs);
}

test "read-only rejects write and erase before touching flash" {
    reset();
    var st = hostState(true);
    try std.testing.expectEqual(mram.err_not_supported, mram.iface.write.?(&st, 0, 1, &sector));
    try std.testing.expectEqual(mram.err_not_supported, mram.iface.erase.?(&st, 0, 1));
    try std.testing.expectEqual(mram.err_out_of_range, mram.iface.write.?(&hostStateRw, blocks, 1, &sector));
    try std.testing.expectEqual(@as(u32, 0), programs + erases);
}
var hostStateRw: mram.State = .{ .base = 0, .block_count = blocks, .read_only = false };

test "null arguments log and caps describe a 0xFF erase-before-write device" {
    reset();
    var st = hostState(true);
    try std.testing.expectEqual(mram.err_null_ptr, mram.iface.read.?(null, 0, 1, &sector));
    try std.testing.expectEqual(mram.err_null_ptr, mram.iface.write.?(&st, 0, 1, null));
    try std.testing.expectEqual(mram.err_null_ptr, mram.iface.erase.?(null, 0, 1));
    try std.testing.expectEqual(mram.err_null_ptr, mram.iface.get_caps.?(&st, null));
    try std.testing.expectEqual(@as(u32, 4), errors_logged);
    var caps: mram.Caps = undefined;
    try std.testing.expectEqual(mram.ok, mram.iface.get_caps.?(&st, &caps));
    try std.testing.expectEqual(blocks, caps.block_count);
    try std.testing.expectEqual(@as(u32, 1), caps.erase_unit_blocks);
    try std.testing.expectEqual(@as(u32, 32), caps.program_size_bytes);
    try std.testing.expectEqual(@as(u16, 512), caps.logical_block_bytes);
    try std.testing.expectEqual(@as(u8, 0xFF), caps.erase_value);
    try std.testing.expect(caps.must_erase_before_write);
    try std.testing.expect(caps.read_only);
}

// The archive root emits every ra8_io unit; these satisfy the others' externs.
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

// The archive root emits every other unit too; satisfy their imports.
export fn ra8_sdramc_init() c_int {
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
