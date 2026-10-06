//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/xspi_read.zig (RA8FW-870).

const std = @import("std");
const rd = @import("xspi_read");

const off_cdctl0 = 0x070;
const off_ints = 0x190;

/// Flash model: each TRREQ serves CDA's bytes (value = addr & 0xFF) into
/// CDD0/CDD1 and logs the CDT. `fail_on` makes that kick time out.
const Regs = struct {
    mem: [0x200 / 4]u32 = @splat(0),
    kicks: u32 = 0,
    fail_on: ?u32 = null,
    cdts: [8]u32 = @splat(0),

    pub fn read(self: *Regs, off: usize) u32 {
        return self.mem[off / 4];
    }
    pub fn write(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
        if (off != off_cdctl0 or v & 1 == 0) return;
        if (self.kicks < self.cdts.len) self.cdts[self.kicks] = self.mem[0x80 / 4];
        self.kicks += 1;
        const a = self.mem[0x84 / 4];
        var w: [2]u32 = .{ 0, 0 };
        for (0..8) |i| w[i / 4] |= ((a + @as(u32, @intCast(i))) & 0xFF) << @intCast((i % 4) * 8);
        self.mem[0x88 / 4] = w[0];
        self.mem[0x8C / 4] = w[1];
    }
    pub fn poll(self: *Regs, _: u32, _: bool) bool {
        if (self.fail_on) |k| if (self.kicks == k) return false;
        self.mem[off_ints / 4] = 1;
        return true;
    }
};

test "rangeCheck bounds the window to 2^24" {
    try std.testing.expectEqual(@as(u16, 0), rd.rangeCheck(0, rd.addr_space_3byte));
    try std.testing.expectEqual(@as(u16, 0), rd.rangeCheck(0xFF_FFFF, 1));
    try std.testing.expectEqual(rd.invalid_arg, rd.rangeCheck(0xFF_FFFF, 2));
    try std.testing.expectEqual(rd.invalid_arg, rd.rangeCheck(rd.addr_space_3byte, 0));
}

test "chunkHeader writes a 3-byte-address CDT and CDA" {
    var r = Regs{};
    rd.chunkHeader(&r, 0x02, 0x1234, 8, 1);
    try std.testing.expectEqual(@as(u32, 0x0200_810D), r.mem[0x80 / 4]);
    try std.testing.expectEqual(@as(u32, 0x1234), r.mem[0x84 / 4]);
}

test "read walks 13 bytes as an 8-byte and a 5-byte chunk" {
    var r = Regs{};
    var buf: [13]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0), rd.read(&r, 0x40, &buf));
    try std.testing.expectEqual(@as(u32, 2), r.kicks);
    for (buf, 0..) |b, i| try std.testing.expectEqual(@as(u8, @intCast(0x40 + i)), b);
    try std.testing.expectEqual(@as(u32, 0x0300_010D), r.cdts[0]);
    try std.testing.expectEqual(@as(u32, 0x0300_00AD), r.cdts[1]);
}

test "a timeout on the second chunk stops the walk" {
    var r = Regs{ .fail_on = 2 };
    var buf: [16]u8 = @splat(0);
    try std.testing.expectEqual(@as(u16, 0x203), rd.read(&r, 0, &buf));
    try std.testing.expectEqual(@as(u32, 2), r.kicks);
    try std.testing.expectEqual(@as(u8, 0), buf[8]);
}

test "an out-of-range window issues no command" {
    var r = Regs{};
    var buf: [4]u8 = undefined;
    try std.testing.expectEqual(rd.invalid_arg, rd.read(&r, 0xFF_FFFE, &buf));
    try std.testing.expectEqual(@as(u32, 0), r.kicks);
}
