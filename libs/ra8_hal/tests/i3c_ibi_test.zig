//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/i3c_ibi.zig.

const std = @import("std");
const ibi = @import("i3c_ibi");

/// Register cells by offset; NTDTBP0 reads pop `rx`; writes are logged.
const Regs = struct {
    bctl: u32 = 0,
    ntst: u32 = 0,
    nibiqp: u32 = 0,
    rx: []const u32 = &.{},
    log: [8][2]u32 = undefined,
    n: usize = 0,
    pub fn read32(self: *Regs, off: usize) u32 {
        return switch (off) {
            0x14 => self.bctl,
            0x1E0 => self.ntst,
            0x17C => self.nibiqp,
            else => blk: {
                const w = self.rx[0];
                self.rx = self.rx[1..];
                break :blk w;
            },
        };
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        switch (off) {
            0x14 => self.bctl = v,
            0x1E0 => self.ntst = v,
            else => {},
        }
        self.log[self.n] = .{ @intCast(off), v };
        self.n += 1;
    }
};

test "HDR mode check and descriptor word" {
    try std.testing.expect(!ibi.hdrModeInvalid(0, 1, 2, 2));
    try std.testing.expect(ibi.hdrModeInvalid(0, 1, 2, 3));
    try std.testing.expectEqual(@as(u32, 0xC809_0000), ibi.hdrWord(0x09, 2));
    var r = Regs{ .ntst = 0xFF };
    ibi.setHdr(&r, 0x09, 1);
    try std.testing.expectEqual(@as(usize, 2), r.n);
    try std.testing.expectEqual([2]u32{ 0x150, 0xC409_0000 }, r.log[0]);
    try std.testing.expectEqual(@as(u32, 0xF7), r.ntst);
}

test "IBI enable and target open" {
    var r = Regs{ .bctl = 0x8000_0001 };
    ibi.ibiEnable(&r);
    ibi.targetOpen(&r, 0x2A);
    try std.testing.expectEqual([2]u32{ 0xB8, 1 }, r.log[0]);
    try std.testing.expectEqual([2]u32{ 0x14, 0x0000_0001 }, r.log[1]);
    try std.testing.expectEqual([2]u32{ 0xB4, 0x802A_0000 }, r.log[2]);
    try std.testing.expectEqual(@as(u32, 0x0001_0001), r.bctl);
}

test "IBI type decode" {
    try std.testing.expectEqual(ibi.type_interrupt, ibi.ibiType(0x0000_0200));
    try std.testing.expectEqual(ibi.type_hot_join, ibi.ibiType(0x8000_0200));
    try std.testing.expectEqual(ibi.type_main_request, ibi.ibiType(0x8000_0400));
}

test "IBI read: empty queue, then a clamped payload" {
    var out: ibi.Ibi = undefined;
    var empty = Regs{};
    try std.testing.expect(!ibi.ibiRead(&empty, &out));
    var r = Regs{ .ntst = 0x7, .nibiqp = 0x0100_120A, .rx = &.{ 0x4433_2211, 0x8877_6655 } };
    try std.testing.expect(ibi.ibiRead(&r, &out));
    try std.testing.expectEqual(@as(u8, 0x09), out.address);
    try std.testing.expectEqual(ibi.type_interrupt, out.type);
    try std.testing.expectEqual(@as(u8, 8), out.payload_len);
    try std.testing.expectEqualSlices(u8, &.{ 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 }, &out.payload);
    try std.testing.expectEqual(@as(u8, 1), out.last);
    try std.testing.expectEqual(@as(u32, 0x3), r.ntst);
}
