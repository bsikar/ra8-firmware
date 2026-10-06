//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/xspi_xip.zig (RA8FW-866).

const std = @import("std");
const xip = @import("xspi_xip");

const Fake = struct {
    mem: [0x140 / 4]u32 = @splat(0),
    log: [8]usize = undefined,
    n: usize = 0,
    clears_after: ?u32 = 0,

    pub fn read(self: *Fake, off: usize) u32 {
        return self.mem[off / 4];
    }
    pub fn write(self: *Fake, off: usize, v: u32) void {
        self.mem[off / 4] = v;
        if (self.n < self.log.len) self.log[self.n] = off;
        self.n += 1;
    }
    pub fn eval(self: *Fake, off: usize, iter: u32, cond: bool) bool {
        _ = off;
        _ = cond;
        const after = self.clears_after orelse return false;
        return iter >= after;
    }
};

test "enter stages both codes, maps read-only, then arms both channels" {
    var f = Fake{};
    xip.enter(&f, 0xEB, 0xFF);
    try std.testing.expectEqualSlices(usize, &.{ xip.off_bmctl0, xip.off_cmctlch0, xip.off_cmctlch1 }, f.log[0..f.n]);
    try std.testing.expectEqual(@as(u32, 0x55), f.read(xip.off_bmctl0));
    try std.testing.expectEqual(@as(u32, 0x0001_FFEB), f.read(xip.off_cmctlch0));
    try std.testing.expectEqual(@as(u32, 0x0001_FFEB), f.read(xip.off_cmctlch1));
}

test "exit clears both channels before reopening the bus" {
    var f = Fake{};
    xip.exit(&f);
    try std.testing.expectEqualSlices(usize, &.{ xip.off_cmctlch0, xip.off_cmctlch1, xip.off_bmctl0 }, f.log[0..f.n]);
    try std.testing.expectEqual(@as(u32, 0xFF), f.read(xip.off_bmctl0));
}

test "setMode rejects widths other than 3 and 4 without writing" {
    var f = Fake{};
    try std.testing.expectError(error.InvalidArg, xip.setMode(&f, true, 0x0C, 2));
    try std.testing.expectEqual(@as(usize, 0), f.n);
    try xip.setMode(&f, true, 0x0C, 4);
    try std.testing.expectEqual(@as(u32, 0x000C_0000), f.read(xip.off_cmcfg_read_cmd));
    try std.testing.expectEqual(@as(u32, 4), f.read(xip.off_cmcfg_addr));
    try std.testing.expectEqual(@as(u32, xip.xipen), f.read(xip.off_cmctlch0));
    try xip.setMode(&f, false, 0x0C, 3);
    try std.testing.expectEqual(@as(u32, 0), f.read(xip.off_cmctlch1));
    try std.testing.expectEqual(@as(u32, 0xFF), f.read(xip.off_bmctl0));
}

test "setDtr toggles only DDREN" {
    var f = Fake{};
    f.mem[xip.off_liocfg0 / 4] = 0x5;
    xip.setDtr(&f, true);
    try std.testing.expectEqual(@as(u32, 0x805), f.read(xip.off_liocfg0));
    xip.setDtr(&f, false);
    try std.testing.expectEqual(@as(u32, 0x5), f.read(xip.off_liocfg0));
}

test "calibrate arms CAEN and times out after the spin budget" {
    var f = Fake{};
    try xip.calibrate(&f);
    try std.testing.expectEqual(@as(u32, xip.caen), f.read(xip.off_ccctl0));
    f.clears_after = 1023;
    try xip.calibrate(&f);
    f.clears_after = null;
    try std.testing.expectError(error.Timeout, xip.calibrate(&f));
}
