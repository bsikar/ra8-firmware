//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_status.zig.

const std = @import("std");
const st = @import("dotf_status");

/// REG00 as a cell; the BIST bit clears after `clear_after` reads.
const Reg = struct {
    v: u32,
    clear_after: u32,
    reads: u32 = 0,
    writes: [4]u32 = undefined,
    n: usize = 0,
    pub fn read(self: *Reg) u32 {
        self.reads += 1;
        if (self.reads > self.clear_after) self.v &= ~st.reg00_self_test;
        return self.v;
    }
    pub fn write(self: *Reg, v: u32) void {
        self.v = v;
        self.writes[self.n] = v;
        self.n += 1;
    }
};

const Plain = struct {
    pub fn eval(_: Plain, _: u32, cond: bool) bool {
        return cond;
    }
};

test "channel REG00 addresses" {
    try std.testing.expectEqual(@as(usize, 0x4026_8880), st.reg00(0));
    try std.testing.expectEqual(@as(usize, 0x4026_8980), st.reg00(1));
}

test "self-test sets bit 20, waits, then restores REG00" {
    var r = Reg{ .v = 0x5, .clear_after = 3 };
    const res = st.selfTest(&r, Plain{});
    try std.testing.expect(res.done);
    try std.testing.expectEqual(@as(u32, 0x5), res.status);
    try std.testing.expectEqual(@as(usize, 2), r.n);
    try std.testing.expectEqual(@as(u32, 0x0010_0005), r.writes[0]);
    try std.testing.expectEqual(@as(u32, 0x5), r.writes[1]);
}

test "self-test times out after eight polls and still restores" {
    var r = Reg{ .v = 0x1, .clear_after = 100 };
    const res = st.selfTest(&r, Plain{});
    try std.testing.expect(!res.done);
    try std.testing.expectEqual(@as(u32, 0x0010_0001), res.status);
    try std.testing.expectEqual(@as(u32, 10), r.reads);
    try std.testing.expectEqual(@as(u32, 0x1), r.writes[1]);
}
