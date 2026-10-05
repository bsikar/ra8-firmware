//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The USB MSC block device (RA8FW-717) against a fake ra8_usb_hmsc driver:
//! init validation, the transfer ceiling, LUN routing, and READ CAPACITY.

const std = @import("std");
const io = @import("ra8_io");
const usbmsc = io.blockdev_usbmsc;
const Bd = usbmsc.Bd;
const Caps = usbmsc.Caps;
const State = usbmsc.State;

const err_timeout: c_int = 0x10A;

var errors_logged: u32 = 0;
var calls: u32 = 0;
var last_lun: u8 = 0xFF;
var last_lba: u32 = 0;
var last_count: u16 = 0;
var result: c_int = 0;
var capacity_blocks: u32 = 0;
var capacity_block_size: u32 = 512;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}

export fn ra8_usb_hmsc_read10(lun: u8, lba: u32, count: u16, _: ?[*]u8) c_int {
    calls += 1;
    last_lun = lun;
    last_lba = lba;
    last_count = count;
    return result;
}

export fn ra8_usb_hmsc_write10(lun: u8, lba: u32, count: u16, _: ?[*]const u8) c_int {
    calls += 1;
    last_lun = lun;
    last_lba = lba;
    last_count = count;
    return result;
}

export fn ra8_usb_hmsc_read_capacity(lun: u8, block_count: *u32, block_size: *u32) c_int {
    calls += 1;
    last_lun = lun;
    if (result != 0) return result;
    block_count.* = capacity_blocks;
    block_size.* = capacity_block_size;
    return 0;
}

// The other units in the same archive need these to link; unused here.
export fn ra8_log_set_byte_sink(_: ?io.log.ByteSink, _: ?*anyopaque) void {}
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

extern fn ra8_io_blockdev_usbmsc_init(bd: ?*Bd, state: ?*State, lun: u8) c_int;

fn reset() void {
    errors_logged = 0;
    calls = 0;
    last_lun = 0xFF;
    result = 0;
    capacity_blocks = 0;
    capacity_block_size = 512;
}

fn bound(bd: *Bd, st: *State, lun: u8) !void {
    try std.testing.expectEqual(usbmsc.ok, ra8_io_blockdev_usbmsc_init(bd, st, lun));
}

test "init rejects a null handle or state, logging each" {
    reset();
    var bd: Bd = .{ .iface = null, .ctx = null };
    var st: State = .{ .lun = 0 };
    try std.testing.expectEqual(usbmsc.err_null_ptr, ra8_io_blockdev_usbmsc_init(null, &st, 0));
    try std.testing.expectEqual(usbmsc.err_null_ptr, ra8_io_blockdev_usbmsc_init(&bd, null, 0));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
    try std.testing.expect(bd.iface == null);
}

test "init accepts LUN 0 through k_ra8_hmsc_max_lun and rejects the next" {
    reset();
    var bd: Bd = .{ .iface = null, .ctx = null };
    var st: State = .{ .lun = 0 };
    try std.testing.expectEqual(usbmsc.err_out_of_range, ra8_io_blockdev_usbmsc_init(&bd, &st, usbmsc.max_lun + 1));
    try std.testing.expect(bd.iface == null);
    try bound(&bd, &st, usbmsc.max_lun);
    try std.testing.expectEqual(usbmsc.max_lun, st.lun);
    try std.testing.expect(bd.iface == &usbmsc.iface);
    try std.testing.expect(bd.ctx == @as(?*anyopaque, &st));
    try std.testing.expect(usbmsc.iface.erase == null);
    try std.testing.expect(usbmsc.iface.sync == null);
}

test "read and write go to the bound LUN with the block count" {
    reset();
    var bd: Bd = undefined;
    var st: State = undefined;
    try bound(&bd, &st, 2);
    var buf = [_]u8{0} ** 512;
    try std.testing.expectEqual(usbmsc.ok, bd.iface.?.read.?(bd.ctx, 77, 1, &buf));
    try std.testing.expectEqual(@as(u8, 2), last_lun);
    try std.testing.expectEqual(@as(u32, 77), last_lba);
    try std.testing.expectEqual(@as(u16, 1), last_count);
    try std.testing.expectEqual(usbmsc.ok, bd.iface.?.write.?(bd.ctx, 9, 65535, &buf));
    try std.testing.expectEqual(@as(u16, 65535), last_count);
    try std.testing.expectEqual(@as(u32, 2), calls);
}

test "a transfer past the READ(10)/WRITE(10) ceiling is out of range" {
    reset();
    var bd: Bd = undefined;
    var st: State = undefined;
    try bound(&bd, &st, 0);
    var buf = [_]u8{0} ** 512;
    try std.testing.expectEqual(usbmsc.err_out_of_range, bd.iface.?.read.?(bd.ctx, 0, 65536, &buf));
    try std.testing.expectEqual(usbmsc.err_out_of_range, bd.iface.?.write.?(bd.ctx, 0, 65536, &buf));
    try std.testing.expectEqual(@as(u32, 0), calls);
}

test "null ctx or buffer is logged and reported before the driver" {
    reset();
    var bd: Bd = undefined;
    var st: State = undefined;
    try bound(&bd, &st, 0);
    var buf = [_]u8{0} ** 512;
    var caps: Caps = undefined;
    try std.testing.expectEqual(usbmsc.err_null_ptr, bd.iface.?.read.?(null, 0, 1, &buf));
    try std.testing.expectEqual(usbmsc.err_null_ptr, bd.iface.?.read.?(bd.ctx, 0, 1, null));
    try std.testing.expectEqual(usbmsc.err_null_ptr, bd.iface.?.write.?(null, 0, 1, &buf));
    try std.testing.expectEqual(usbmsc.err_null_ptr, bd.iface.?.write.?(bd.ctx, 0, 1, null));
    try std.testing.expectEqual(usbmsc.err_null_ptr, bd.iface.?.get_caps.?(null, &caps));
    try std.testing.expectEqual(usbmsc.err_null_ptr, bd.iface.?.get_caps.?(bd.ctx, null));
    try std.testing.expectEqual(@as(u32, 6), errors_logged);
    try std.testing.expectEqual(@as(u32, 0), calls);
}

test "a driver error passes straight back" {
    reset();
    var bd: Bd = undefined;
    var st: State = undefined;
    try bound(&bd, &st, 1);
    var buf = [_]u8{0} ** 512;
    var caps: Caps = undefined;
    result = err_timeout;
    try std.testing.expectEqual(err_timeout, bd.iface.?.read.?(bd.ctx, 0, 1, &buf));
    try std.testing.expectEqual(err_timeout, bd.iface.?.write.?(bd.ctx, 0, 1, &buf));
    try std.testing.expectEqual(err_timeout, bd.iface.?.get_caps.?(bd.ctx, &caps));
}

test "get_caps reports 512-byte writable blocks and rejects other sizes" {
    reset();
    var bd: Bd = undefined;
    var st: State = undefined;
    try bound(&bd, &st, 3);
    var caps: Caps = undefined;
    capacity_blocks = 7_812_500;
    try std.testing.expectEqual(usbmsc.ok, bd.iface.?.get_caps.?(bd.ctx, &caps));
    try std.testing.expectEqual(@as(u8, 3), last_lun);
    try std.testing.expectEqual(@as(u32, 7_812_500), caps.block_count);
    try std.testing.expectEqual(@as(u32, 1), caps.erase_unit_blocks);
    try std.testing.expectEqual(@as(u32, 512), caps.program_size_bytes);
    try std.testing.expectEqual(@as(u16, 512), caps.logical_block_bytes);
    try std.testing.expectEqual(@as(u8, 0), caps.erase_value);
    try std.testing.expect(!caps.must_erase_before_write);
    try std.testing.expect(!caps.read_only);
    capacity_block_size = 4096;
    try std.testing.expectEqual(usbmsc.err_not_supported, bd.iface.?.get_caps.?(bd.ctx, &caps));
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
