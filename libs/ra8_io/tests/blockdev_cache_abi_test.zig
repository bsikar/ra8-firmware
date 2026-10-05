//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The LRU write-through block cache (RA8FW-728) wrapped around a RAM
//! backend that counts reads and can be told to fail.

const std = @import("std");
const io = @import("ra8_io");
const cache = io.blockdev_cache;
const log = io.log;

const err_io: c_int = 0x401;
const blocks: u32 = 8;

var errors_logged: u32 = 0;
var backend_reads: u32 = 0;
var backend_writes: u32 = 0;
var backend_erases: u32 = 0;
var syncs: u32 = 0;
var fail: bool = false;
var disk: [blocks * 512]u8 = undefined;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}

fn ramRead(_: ?*anyopaque, lba: u32, count: u32, buf: ?[*]u8) callconv(.c) c_int {
    backend_reads += 1;
    if (fail) return err_io;
    const at = @as(usize, lba) * 512;
    @memcpy(buf.?[0 .. count * 512], disk[at .. at + count * 512]);
    return 0;
}
fn ramWrite(_: ?*anyopaque, lba: u32, count: u32, buf: ?[*]const u8) callconv(.c) c_int {
    backend_writes += 1;
    if (fail) return err_io;
    const at = @as(usize, lba) * 512;
    @memcpy(disk[at .. at + count * 512], buf.?[0 .. count * 512]);
    return 0;
}
fn ramErase(_: ?*anyopaque, _: u32, _: u32) callconv(.c) c_int {
    backend_erases += 1;
    return if (fail) err_io else 0;
}
fn ramCaps(_: ?*const anyopaque, out: ?*cache.Caps) callconv(.c) c_int {
    out.?.* = std.mem.zeroes(cache.Caps);
    out.?.block_count = blocks;
    out.?.logical_block_bytes = 512;
    return 0;
}
fn ramSync(_: ?*anyopaque) callconv(.c) c_int {
    syncs += 1;
    return 0;
}

const ram_iface = cache.Iface{ .read = ramRead, .write = ramWrite, .erase = ramErase, .get_caps = ramCaps, .sync = ramSync };
var under = cache.Device{ .iface = &ram_iface, .ctx = &disk };

var dev: cache.Device = undefined;
var state: cache.State = undefined;
var data: [3 * 512]u8 = undefined;
var slots: [3]cache.Slot = undefined;
var io_buf: [2 * 512]u8 = undefined;

fn setup() !void {
    errors_logged = 0;
    backend_reads = 0;
    backend_writes = 0;
    backend_erases = 0;
    syncs = 0;
    fail = false;
    for (&disk, 0..) |*b, i| b.* = @truncate(i / 512 + 1);
    try std.testing.expectEqual(cache.ok, cache.ra8_io_blockdev_cache_init(&dev, &state, &under, &data, &slots, slots.len));
}

fn read(lba: u32, count: u32) c_int {
    return dev.iface.?.read.?(dev.ctx, lba, count, &io_buf);
}

test "slot and state match the C layout" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(cache.Slot));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(cache.Slot, "last_use"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(cache.Slot, "valid"));
    try std.testing.expectEqual(3 * @sizeOf(usize) + 4, @offsetOf(cache.State, "clock"));
    try std.testing.expectEqual(3 * @sizeOf(usize) + 12, @offsetOf(cache.State, "misses"));
}

test "init rejects null arguments and zero slots" {
    errors_logged = 0;
    try std.testing.expectEqual(cache.err_null_ptr, cache.ra8_io_blockdev_cache_init(null, &state, &under, &data, &slots, 3));
    try std.testing.expectEqual(cache.err_null_ptr, cache.ra8_io_blockdev_cache_init(&dev, null, &under, &data, &slots, 3));
    try std.testing.expectEqual(cache.err_null_ptr, cache.ra8_io_blockdev_cache_init(&dev, &state, null, &data, &slots, 3));
    try std.testing.expectEqual(cache.err_null_ptr, cache.ra8_io_blockdev_cache_init(&dev, &state, &under, null, &slots, 3));
    try std.testing.expectEqual(cache.err_null_ptr, cache.ra8_io_blockdev_cache_init(&dev, &state, &under, &data, null, 3));
    try std.testing.expectEqual(@as(u32, 5), errors_logged);
    try std.testing.expectEqual(cache.err_invalid_size, cache.ra8_io_blockdev_cache_init(&dev, &state, &under, &data, &slots, 0));
    try std.testing.expectEqual(@as(u32, 5), errors_logged);
}

test "init clears every slot and binds the vtable" {
    for (&slots) |*s| s.* = .{ .lba = 9, .last_use = 9, .valid = true };
    try setup();
    for (slots) |s| try std.testing.expect(!s.valid and s.lba == 0 and s.last_use == 0);
    try std.testing.expectEqual(@as(?*anyopaque, &state), dev.ctx);
    try std.testing.expectEqual(@as(u32, 0), state.clock);
}

test "a repeated read hits the cache" {
    try setup();
    try std.testing.expectEqual(cache.ok, read(2, 1));
    try std.testing.expectEqual(cache.ok, read(2, 1));
    try std.testing.expectEqual(@as(u8, 3), io_buf[0]);
    try std.testing.expectEqual(@as(u32, 1), backend_reads);
    var hits: u32 = 0;
    var misses: u32 = 0;
    try std.testing.expectEqual(cache.ok, cache.ra8_io_blockdev_cache_stats(&state, &hits, &misses));
    try std.testing.expectEqual(@as(u32, 1), hits);
    try std.testing.expectEqual(@as(u32, 1), misses);
    try std.testing.expectEqual(cache.ok, cache.ra8_io_blockdev_cache_stats(&state, null, null));
}

test "a miss evicts the least recently used slot" {
    try setup();
    for ([_]u32{ 0, 1, 2 }) |lba| try std.testing.expectEqual(cache.ok, read(lba, 1));
    try std.testing.expectEqual(cache.ok, read(0, 1));
    try std.testing.expectEqual(cache.ok, read(3, 1));
    try std.testing.expectEqual(@as(u32, 3), cache.find(&state, 1));
    try std.testing.expect(cache.find(&state, 0) != 3);
    try std.testing.expectEqual(cache.ok, read(1, 1));
    try std.testing.expectEqual(@as(u32, 5), backend_reads);
}

test "multi-block reads fill each block" {
    try setup();
    try std.testing.expectEqual(cache.ok, read(4, 2));
    try std.testing.expectEqual(@as(u8, 5), io_buf[0]);
    try std.testing.expectEqual(@as(u8, 6), io_buf[512]);
}

test "writes go through and refresh the cached copy" {
    try setup();
    try std.testing.expectEqual(cache.ok, read(1, 1));
    @memset(io_buf[0..512], 0xAB);
    try std.testing.expectEqual(cache.ok, dev.iface.?.write.?(dev.ctx, 1, 1, &io_buf));
    try std.testing.expectEqual(@as(u32, 1), backend_writes);
    try std.testing.expectEqual(@as(u8, 0xAB), disk[512]);
    @memset(io_buf[0..512], 0);
    try std.testing.expectEqual(cache.ok, read(1, 1));
    try std.testing.expectEqual(@as(u8, 0xAB), io_buf[0]);
    try std.testing.expectEqual(@as(u32, 1), backend_reads);
}

test "erase invalidates only the erased range" {
    try setup();
    for ([_]u32{ 1, 2, 5 }) |lba| try std.testing.expectEqual(cache.ok, read(lba, 1));
    try std.testing.expectEqual(cache.ok, dev.iface.?.erase.?(dev.ctx, 2, 3));
    try std.testing.expectEqual(@as(u32, 1), backend_erases);
    try std.testing.expect(cache.find(&state, 1) != 3);
    try std.testing.expectEqual(@as(u32, 3), cache.find(&state, 2));
    try std.testing.expect(cache.find(&state, 5) != 3);
}

test "backend failures pass through and leave the cache alone" {
    try setup();
    fail = true;
    try std.testing.expectEqual(err_io, read(0, 1));
    try std.testing.expectEqual(@as(u32, 3), cache.find(&state, 0));
    try std.testing.expectEqual(err_io, dev.iface.?.write.?(dev.ctx, 0, 1, &io_buf));
    try std.testing.expectEqual(@as(u32, 3), cache.find(&state, 0));
    try std.testing.expectEqual(err_io, dev.iface.?.erase.?(dev.ctx, 0, 1));
    try std.testing.expect(errors_logged >= 5);
}

test "caps and sync forward, null arguments are rejected" {
    try setup();
    var caps: cache.Caps = undefined;
    try std.testing.expectEqual(cache.ok, dev.iface.?.get_caps.?(dev.ctx, &caps));
    try std.testing.expectEqual(blocks, caps.block_count);
    try std.testing.expectEqual(cache.ok, dev.iface.?.sync.?(dev.ctx));
    try std.testing.expectEqual(@as(u32, 1), syncs);
    errors_logged = 0;
    try std.testing.expectEqual(cache.err_null_ptr, dev.iface.?.read.?(null, 0, 1, &io_buf));
    try std.testing.expectEqual(cache.err_null_ptr, dev.iface.?.write.?(dev.ctx, 0, 1, null));
    try std.testing.expectEqual(cache.err_null_ptr, dev.iface.?.get_caps.?(dev.ctx, null));
    try std.testing.expectEqual(cache.err_null_ptr, dev.iface.?.sync.?(null));
    try std.testing.expectEqual(cache.err_null_ptr, cache.ra8_io_blockdev_cache_stats(null, null, null));
    try std.testing.expectEqual(@as(u32, 5), errors_logged);
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
// The VFS mount table reaches the C format registry (ra8_io_fsfmt.c); unused here.
export fn ra8_io_fsfmt_get_builtin(_: u8, _: *?*const anyopaque) c_int {
    return 0x107;
}
export fn ra8_io_fsfmt_probe(_: *const anyopaque, _: *?*const anyopaque) c_int {
    return 0x107;
}

// The blockdev_mram unit in the same archive needs these to link; unused here.
export fn ra8_flash_extra_mram_write(_: u32, _: [*]const u8, _: u32) c_int {
    return 0;
}
export fn ra8_flash_extra_mram_erase(_: u32) c_int {
    return 0;
}
