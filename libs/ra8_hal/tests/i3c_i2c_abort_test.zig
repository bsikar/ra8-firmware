//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const a = @import("i3c_i2c_abort");

const Op = enum { stop, clear };

const Fake = struct {
    ops: [4]Op = undefined,
    n: usize = 0,

    pub fn stop(self: *Fake) void {
        self.ops[self.n] = .stop;
        self.n += 1;
    }
    pub fn clearBst(self: *Fake) void {
        self.ops[self.n] = .clear;
        self.n += 1;
    }
};

test "abort masks interrupts, stops, clears and releases the bus" {
    var f = Fake{};
    var bie: u32 = 0x10;
    var ntie: u32 = 0x01;
    var held = true;
    a.run(&f, &bie, &ntie, &held);
    try std.testing.expectEqual(@as(u32, 0), bie);
    try std.testing.expectEqual(@as(u32, 0), ntie);
    try std.testing.expect(!held);
    try std.testing.expectEqualSlices(Op, &.{ .stop, .clear }, f.ops[0..f.n]);
}

test "abort on an idle bus is still a full teardown" {
    var f = Fake{};
    var bie: u32 = 0;
    var ntie: u32 = 0;
    var held = false;
    a.run(&f, &bie, &ntie, &held);
    try std.testing.expect(!held);
    try std.testing.expectEqual(@as(usize, 2), f.n);
}

test "register offsets match the HUM map" {
    try std.testing.expectEqual(@as(usize, 0x1D8), a.off_bie);
    try std.testing.expectEqual(@as(usize, 0x1E8), a.off_ntie);
}
