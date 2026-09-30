//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the house-I2C binder: the staged frame and its cap,
//! the argument guards in their original order, and the two trampolines
//! observed through a fake seam that records every transaction. This file
//! exports its own `ra8_log_emit_error` sink, the link-time substitute for the
//! real `ra8_core` log the membrane calls on a refusal.

const std = @import("std");
const testing = std.testing;
const lsm6dso = @import("lsm6dso");

const binder = lsm6dso.binder;
const driver = lsm6dso.driver;
const framing = lsm6dso.framing;

const invalid_arg: u16 = 0x103;
const null_ptr: u16 = 0x504;
const nack: u16 = 0x407;
const sa0_high: u8 = 0x6B;

var log_count: u32 = 0;

/// Link-time substitute for the real `ra8_core` log sink.
export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) callconv(.c) void {
    log_count += 1;
}

/// A fake house seam that records what the binder handed it.
const FakeBus = struct {
    var last_addr: u8 = 0;
    var last_write: [32]u8 = undefined;
    var last_write_len: u32 = 0;
    var last_send_stop: bool = false;
    var last_wr: [8]u8 = undefined;
    var last_wr_len: u32 = 0;
    var last_rd_len: u32 = 0;
    var read_back: u8 = 0;
    var write_status: u16 = 0;
    var transfer_status: u16 = 0;
    var cookie_seen: ?*anyopaque = null;

    fn reset() void {
        last_addr = 0;
        last_write_len = 0;
        last_send_stop = false;
        last_wr_len = 0;
        last_rd_len = 0;
        read_back = 0;
        write_status = 0;
        transfer_status = 0;
        cookie_seen = null;
    }

    fn write(ctx: ?*anyopaque, addr: u8, data: ?[*]const u8, len: u32, send_stop: bool) callconv(.c) u16 {
        cookie_seen = ctx;
        last_addr = addr;
        last_write_len = len;
        last_send_stop = send_stop;
        @memcpy(last_write[0..len], data.?[0..len]);
        return write_status;
    }

    fn read(_: ?*anyopaque, _: u8, _: ?[*]u8, _: u32) callconv(.c) u16 {
        return 0;
    }

    fn transfer(ctx: ?*anyopaque, addr: u8, wr: ?[*]const u8, wr_len: u32, rd: ?[*]u8, rd_len: u32) callconv(.c) u16 {
        cookie_seen = ctx;
        last_addr = addr;
        last_wr_len = wr_len;
        last_rd_len = rd_len;
        @memcpy(last_wr[0..wr_len], wr.?[0..wr_len]);
        if (transfer_status == 0) {
            var index: u32 = 0;
            while (index < rd_len) : (index += 1) {
                rd.?[index] = read_back +% @as(u8, @intCast(index));
            }
        }
        return transfer_status;
    }

    fn ops() binder.BusOps {
        return .{ .write = write, .read = read, .transfer = transfer, .ctx = null };
    }
};

fn bind(device: *driver.Device, context: *binder.I2cCtx) u16 {
    const ops = FakeBus.ops();
    return binder.ra8_lsm6dso_bind_i2c(device, context, &ops, sa0_high);
}

test "a staged frame is the register byte then the payload" {
    const frame = framing.stageWrite(0x10, &.{ 0xA0, 0x0B });
    try testing.expectEqualSlices(u8, &.{ 0x10, 0xA0, 0x0B }, frame.slice());
}

test "an empty payload still stages the register byte" {
    const frame = framing.stageWrite(0x0F, &.{});
    try testing.expectEqualSlices(u8, &.{0x0F}, frame.slice());
}

test "the payload cap leaves room for the register byte" {
    try testing.expectEqual(@as(usize, 16), framing.wire.frame_bytes_max);
    try testing.expect(framing.payloadFits(15));
    try testing.expect(!framing.payloadFits(16));
}

test "bind rejects each null argument in the C's order" {
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();

    try testing.expectEqual(null_ptr, binder.ra8_lsm6dso_bind_i2c(null, &context, &ops, sa0_high));
    try testing.expectEqual(null_ptr, binder.ra8_lsm6dso_bind_i2c(&device, null, &ops, sa0_high));
    try testing.expectEqual(null_ptr, binder.ra8_lsm6dso_bind_i2c(&device, &context, null, sa0_high));
}

test "bind rejects a seam missing write or transfer" {
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};

    var no_write = FakeBus.ops();
    no_write.write = null;
    try testing.expectEqual(null_ptr, binder.ra8_lsm6dso_bind_i2c(&device, &context, &no_write, sa0_high));

    var no_transfer = FakeBus.ops();
    no_transfer.transfer = null;
    try testing.expectEqual(null_ptr, binder.ra8_lsm6dso_bind_i2c(&device, &context, &no_transfer, sa0_high));
}

test "bind rejects an address outside the 7-bit space" {
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();

    try testing.expectEqual(invalid_arg, binder.ra8_lsm6dso_bind_i2c(&device, &context, &ops, 0x80));
    try testing.expectEqual(@as(u16, 0), binder.ra8_lsm6dso_bind_i2c(&device, &context, &ops, 0x7F));
}

test "a bound device carries the seam, the trampolines and the target" {
    FakeBus.reset();
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};

    try testing.expectEqual(@as(u16, 0), bind(&device, &context));
    try testing.expect(device.initialized);
    try testing.expectEqual(sa0_high, context.target_7b);
    try testing.expectEqual(@as(?*anyopaque, @ptrCast(&context)), device.bus.ctx);
    try testing.expect(context.bus.write != null);
    try testing.expect(context.bus.transfer != null);
}

test "a read is one write-restart-read of the register byte at the target" {
    FakeBus.reset();
    FakeBus.read_back = 0x6C;
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    try testing.expectEqual(@as(u16, 0), bind(&device, &context));

    var who: u8 = 0;
    try testing.expectEqual(@as(u16, 0), driver.ra8_lsm6dso_who_am_i(&device, &who));
    try testing.expectEqual(@as(u8, 0x6C), who);
    try testing.expectEqual(sa0_high, FakeBus.last_addr);
    try testing.expectEqual(@as(u32, 1), FakeBus.last_wr_len);
    try testing.expectEqual(@as(u32, 1), FakeBus.last_rd_len);
    try testing.expectEqualSlices(u8, &.{0x0F}, FakeBus.last_wr[0..1]);
}

test "a write is one framed transaction terminated with stop" {
    FakeBus.reset();
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    try testing.expectEqual(@as(u16, 0), bind(&device, &context));

    try testing.expectEqual(@as(u16, 0), driver.ra8_lsm6dso_set_accel_range(&device, 0));
    try testing.expectEqual(sa0_high, FakeBus.last_addr);
    try testing.expectEqual(@as(u32, 2), FakeBus.last_write_len);
    try testing.expect(FakeBus.last_send_stop);
}

test "the binder forwards the seam's own cookie, not the binding state" {
    FakeBus.reset();
    var marker: u32 = 0xA5;
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    var ops = FakeBus.ops();
    ops.ctx = @ptrCast(&marker);
    try testing.expectEqual(@as(u16, 0), binder.ra8_lsm6dso_bind_i2c(&device, &context, &ops, sa0_high));

    _ = driver.ra8_lsm6dso_set_accel_range(&device, 0);
    try testing.expectEqual(@as(?*anyopaque, @ptrCast(&marker)), FakeBus.cookie_seen);
}

test "a transport refusal passes through untouched" {
    FakeBus.reset();
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    try testing.expectEqual(@as(u16, 0), bind(&device, &context));

    FakeBus.transfer_status = nack;
    var who: u8 = 0;
    try testing.expectEqual(nack, driver.ra8_lsm6dso_who_am_i(&device, &who));

    FakeBus.write_status = nack;
    try testing.expectEqual(nack, driver.ra8_lsm6dso_set_accel_range(&device, 0));
}

test "the binding state is the house seam plus the target address" {
    try testing.expectEqual(@as(usize, 0), @offsetOf(binder.I2cCtx, "bus"));
    try testing.expect(@offsetOf(binder.I2cCtx, "target_7b") >= @sizeOf(binder.BusOps));
}
