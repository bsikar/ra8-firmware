//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const mfwd = @import("eth_mfwd");

const words = 0x4A30 / 4;

const Fake = struct {
    mem: [words]u32 = [_]u32{0xFFFF_FFFF} ** words,

    fn window(f: *Fake) mfwd.Window {
        return .{ .base = @intFromPtr(&f.mem) };
    }

    fn at(f: *Fake, off: usize) u32 {
        return f.mem[off / 4];
    }
};

test "reset zeroes CTRL, STS, IE and ICLR" {
    var f: Fake = .{};
    mfwd.reset(f.window());
    try std.testing.expectEqualSlices(u32, &.{ 0, 0, 0, 0 }, f.mem[0..4]);
}

test "quiesce zeroes CTRL and IE only" {
    var f: Fake = .{};
    mfwd.quiesce(f.window());
    try std.testing.expectEqualSlices(u32, &.{ 0, 0xFFFF_FFFF, 0, 0xFFFF_FFFF }, f.mem[0..4]);
}

test "clearStatus writes ICLR and drops only the masked STS bits" {
    var f: Fake = .{};
    f.mem[1] = 0b1100;
    mfwd.clearStatus(f.window(), 0b0100);
    try std.testing.expectEqual(@as(u32, 0b1000), mfwd.status(f.window()));
    try std.testing.expectEqual(@as(u32, 0b0100), f.mem[3]);
}

test "takeStatus mirrors STS to ICLR and zeroes STS" {
    var f: Fake = .{};
    f.mem[1] = 0x42;
    try std.testing.expectEqual(@as(u32, 0x42), mfwd.takeStatus(f.window()));
    try std.testing.expectEqual(@as(u32, 0x42), f.mem[3]);
    try std.testing.expectEqual(@as(u32, 0), f.mem[1]);
}

test "setForwardingMasks rewrites PBDV[6:0] of all three ports and keeps the rest" {
    var f: Fake = .{};
    mfwd.setForwardingMasks(f.window(), &.{ 0x02, 0x81, 0x7F });
    try std.testing.expectEqual(@as(u32, 0xFFFF_FF82), f.at(0x4A00));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FF81), f.at(0x4A10));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), f.at(0x4A20));
}

test "routeQueue rewrites PBCSD[6:0] of the port's FWPBFCSDC" {
    var f: Fake = .{};
    try mfwd.routeQueue(f.window(), 1, 5);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FF85), f.at(0x4A14));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), f.at(0x4A04));
    try mfwd.routeQueue(f.window(), 0, 31);
    try std.testing.expectEqual(@as(u32, 0xFFFF_FF9F), f.at(0x4A04));
}

test "routeQueue rejects port > 1 and queue > 31 without touching registers" {
    var f: Fake = .{};
    try std.testing.expectError(error.InvalidArg, mfwd.routeQueue(f.window(), 2, 0));
    try std.testing.expectError(error.InvalidArg, mfwd.routeQueue(f.window(), 0, 32));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), f.at(0x4A04));
}
