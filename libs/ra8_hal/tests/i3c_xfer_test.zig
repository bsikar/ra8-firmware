//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i3c_xfer.zig.

const std = @import("std");
const xfer = @import("i3c_xfer");

/// NCMDQP/NTDTBP0 writes are logged; NTDTBP0 reads pop `rx`; NTST is a cell.
const Regs = struct {
    log: [8][2]u32 = undefined,
    n: usize = 0,
    rx: []const u32 = &.{},
    ntst: u32 = 0xFF,
    pub fn read32(self: *Regs, off: usize) u32 {
        if (off == 0x1E0) return self.ntst;
        const w = self.rx[0];
        self.rx = self.rx[1..];
        return w;
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        if (off == 0x1E0) {
            self.ntst = v;
            return;
        }
        self.log[self.n] = .{ @intCast(off), v };
        self.n += 1;
    }
};

fn expectLog(r: *const Regs, want: []const [2]u32) !void {
    try std.testing.expectEqual(want.len, r.n);
    for (want, 0..) |w, i| try std.testing.expectEqual(w, r.log[i]);
    try std.testing.expectEqual(@as(u32, 0xF7), r.ntst);
}

test "private descriptor word" {
    try std.testing.expectEqual(@as(u32, 0xC009_0000), xfer.xferWord(0x09, false));
    try std.testing.expectEqual(@as(u32, 0xE07F_0000), xfer.xferWord(0x7F, true));
}

test "write of four bytes or fewer rides in the descriptor" {
    var r = Regs{};
    xfer.write(&r, 0x09, &.{ 0xAA, 0xBB, 0xCC, 0xDD });
    try expectLog(&r, &.{ .{ 0x150, 0xC009_0000 | 1 | (4 << 23) }, .{ 0x150, 0xDDCC_BBAA } });
    var z = Regs{};
    xfer.write(&z, 0x09, &.{});
    try expectLog(&z, &.{ .{ 0x150, 0xC009_0000 | 1 }, .{ 0x150, 0 } });
}

test "longer write declares the length and fills the FIFO" {
    var r = Regs{};
    xfer.write(&r, 0x0A, &.{ 1, 2, 3, 4, 5 });
    try expectLog(&r, &.{ .{ 0x150, 0xC00A_0000 }, .{ 0x150, 5 << 16 }, .{ 0x158, 0x0403_0201 }, .{ 0x158, 0x05 } });
}

test "read declares the length and drains the FIFO" {
    var r = Regs{ .rx = &.{ 0x4433_2211, 0x6655 } };
    var buf: [6]u8 = undefined;
    xfer.read(&r, 0x0B, &buf);
    try expectLog(&r, &.{ .{ 0x150, 0xE00B_0000 }, .{ 0x150, 6 << 16 } });
    try std.testing.expectEqualSlices(u8, &.{ 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }, &buf);
}
