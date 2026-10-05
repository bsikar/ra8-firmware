//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GT911 touch logic (RA8FW-790): packing, decode, clamps and the frame
//! read order against a fake bus.

const std = @import("std");
const t = @import("touch");

const Fake = struct {
    status: u8 = 0,
    status_err: u16 = 0,
    block_err: u16 = 0,
    product: [4]u8 = .{ '9', '1', '1', 0 },
    block: [40]u8 = [_]u8{0} ** 40,
    block_len: usize = 0,
    acks: usize = 0,
    last_write: [2]u16 = .{ 0, 0 },

    pub fn read(self: *Fake, reg: u16, buf: []u8) u16 {
        switch (reg) {
            t.reg_status => {
                if (self.status_err != 0) return self.status_err;
                buf[0] = self.status;
            },
            t.reg_product => @memcpy(buf, &self.product),
            t.reg_point0 => {
                if (self.block_err != 0) return self.block_err;
                self.block_len = buf.len;
                @memcpy(buf, self.block[0..buf.len]);
            },
            else => unreachable,
        }
        return t.ok;
    }
    pub fn writeByte(self: *Fake, reg: u16, value: u8) u16 {
        self.acks += 1;
        self.last_write = .{ reg, value };
        return t.ok;
    }
};

fn record(raw: []u8, i: usize, track: u8, x: u16, y: u16, size: u8) void {
    const r = raw[i * t.point_bytes ..][0..t.point_bytes];
    r.* = .{ track, @truncate(x), @truncate(x >> 8), @truncate(y), @truncate(y >> 8), size, 0xAA, 0 };
}

test "register pointer goes out big-endian" {
    try std.testing.expectEqual([2]u8{ 0x81, 0x4E }, t.packReg(t.reg_status));
    try std.testing.expectEqual([2]u8{ 0x81, 0x40 }, t.packReg(t.reg_product));
}

test "decodeBlock caps at max_count and five, and reports the count" {
    var raw = [_]u8{0} ** 48;
    for (0..6) |i| record(&raw, i, @intCast(i), @intCast(100 + i), @intCast(0x1234 + i), @intCast(7 + i));
    var out: [6]t.Point = undefined;
    var got: u8 = 99;
    t.decodeBlock(&raw, 6, &out, 9, &got);
    try std.testing.expectEqual(@as(u8, 5), got);
    try std.testing.expectEqual(t.Point{ .x = 100, .y = 0x1234, .track_id = 0, .pressure = 7 }, out[0]);
    try std.testing.expectEqual(t.Point{ .x = 104, .y = 0x1238, .track_id = 4, .pressure = 11 }, out[4]);
    t.decodeBlock(&raw, 6, &out, 2, &got);
    try std.testing.expectEqual(@as(u8, 2), got);
}

test "address and cap clamps match the C" {
    try std.testing.expect(t.validAddr(0x5D) and t.validAddr(0x14));
    try std.testing.expect(!t.validAddr(0x5C));
    try std.testing.expectEqual(@as(u8, 5), t.clampCap(0));
    try std.testing.expectEqual(@as(u8, 5), t.clampCap(6));
    try std.testing.expectEqual(@as(u8, 3), t.clampCap(3));
    try std.testing.expectEqual(@as(u8, 2), t.clampEmit(0x84, 9, 2));
    try std.testing.expectEqual(@as(u8, 4), t.clampEmit(0xF4, 9, 5));
}

test "product id must start with 9" {
    var bus = Fake{};
    try std.testing.expectEqual(t.ok, t.checkProductId(&bus));
    bus.product[0] = '8';
    try std.testing.expectEqual(t.err_hw_init_failed, t.checkProductId(&bus));
}

test "frame not ready: ok, zero count, no ack" {
    var bus = Fake{ .status = 0x03 };
    var out: [5]t.Point = undefined;
    var got: u8 = 9;
    try std.testing.expectEqual(t.ok, t.readFrame(&bus, &out, 5, 5, &got));
    try std.testing.expectEqual(@as(u8, 0), got);
    try std.testing.expectEqual(@as(usize, 0), bus.acks);
}

test "ready frame with two points reads 16 bytes and acks once" {
    var bus = Fake{ .status = 0x82 };
    record(&bus.block, 0, 1, 10, 20, 30);
    record(&bus.block, 1, 2, 300, 400, 50);
    var out: [5]t.Point = undefined;
    var got: u8 = 0;
    try std.testing.expectEqual(t.ok, t.readFrame(&bus, &out, 5, 5, &got));
    try std.testing.expectEqual(@as(u8, 2), got);
    try std.testing.expectEqual(@as(usize, 16), bus.block_len);
    try std.testing.expectEqual(t.Point{ .x = 300, .y = 400, .track_id = 2, .pressure = 50 }, out[1]);
    try std.testing.expectEqual(@as(usize, 1), bus.acks);
    try std.testing.expectEqual([2]u16{ t.reg_status, t.cmd_clear_status }, bus.last_write);
}

test "read errors: status fails without ack, block fails with ack" {
    var out: [5]t.Point = undefined;
    var got: u8 = 9;
    var bus = Fake{ .status_err = 0x204 };
    try std.testing.expectEqual(t.err_hw_error, t.readFrame(&bus, &out, 5, 5, &got));
    try std.testing.expectEqual(@as(u8, 0), got);
    try std.testing.expectEqual(@as(usize, 0), bus.acks);
    bus = Fake{ .status = 0x81, .block_err = 0x204 };
    got = 9;
    try std.testing.expectEqual(t.err_hw_error, t.readFrame(&bus, &out, 5, 5, &got));
    try std.testing.expectEqual(@as(u8, 0), got);
    try std.testing.expectEqual(@as(usize, 1), bus.acks);
    bus = Fake{ .status = 0x80 };
    got = 9;
    try std.testing.expectEqual(t.ok, t.readFrame(&bus, &out, 5, 5, &got));
    try std.testing.expectEqual(@as(u8, 0), got);
    try std.testing.expectEqual(@as(usize, 1), bus.acks);
}
