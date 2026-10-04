//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_afl.zig.

const std = @import("std");
const afl = @import("canfd_afl");

/// Records every register write in order; reads return `cfg0`.
const Regs = struct {
    offs: [80]usize = undefined,
    vals: [80]u32 = undefined,
    n: usize = 0,
    cfg0: u32 = 0,

    pub fn read(self: *Regs, offset: usize) u32 {
        std.debug.assert(offset == afl.off_cfg0);
        return self.cfg0;
    }
    pub fn write(self: *Regs, offset: usize, value: u32) void {
        self.offs[self.n] = offset;
        self.vals[self.n] = value;
        self.n += 1;
    }
};

fn std_rule(id: u32, mask: u32, rx: u8) afl.Rule {
    return .{ .id = id, .mask = mask, .extended = false, .rtr = false, .target_rx = rx };
}

test "the rule layout matches ra8_canfd_afl_rule_t" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(afl.Rule));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(afl.Rule, "extended"));
    try std.testing.expectEqual(@as(usize, 9), @offsetOf(afl.Rule, "rtr"));
    try std.testing.expectEqual(@as(usize, 10), @offsetOf(afl.Rule, "target_rx"));
}

test "validation bounds the FIFO and the ID width" {
    try std.testing.expectEqual(afl.ok, afl.validateRule(std_rule(0x7FF, 0x7FF, 1)));
    try std.testing.expectEqual(afl.invalid_arg, afl.validateRule(std_rule(0x123, 0x7FF, 2)));
    try std.testing.expectEqual(afl.invalid_arg, afl.validateRule(std_rule(0x800, 0x7FF, 0)));
    try std.testing.expectEqual(afl.invalid_arg, afl.validateRule(std_rule(0x100, 0x800, 0)));
    var ext = std_rule(0x1FFF_FFFF, 0x1FFF_FFFF, 0);
    ext.extended = true;
    try std.testing.expectEqual(afl.ok, afl.validateRule(ext));
    ext.id = 0x2000_0000;
    try std.testing.expectEqual(afl.invalid_arg, afl.validateRule(ext));
    ext.id = 1;
    ext.mask = 0x2000_0000;
    try std.testing.expectEqual(afl.invalid_arg, afl.validateRule(ext));
}

test "validate stops at the first bad rule" {
    const rules = [_]afl.Rule{ std_rule(1, 1, 0), std_rule(1, 1, 5), std_rule(0x900, 1, 0) };
    try std.testing.expectEqual(afl.ok, afl.validate(rules[0..1]));
    try std.testing.expectEqual(afl.invalid_arg, afl.validate(&rules));
}

test "ID, mask and pointer words carry the flag bits" {
    var r = std_rule(0x123, 0x7F0, 1);
    try std.testing.expectEqual(@as(u32, 0x123), afl.idWord(r));
    try std.testing.expectEqual(@as(u32, 0xC000_07F0), afl.maskWord(r));
    try std.testing.expectEqual(@as(u32, 2), afl.p1Word(r));
    r.extended = true;
    r.rtr = true;
    r.id = 0x1ABC_DEF0;
    try std.testing.expectEqual(@as(u32, 0xDABC_DEF0), afl.idWord(r));
    try std.testing.expectEqual(@as(u32, 1), afl.p1Word(std_rule(0, 0, 0)));
}

test "cfg0 keeps the other bits and sets RNC0" {
    try std.testing.expectEqual(@as(u32, 0xFFE3_FFFF | (3 << 16)), afl.cfg0With(0xFFFF_FFFF, 3));
    try std.testing.expectEqual(@as(u32, 16 << 16), afl.cfg0With(0, 16));
}

test "program writes the window, count and each slot in order" {
    var regs: Regs = .{ .cfg0 = 0x0000_00AA };
    const rules = [_]afl.Rule{ std_rule(0x10, 0x7FF, 0), std_rule(0x20, 0x7F0, 1) };
    afl.program(&regs, &rules);
    try std.testing.expectEqual(@as(usize, 2 + 4 * 2 + 1), regs.n);
    try std.testing.expectEqual(afl.off_ectr, regs.offs[0]);
    try std.testing.expectEqual(@as(u32, 0x100), regs.vals[0]);
    try std.testing.expectEqual(afl.off_cfg0, regs.offs[1]);
    try std.testing.expectEqual(@as(u32, 0x0002_00AA), regs.vals[1]);
    try std.testing.expectEqual(@as(usize, 0x130), regs.offs[6]);
    try std.testing.expectEqual(@as(u32, 0x20), regs.vals[6]);
    try std.testing.expectEqual(@as(usize, 0x138), regs.offs[8]);
    try std.testing.expectEqual(@as(u32, 0), regs.vals[8]);
    try std.testing.expectEqual(@as(usize, 0x13C), regs.offs[9]);
    try std.testing.expectEqual(@as(u32, 2), regs.vals[9]);
    try std.testing.expectEqual(afl.off_ectr, regs.offs[10]);
    try std.testing.expectEqual(@as(u32, 0), regs.vals[10]);
}

test "a full page reaches the last slot" {
    var regs: Regs = .{};
    var rules: [afl.rule_capacity]afl.Rule = undefined;
    for (&rules, 0..) |*r, i| r.* = std_rule(@intCast(i), 0x7FF, 0);
    afl.program(&regs, &rules);
    try std.testing.expectEqual(@as(usize, 0x120 + 15 * 16 + 0xC), regs.offs[regs.n - 2]);
    try std.testing.expectEqual(@as(u32, 16 << 16), regs.vals[1]);
}
