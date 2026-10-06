//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_mode.zig (RA8FW-863).

const std = @import("std");
const mode = @import("canfd_mode");

const Fake = struct {
    regs: [0x140 / 4]u32 = @splat(0),
    stuck_off: ?usize = null,
    trail: [64]u8 = undefined,
    n: usize = 0,

    fn mark(self: *Fake, c: u8) void {
        self.trail[self.n] = c;
        self.n += 1;
    }
    pub fn read(self: *Fake, off: usize) u32 {
        return self.regs[off / 4];
    }
    pub fn write(self: *Fake, off: usize, v: u32) void {
        self.regs[off / 4] = v;
        self.mark(switch (off) {
            mode.off_ctr => 'c',
            mode.off_gctr => 'g',
            mode.off_rfcc0 => 'f',
            else => 'a',
        });
    }
    pub fn wait(self: *Fake, off: usize, mask: u32, set: bool) bool {
        _ = mask;
        _ = set;
        self.mark('W');
        return self.stuck_off != off;
    }
};

test "modeWord sets the mode field and clears the sleep request" {
    try std.testing.expectEqual(@as(u32, 0xF0 | 2), mode.modeWord(0xF7, 2));
    try std.testing.expectEqual(@as(u32, 1), mode.modeWord(0x4, 5));
}

test "waitFor picks the status bit per mode" {
    try std.testing.expectEqual(mode.Wait{ .mask = 2, .set = true }, mode.waitFor(mode.halt));
    try std.testing.expectEqual(mode.Wait{ .mask = 1, .set = true }, mode.waitFor(mode.reset));
    try std.testing.expectEqual(mode.Wait{ .mask = 3, .set = false }, mode.waitFor(mode.operation));
    try std.testing.expectEqual(mode.Wait{ .mask = 3, .set = false }, mode.waitFor(3));
}

test "handshakes write the control register and report a stuck status" {
    var f = Fake{ .regs = undefined };
    @memset(&f.regs, 0);
    f.regs[mode.off_ctr / 4] = 0x4;
    try std.testing.expectEqual(@as(u16, 0), mode.setChannelMode(&f, mode.reset));
    try std.testing.expectEqual(@as(u32, 1), f.regs[mode.off_ctr / 4]);
    f.stuck_off = mode.off_gsts;
    try std.testing.expectEqual(@as(u16, 0x203), mode.setGlobalMode(&f, mode.halt));
    try std.testing.expectEqual(@as(u32, 2), f.regs[mode.off_gctr / 4]);
}

test "openChannel order and the default rule" {
    var f = Fake{};
    f.regs[mode.off_rfcc0 / 4] = 0;
    try std.testing.expectEqual(@as(u16, 0), mode.openChannel(&f));
    try std.testing.expectEqualStrings("gWcWaaaaaaafgWfcW", f.trail[0..f.n]);
    try std.testing.expectEqual(@as(u32, 1 << 16), f.regs[0x2C / 4]);
    try std.testing.expectEqual(@as(u32, 1), f.regs[(0x120 + 0xC) / 4]);
    try std.testing.expectEqual(@as(u32, mode.rfcc_default | 1), f.regs[mode.off_rfcc0 / 4]);
}

test "openChannel stops when global operation never latches" {
    var f = Fake{ .stuck_off = mode.off_gsts };
    try std.testing.expectEqual(@as(u16, 0x203), mode.openChannel(&f));
    try std.testing.expectEqualStrings("gWcWaaaaaaafgW", f.trail[0..f.n]);
}
