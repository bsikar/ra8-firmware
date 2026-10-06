//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/etha_life.zig.

const std = @import("std");
const life = @import("etha_life");

/// One port's ETHA block; writes are logged in order.
const Regs = struct {
    mem: [0x180]u32 = @splat(0),
    log: [8][2]u32 = undefined,
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

test "init sets the mode, disables every source, then writes the enables" {
    var r = Regs{};
    const cfg = life.Config{ .initial_mode = 0xFE, .eaeie0_mask = 1, .eaeie1_mask = 2, .eaeie2_mask = 3 };
    life.init(&r, &cfg);
    try expectLog(&r, &.{
        .{ 0x000, 2 },
        .{ 0x508, 0xFFFF_FFFF },
        .{ 0x518, 0xFFFF_FFFF },
        .{ 0x528, 0xFFFF_FFFF },
        .{ 0x504, 1 },
        .{ 0x514, 2 },
        .{ 0x524, 3 },
    });
}

test "deinit enters RESET and clears every enable" {
    var r = Regs{};
    life.deinit(&r);
    try expectLog(&r, &.{ .{ 0x000, 0 }, .{ 0x504, 0 }, .{ 0x514, 0 }, .{ 0x524, 0 } });
}

test "reset passes through RESET into CONFIG" {
    var r = Regs{};
    life.reset(&r);
    try expectLog(&r, &.{ .{ 0x000, 0 }, .{ 0x000, 2 } });
}

test "only CONFIG and DISABLE wait for EAMS" {
    try std.testing.expect(!life.waits(0));
    try std.testing.expect(life.waits(1));
    try std.testing.expect(life.waits(2));
    try std.testing.expect(!life.waits(3));
}
