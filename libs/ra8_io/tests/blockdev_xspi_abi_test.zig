//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The OSPI NOR block device (RA8FW-1085): init's geometry checks, the vtable
//! over a host buffer standing in for xSPI NOR flash with the HAL read,
//! program and erase calls captured, chunked reads and programs at the HAL
//! transfer limit, the whole-sector read-modify-write for arbitrary writes,
//! sector-aligned erase, the capability report and error propagation.

const std = @import("std");
const io = @import("ra8_io");
const xspi = io.blockdev_xspi;
const log = io.log;

const err_io: c_int = 0x401;
const err_timeout: c_int = 0x108;

var errors_logged: u32 = 0;
var reads: u32 = 0;
var programs: u32 = 0;
var erases: u32 = 0;
var fail_read_on: u32 = 0;
var fail_prog: bool = false;
var fail_erase: bool = false;

export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) void {
    errors_logged += 1;
}
export fn ra8_log_emit_error_val(_: [*:0]const u8, _: [*:0]const u8, _: u32) void {}

comptime {
    _ = @import("fs_stubs.zig");
    _ = @import("netif_stubs.zig");
    _ = @import("driver_stubs.zig");
}

const flash_sectors: u32 = 4;
const flash_bytes: u32 = flash_sectors * xspi.sector_bytes;
var flash: [flash_bytes]u8 = undefined;

export fn ra8_xspi_flash_read(instance: u8, flash_addr: u32, buf: ?[*]u8, len: u32) c_int {
    _ = instance;
    reads += 1;
    if (fail_read_on == reads) return err_io;
    const dst = buf orelse return xspi.err_null_ptr;
    @memcpy(dst[0..len], flash[flash_addr .. flash_addr + len]);
    return xspi.ok;
}

export fn ra8_xspi_flash_program(instance: u8, flash_addr: u32, data: ?[*]const u8, len: u32) c_int {
    _ = instance;
    programs += 1;
    if (fail_prog) return err_timeout;
    const src = data orelse return xspi.err_null_ptr;
    // NOR programming can only clear bits.
    for (0..len) |i| {
        flash[flash_addr + i] &= src[i];
    }
    return xspi.ok;
}

export fn ra8_xspi_flash_erase_sector(instance: u8, flash_addr: u32) c_int {
    _ = instance;
    erases += 1;
    if (fail_erase) return err_timeout;
    @memset(flash[flash_addr .. flash_addr + xspi.sector_bytes], 0xFF);
    return xspi.ok;
}

const blocks: u32 = flash_sectors * xspi.blocks_per_sector;
var device: xspi.Device = .{ .iface = null, .ctx = null };
var state: xspi.State = .{ .instance = 0, .base_off = 0, .block_count = 0, .read_only = false };

fn setup() !*const xspi.Iface {
    errors_logged = 0;
    reads = 0;
    programs = 0;
    erases = 0;
    fail_read_on = 0;
    fail_prog = false;
    fail_erase = false;
    @memset(&flash, 0);
    for (&flash, 0..) |*b, i| b.* = @truncate(i *% 7 +% 3);
    try std.testing.expectEqual(xspi.ok, ra8_io_blockdev_xspi_init(&device, &state, 1, 0, blocks, false));
    try std.testing.expectEqual(@as(u8, 1), state.instance);
    return device.iface.?;
}

extern fn ra8_io_blockdev_xspi_init(
    bd: ?*xspi.Device,
    state: ?*xspi.State,
    instance: u8,
    base_off: u32,
    block_count: u32,
    read_only: bool,
) c_int;

test "init rejects null arguments with a log line each" {
    _ = try setup();
    errors_logged = 0;
    try std.testing.expectEqual(xspi.err_null_ptr, ra8_io_blockdev_xspi_init(null, &state, 1, 0, blocks, false));
    try std.testing.expectEqual(xspi.err_null_ptr, ra8_io_blockdev_xspi_init(&device, null, 1, 0, blocks, false));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "init rejects out-of-range geometry without touching the state" {
    _ = try setup();
    try std.testing.expectEqual(xspi.err_invalid_arg, ra8_io_blockdev_xspi_init(&device, &state, xspi.instance_count, 0, blocks, false));
    try std.testing.expectEqual(xspi.err_invalid_arg, ra8_io_blockdev_xspi_init(&device, &state, 0, 512, blocks, false));
    try std.testing.expectEqual(xspi.err_invalid_arg, ra8_io_blockdev_xspi_init(&device, &state, 0, 0, 0, false));
    try std.testing.expectEqual(xspi.err_invalid_arg, ra8_io_blockdev_xspi_init(&device, &state, 0, 0, blocks - 4, false));
    try std.testing.expectEqual(@as(u32, blocks), state.block_count);
    try std.testing.expectEqual(@as(u32, 0), errors_logged);
}

test "init binds the state and reports the all-ones-erase medium" {
    const iface = try setup();
    try std.testing.expectEqual(@as(u8, 1), state.instance);
    try std.testing.expectEqual(@as(u32, blocks), state.block_count);
    try std.testing.expect(device.ctx == @as(?*anyopaque, &state));
    var caps: xspi.Caps = undefined;
    try std.testing.expectEqual(xspi.ok, iface.get_caps.?(device.ctx, &caps));
    try std.testing.expectEqual(@as(u32, blocks), caps.block_count);
    try std.testing.expectEqual(@as(u32, xspi.blocks_per_sector), caps.erase_unit_blocks);
    try std.testing.expectEqual(@as(u32, xspi.program_bytes), caps.program_size_bytes);
    try std.testing.expectEqual(@as(u16, 512), caps.logical_block_bytes);
    try std.testing.expectEqual(@as(u8, 0xFF), caps.erase_value);
    try std.testing.expect(caps.must_erase_before_write);
    try std.testing.expect(!caps.read_only);
}

test "read is bounds-checked and propagates a HAL failure mid-request" {
    const iface = try setup();
    var buf: [blocks * 512]u8 = undefined;
    try std.testing.expectEqual(xspi.err_out_of_range, iface.read.?(device.ctx, 0, blocks + 1, &buf));
    try std.testing.expectEqual(xspi.err_out_of_range, iface.read.?(device.ctx, blocks, 1, &buf));
    try std.testing.expectEqual(xspi.ok, iface.read.?(device.ctx, 0, blocks, &buf));
    try std.testing.expectEqualSlices(u8, &flash, &buf);
    fail_read_on = reads + 2; // second chunk of a two-chunk 8 KiB read
    try std.testing.expectEqual(err_io, iface.read.?(device.ctx, 0, blocks, &buf));
}

test "read honours a sector-aligned base offset" {
    _ = try setup();
    var shifted_state: xspi.State = .{ .instance = 0, .base_off = 0, .block_count = 0, .read_only = false };
    var shifted: xspi.Device = .{ .iface = null, .ctx = null };
    const win_blocks = blocks - xspi.blocks_per_sector;
    try std.testing.expectEqual(xspi.ok, ra8_io_blockdev_xspi_init(&shifted, &shifted_state, 0, xspi.sector_bytes, win_blocks, false));
    var buf: [512]u8 = undefined;
    const err = shifted.iface.?.read.?(shifted.ctx, 0, 1, &buf);
    try std.testing.expectEqual(xspi.ok, err);
    try std.testing.expectEqualSlices(u8, flash[xspi.sector_bytes .. xspi.sector_bytes + 512], &buf);
}

test "write runs the whole-sector read-modify-write for a mid-sector block" {
    const iface = try setup();
    var payload: [512]u8 = undefined;
    @memset(&payload, 0xA5);
    const target: u32 = 9; // second block of the second sector
    const sec = (target / xspi.blocks_per_sector) * xspi.sector_bytes;
    const before = flash;
    erases = 0;
    programs = 0;
    try std.testing.expectEqual(xspi.ok, iface.write.?(device.ctx, target, 1, &payload));
    try std.testing.expectEqual(@as(u32, 1), erases);
    // One whole-sector program at the HAL limit (4096 == max_xfer).
    try std.testing.expectEqual(@as(u32, 1), programs);
    try std.testing.expectEqualSlices(u8, &payload, flash[target * 512 .. target * 512 + 512]);
    // Every untouched byte of the sector survives the round trip.
    try std.testing.expectEqualSlices(u8, before[sec .. sec + 512], flash[sec .. sec + 512]);
    try std.testing.expectEqualSlices(u8, before[sec + 1024 .. sec + xspi.sector_bytes], flash[sec + 1024 .. sec + xspi.sector_bytes]);
    // Blocks outside the touched sector are untouched.
    try std.testing.expectEqualSlices(u8, before[xspi.sector_bytes..sec], flash[xspi.sector_bytes..sec]);
}

test "write spanning a sector boundary rewrites both sectors" {
    const iface = try setup();
    var payload: [4 * 512]u8 = undefined;
    @memset(&payload, 0x3C);
    const before = flash;
    erases = 0;
    try std.testing.expectEqual(xspi.ok, iface.write.?(device.ctx, 6, 4, &payload));
    try std.testing.expectEqual(@as(u32, 2), erases);
    try std.testing.expectEqualSlices(u8, &payload, flash[6 * 512 .. 10 * 512]);
    // Untouched blocks on both sides of the boundary keep their prior bytes.
    try std.testing.expectEqualSlices(u8, before[5 * 512 .. 6 * 512], flash[5 * 512 .. 6 * 512]);
    try std.testing.expectEqualSlices(u8, before[10 * 512 ..], flash[10 * 512 ..]);
}

test "write is bounds-checked, read-only guarded and HAL-failure propagating" {
    const iface = try setup();
    var payload: [512]u8 = undefined;
    @memset(&payload, 0x11);
    try std.testing.expectEqual(xspi.err_out_of_range, iface.write.?(device.ctx, 0, blocks + 1, &payload));
    fail_prog = true;
    try std.testing.expectEqual(err_timeout, iface.write.?(device.ctx, 0, 1, &payload));
    fail_prog = false;
    fail_erase = true;
    try std.testing.expectEqual(err_timeout, iface.write.?(device.ctx, 0, 1, &payload));
    fail_erase = false;
    var ro_state: xspi.State = .{ .instance = 0, .base_off = 0, .block_count = 0, .read_only = false };
    var ro: xspi.Device = .{ .iface = null, .ctx = null };
    try std.testing.expectEqual(xspi.ok, ra8_io_blockdev_xspi_init(&ro, &ro_state, 0, 0, blocks, true));
    try std.testing.expectEqual(xspi.err_not_supported, ro.iface.?.write.?(ro.ctx, 0, 1, &payload));
    try std.testing.expectEqual(xspi.err_not_supported, ro.iface.?.erase.?(ro.ctx, 0, xspi.blocks_per_sector));
}

test "erase requires sector alignment and clears the span to all-ones" {
    const iface = try setup();
    try std.testing.expectEqual(xspi.err_invalid_arg, iface.erase.?(device.ctx, 1, xspi.blocks_per_sector));
    try std.testing.expectEqual(xspi.err_invalid_arg, iface.erase.?(device.ctx, 0, xspi.blocks_per_sector - 4));
    try std.testing.expectEqual(xspi.err_out_of_range, iface.erase.?(device.ctx, 0, blocks + xspi.blocks_per_sector));
    erases = 0;
    try std.testing.expectEqual(xspi.ok, iface.erase.?(device.ctx, xspi.blocks_per_sector, 2 * xspi.blocks_per_sector));
    try std.testing.expectEqual(@as(u32, 2), erases);
    var ones: [2 * xspi.sector_bytes]u8 = undefined;
    @memset(&ones, 0xFF);
    try std.testing.expectEqualSlices(u8, flash[xspi.sector_bytes .. 3 * xspi.sector_bytes], &ones);
}

test "null ctx and null buffer arguments are refused with a log line each" {
    const iface = try setup();
    var buf: [512]u8 = undefined;
    errors_logged = 0;
    try std.testing.expectEqual(xspi.err_null_ptr, iface.read.?(null, 0, 1, &buf));
    try std.testing.expectEqual(xspi.err_null_ptr, iface.read.?(device.ctx, 0, 1, null));
    try std.testing.expectEqual(xspi.err_null_ptr, iface.write.?(null, 0, 1, &buf));
    try std.testing.expectEqual(xspi.err_null_ptr, iface.write.?(device.ctx, 0, 1, null));
    try std.testing.expectEqual(xspi.err_null_ptr, iface.erase.?(null, 0, xspi.blocks_per_sector));
    var caps: xspi.Caps = undefined;
    try std.testing.expectEqual(xspi.err_null_ptr, iface.get_caps.?(null, &caps));
    try std.testing.expectEqual(xspi.err_null_ptr, iface.get_caps.?(device.ctx, null));
    try std.testing.expectEqual(@as(u32, 7), errors_logged);
}
