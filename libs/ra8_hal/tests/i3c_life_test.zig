//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i3c_life.zig.

const std = @import("std");
const life = @import("i3c_life");

/// The I3C block up to INSTFC; writes are logged in order.
const Regs = struct {
    mem: [16]u32 = @splat(0),
    log: [16][2]u32 = undefined,
    n: usize = 0,
    pub fn read32(self: *Regs, off: usize) u32 {
        return self.mem[off / 4];
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
        self.log[self.n] = .{ @intCast(off), v };
        self.n += 1;
    }
};

fn expectLog(r: *const Regs, want: []const [2]u32) !void {
    try std.testing.expectEqual(want.len, r.n);
    for (want, 0..) |w, i| try std.testing.expectEqual(w, r.log[i]);
}

test "native init pulses both resets, then zeroes status, enables and MSDVAD" {
    var r = Regs{};
    life.nativeInit(&r);
    try expectLog(&r, &.{
        .{ 0x10, 1 },       .{ 0x14, 0 }, .{ 0x20, 1 }, .{ 0x20, 0 },
        .{ 0x20, 0x10000 }, .{ 0x20, 0 }, .{ 0x00, 0 }, .{ 0x30, 0 },
        .{ 0x34, 0 },       .{ 0x38, 0 }, .{ 0x3C, 0 }, .{ 0x18, 0 },
    });
}

test "native deinit clears INIE, INSTE, BCTL then CECTL" {
    var r = Regs{};
    life.nativeDeinit(&r);
    try expectLog(&r, &.{ .{ 0x38, 0 }, .{ 0x34, 0 }, .{ 0x14, 0 }, .{ 0x10, 0 } });
}

test "take status returns INST and clears it" {
    var r = Regs{};
    r.mem[0x30 / 4] = 0x0000_0410;
    try std.testing.expectEqual(@as(u32, 0x410), life.takeStatus(&r));
    try std.testing.expectEqual(@as(u32, 0), r.mem[0x30 / 4]);
    try expectLog(&r, &.{.{ 0x30, 0 }});
}
