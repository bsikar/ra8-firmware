//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_filter.zig (RA8FW-862).

const std = @import("std");
const filter = @import("canfd_filter");

const Fake = struct {
    regs: [0x240 / 4]u32 = @splat(0),
    reset_err: u16 = 0,
    op_err: u16 = 0,
    trail: [32]u8 = undefined,
    n: usize = 0,

    fn mark(self: *Fake, c: u8) void {
        self.trail[self.n] = c;
        self.n += 1;
    }
    pub fn globalMode(self: *Fake, mode: filter.GlobalMode) u16 {
        self.mark(if (mode == .reset) 'R' else 'O');
        return if (mode == .reset) self.reset_err else self.op_err;
    }
    pub fn read(self: *Fake, off: usize) u32 {
        return self.regs[off / 4];
    }
    pub fn write(self: *Fake, off: usize, v: u32) void {
        self.mark('w');
        self.regs[off / 4] = v;
    }
    pub fn enableRxFifo0(self: *Fake) void {
        self.mark('F');
    }
};

test "validate rejects id, dlc and accept_id out of range" {
    try std.testing.expectError(error.InvalidArg, filter.validate(256, 0, 0));
    try std.testing.expectError(error.InvalidArg, filter.validate(0, 0, 16));
    try std.testing.expectError(error.InvalidArg, filter.validate(0, 0x2000_0000, 0));
    try filter.validate(255, 0x1FFF_FFFF, 15);
}

test "rnc0With only raises page-0 counts" {
    try std.testing.expectEqual(@as(?u32, 3 << 16), filter.rnc0With(0, 2));
    try std.testing.expectEqual(@as(?u32, null), filter.rnc0With(5 << 16, 2));
    try std.testing.expectEqual(@as(?u32, null), filter.rnc0With(0, 16));
    try std.testing.expectEqual(@as(?u32, (0xA0 & ~@as(u32, 0x1F << 16)) | (1 << 16)), filter.rnc0With(0xA0, 0));
}

test "set on page 0 writes RNC0, window, slot, then resumes and re-arms FIFO0" {
    var f = Fake{};
    try std.testing.expectEqual(@as(u16, 0), try filter.set(&f, 3, 0x123, 0x7FF, 8));
    try std.testing.expectEqualStrings("RwwwwwwwOF", f.trail[0..f.n]);
    try std.testing.expectEqual(@as(u32, 4 << 16), f.regs[0x2C / 4]);
    try std.testing.expectEqual(@as(u32, 0), f.regs[0x28 / 4]);
    const s = (0x120 + 3 * 16) / 4;
    try std.testing.expectEqual(@as(u32, 0x123), f.regs[s]);
    try std.testing.expectEqual(@as(u32, 0x7FF | (1 << 29)), f.regs[s + 1]);
    try std.testing.expectEqual(@as(u32, (8 << 28) | 1), f.regs[s + 3]);
}

test "set on a later page leaves RNC0 and selects the page" {
    var f = Fake{};
    _ = try filter.set(&f, 37, 1, 0, 0);
    try std.testing.expectEqualStrings("RwwwwwwOF", f.trail[0..f.n]);
    try std.testing.expectEqual(@as(u32, 0), f.regs[0x2C / 4]);
    try std.testing.expectEqual(@as(u32, 1), f.regs[(0x120 + 5 * 16) / 4]);
}

test "set reports global-mode failures and stops" {
    var f = Fake{ .reset_err = 0x203 };
    try std.testing.expectEqual(@as(u16, 0x203), try filter.set(&f, 0, 0, 0, 0));
    try std.testing.expectEqualStrings("R", f.trail[0..f.n]);
    var g = Fake{ .op_err = 0x203 };
    try std.testing.expectEqual(@as(u16, 0x203), try filter.set(&g, 0, 0, 0, 0));
    try std.testing.expectEqual(@as(u8, 'O'), g.trail[g.n - 1]);
}
