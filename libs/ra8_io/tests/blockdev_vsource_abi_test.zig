//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_io_blockdev_vsource_init/read (RA8FW-1083) over a static
//! ra8_io_blockdev_ram device: argument checks, the offset+len overflow
//! guard, aligned whole-sector reads, unaligned head and tail service,
//! sub-sector reads and error propagation from the device.

const std = @import("std");
const io = @import("ra8_io");
const ram = io.blockdev_ram;
const vsource = io.blockdev_vsource;
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

extern fn ra8_io_blockdev_vsource_init(ctx: ?*vsource.Ctx, bd: ?*const vsource.Device) c_int;
extern fn ra8_io_blockdev_vsource_read(ctx: ?*anyopaque, offset: u64, buf: ?[*]u8, len: u32) c_int;

const blocks: u32 = 8;
var disk: [blocks * 512]u8 = undefined;
var device: ram.Device = .{ .iface = null, .ctx = null };
var state: ram.State = .{ .storage = null, .block_count = 0, .read_only = false };
var ctx: vsource.Ctx = .{ .bd = null, .scratch = undefined };

fn setup() !*const ram.Iface {
    errors_logged = 0;
    @memset(&disk, 0);
    for (&disk, 0..) |*b, i| b.* = @truncate(i *% 7 +% 1);
    try std.testing.expectEqual(ram.ok, ra8_io_blockdev_ram_init(&device, &state, &disk, blocks, false));
    return device.iface.?;
}

fn bind() !void {
    try std.testing.expectEqual(vsource.ok, ra8_io_blockdev_vsource_init(&ctx, &device));
    try std.testing.expect(ctx.bd == @as(?*const vsource.Device, &device));
    for (ctx.scratch) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "init rejects null arguments with a log line each" {
    _ = try setup();
    errors_logged = 0;
    try std.testing.expectEqual(vsource.err_null_ptr, ra8_io_blockdev_vsource_init(null, &device));
    try std.testing.expectEqual(vsource.err_null_ptr, ra8_io_blockdev_vsource_init(&ctx, null));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "init binds the device and zeroes the bounce buffer" {
    _ = try setup();
    try bind();
}

test "null ctx and null buf are refused with a log line each" {
    _ = try setup();
    try bind();
    var buf: [512]u8 = undefined;
    errors_logged = 0;
    try std.testing.expectEqual(vsource.err_null_ptr, ra8_io_blockdev_vsource_read(null, 0, &buf, 4));
    try std.testing.expectEqual(vsource.err_null_ptr, ra8_io_blockdev_vsource_read(&ctx, 0, null, 4));
    try std.testing.expectEqual(@as(u32, 2), errors_logged);
}

test "offset plus len past u64 is out of range" {
    _ = try setup();
    try bind();
    var buf: [16]u8 = undefined;
    const big: u64 = std.math.maxInt(u64);
    try std.testing.expectEqual(vsource.err_out_of_range, ra8_io_blockdev_vsource_read(&ctx, big, &buf, 1));
}

test "aligned whole-sector read is byte-identical" {
    _ = try setup();
    try bind();
    var buf: [1024]u8 = undefined;
    try std.testing.expectEqual(vsource.ok, ra8_io_blockdev_vsource_read(&ctx, 2 * 512, &buf, 1024));
    try std.testing.expectEqualSlices(u8, disk[2 * 512 .. 4 * 512], &buf);
}

test "unaligned head and tail span three sectors" {
    _ = try setup();
    try bind();
    var buf: [1000]u8 = undefined;
    try std.testing.expectEqual(vsource.ok, ra8_io_blockdev_vsource_read(&ctx, 100, &buf, 1000));
    try std.testing.expectEqualSlices(u8, disk[100..1100], &buf);
}

test "sub-sector read inside one sector reads only that sector" {
    _ = try setup();
    try bind();
    var buf: [5]u8 = undefined;
    try std.testing.expectEqual(vsource.ok, ra8_io_blockdev_vsource_read(&ctx, 513, &buf, 5));
    try std.testing.expectEqualSlices(u8, disk[513..518], &buf);
}

test "zero-length read is a no-op ok" {
    _ = try setup();
    try bind();
    try std.testing.expectEqual(vsource.ok, ra8_io_blockdev_vsource_read(&ctx, 123, @as([*]u8, undefined), 0));
}

test "device errors propagate unchanged" {
    _ = try setup();
    try bind();
    var buf: [512]u8 = undefined;
    // lba 8 is one past the eight-block device: the RAM backend bounds it.
    try std.testing.expectEqual(vsource.err_out_of_range, ra8_io_blockdev_vsource_read(&ctx, 8 * 512, &buf, 512));
}
