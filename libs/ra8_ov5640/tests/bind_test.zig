//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the house-I2C binder: the wire framing, the argument
//! guards in their original order, and the two trampolines observed through a
//! fake seam that records every transaction. This file exports its own
//! `ra8_log_emit_error` sink, the link-time substitute for the real `ra8_core`
//! log the membrane calls on a refusal.

const std = @import("std");
const testing = std.testing;
const ov5640 = @import("ov5640");

const binder = ov5640.binder;
const driver = ov5640.driver;
const framing = ov5640.framing;

const null_ptr: u16 = 0x504;
const nack: u16 = 0x407;

var log_count: u32 = 0;

/// Link-time substitute for the real `ra8_core` log sink.
export fn ra8_log_emit_error(_: [*:0]const u8, _: [*:0]const u8) callconv(.c) void {
    log_count += 1;
}

/// A fake house seam that records what the binder handed it.
const FakeBus = struct {
    var last_addr: u8 = 0;
    var last_write: [8]u8 = undefined;
    var last_write_len: u32 = 0;
    var last_send_stop: bool = false;
    var last_wr: [8]u8 = undefined;
    var last_wr_len: u32 = 0;
    var last_rd_len: u32 = 0;
    var read_back: u8 = 0;
    var write_status: u16 = 0;
    var transfer_status: u16 = 0;
    var cookie_seen: ?*anyopaque = null;
    var delays: u32 = 0;

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
        delays = 0;
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
        if (transfer_status == 0 and rd_len > 0) {
            rd.?[0] = read_back;
        }
        return transfer_status;
    }

    fn ops() binder.BusOps {
        return .{ .write = write, .read = read, .transfer = transfer, .ctx = null };
    }
};

fn fakeDelay(_: ?*anyopaque, _: u32) callconv(.c) void {
    FakeBus.delays += 1;
}

test "a register pointer goes out big-endian, high byte first" {
    try testing.expectEqualSlices(u8, &.{ 0x30, 0x0A }, &framing.packReg(0x300A));
    try testing.expectEqualSlices(u8, &.{ 0x00, 0xFF }, &framing.packReg(0x00FF));
}

test "a write frame stages the pointer then the value" {
    try testing.expectEqualSlices(u8, &.{ 0x38, 0x21, 0x07 }, &framing.packWrite(0x3821, 0x07));
}

test "the wire sizes match the header's constants" {
    try testing.expectEqual(@as(usize, 2), framing.wire.reg_bytes);
    try testing.expectEqual(@as(usize, 3), framing.wire.frame_bytes);
}

test "bind rejects each null argument in the C's order" {
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();

    try testing.expectEqual(null_ptr, binder.ra8_ov5640_bind_i2c(null, &context, &ops, fakeDelay));
    try testing.expectEqual(null_ptr, binder.ra8_ov5640_bind_i2c(&device, null, &ops, fakeDelay));
    try testing.expectEqual(null_ptr, binder.ra8_ov5640_bind_i2c(&device, &context, null, fakeDelay));
    try testing.expectEqual(null_ptr, binder.ra8_ov5640_bind_i2c(&device, &context, &ops, null));
}

test "bind rejects a seam missing write or transfer" {
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};

    var no_write = FakeBus.ops();
    no_write.write = null;
    try testing.expectEqual(null_ptr, binder.ra8_ov5640_bind_i2c(&device, &context, &no_write, fakeDelay));

    var no_transfer = FakeBus.ops();
    no_transfer.transfer = null;
    try testing.expectEqual(null_ptr, binder.ra8_ov5640_bind_i2c(&device, &context, &no_transfer, fakeDelay));
}

test "a bound device carries the seam, the trampolines and the primary address" {
    FakeBus.reset();
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();

    try testing.expectEqual(@as(u16, 0), binder.ra8_ov5640_bind_i2c(&device, &context, &ops, fakeDelay));
    try testing.expect(device.initialized);
    try testing.expectEqual(@as(u8, 0x3C), device.address);
    try testing.expectEqual(@as(?*anyopaque, @ptrCast(&context)), device.bus.ctx);
    try testing.expect(context.bus.write != null);
    try testing.expect(context.bus.transfer != null);
}

test "a read is one write-restart-read of the two-byte pointer" {
    FakeBus.reset();
    FakeBus.read_back = 0x56;
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();
    try testing.expectEqual(@as(u16, 0), binder.ra8_ov5640_bind_i2c(&device, &context, &ops, fakeDelay));

    var value: u8 = 0;
    try testing.expectEqual(@as(u16, 0), driver.ra8_ov5640_read_reg(&device, 0x300A, &value));
    try testing.expectEqual(@as(u8, 0x56), value);
    try testing.expectEqual(@as(u8, 0x3C), FakeBus.last_addr);
    try testing.expectEqual(@as(u32, 2), FakeBus.last_wr_len);
    try testing.expectEqual(@as(u32, 1), FakeBus.last_rd_len);
    try testing.expectEqualSlices(u8, &.{ 0x30, 0x0A }, FakeBus.last_wr[0..2]);
}

test "a write is one framed transaction terminated with stop" {
    FakeBus.reset();
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();
    try testing.expectEqual(@as(u16, 0), binder.ra8_ov5640_bind_i2c(&device, &context, &ops, fakeDelay));

    try testing.expectEqual(@as(u16, 0), driver.ra8_ov5640_write_reg(&device, 0x3821, 0x07));
    try testing.expectEqual(@as(u32, 3), FakeBus.last_write_len);
    try testing.expect(FakeBus.last_send_stop);
    try testing.expectEqualSlices(u8, &.{ 0x38, 0x21, 0x07 }, FakeBus.last_write[0..3]);
}

test "the binder forwards the seam's own cookie, not the binding state" {
    FakeBus.reset();
    var marker: u32 = 0xA5;
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    var ops = FakeBus.ops();
    ops.ctx = @ptrCast(&marker);
    try testing.expectEqual(@as(u16, 0), binder.ra8_ov5640_bind_i2c(&device, &context, &ops, fakeDelay));

    try testing.expectEqual(@as(u16, 0), driver.ra8_ov5640_write_reg(&device, 0x3008, 0x42));
    try testing.expectEqual(@as(?*anyopaque, @ptrCast(&marker)), FakeBus.cookie_seen);
}

test "a transport refusal passes through untouched" {
    FakeBus.reset();
    FakeBus.write_status = nack;
    FakeBus.transfer_status = nack;
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();
    try testing.expectEqual(@as(u16, 0), binder.ra8_ov5640_bind_i2c(&device, &context, &ops, fakeDelay));

    var value: u8 = 0;
    try testing.expectEqual(nack, driver.ra8_ov5640_read_reg(&device, 0x300A, &value));
    try testing.expectEqual(nack, driver.ra8_ov5640_write_reg(&device, 0x300A, 0x01));
}

test "the bound delay callback is the one the caller supplied" {
    FakeBus.reset();
    var device: driver.Device = .{};
    var context: binder.I2cCtx = .{};
    const ops = FakeBus.ops();
    try testing.expectEqual(@as(u16, 0), binder.ra8_ov5640_bind_i2c(&device, &context, &ops, fakeDelay));

    try testing.expectEqual(@as(?driver.DelayFn, fakeDelay), device.bus.delay_ms);
    _ = driver.ra8_ov5640_stream_set(&device, 1);
    try testing.expect(FakeBus.delays > 0);
}

test "the binding state is the house seam, copied by value" {
    try testing.expectEqual(@sizeOf(binder.BusOps), @sizeOf(binder.I2cCtx));
    try testing.expectEqual(@as(usize, 0), @offsetOf(binder.I2cCtx, "bus"));
}
