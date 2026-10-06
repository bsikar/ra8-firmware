//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_life.zig (RA8FW-851).

const std = @import("std");
const life = @import("eth_gwca_life");

/// Appends one letter per call so the tests can pin the order.
const Ops = struct {
    log: *std.ArrayList(u8),
    enable_err: u16 = 0,
    pub fn mstpEnable(self: Ops) u16 {
        self.log.append('E') catch unreachable;
        return self.enable_err;
    }
    pub fn mstpDisable(self: Ops) u16 {
        self.log.append('D') catch unreachable;
        return 0;
    }
    pub fn fail(self: Ops, _: [*:0]const u8, _: u16) void {
        self.log.append('F') catch unreachable;
    }
    pub fn info(self: Ops, _: [*:0]const u8) void {
        self.log.append('I') catch unreachable;
    }
    pub fn clearHandler(self: Ops) void {
        self.log.append('C') catch unreachable;
    }
};

test "init clears GWCA, sets DDE on every agent and logs once" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var g: life.events.Regs = .{ .ctrl = 5, .sts = 6, .ie = 7, .iclr = 8 };
    var f = [3]u32{ 0x10, 0x11, 0xFFFF_FFFE };
    const v = life.View{ .gwca = &g, .fwpc = .{ &f[0], &f[1], &f[2] } };
    try std.testing.expectEqual(life.ok, life.init(Ops{ .log = &log }, v));
    try std.testing.expectEqualStrings("EI", log.items);
    try std.testing.expectEqual(@as(u32, 0), g.ctrl | g.sts | g.ie | g.iclr);
    try std.testing.expectEqualSlices(u32, &.{ 0x11, 0x11, 0xFFFF_FFFF }, &f);
}

test "init stops before touching registers when MSTP fails" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var g: life.events.Regs = .{ .ctrl = 5, .sts = 6, .ie = 7, .iclr = 8 };
    var f = [3]u32{ 0, 0, 0 };
    const v = life.View{ .gwca = &g, .fwpc = .{ &f[0], &f[1], &f[2] } };
    try std.testing.expectEqual(@as(u16, 0x205), life.init(Ops{ .log = &log, .enable_err = 0x205 }, v));
    try std.testing.expectEqualStrings("EF", log.items);
    try std.testing.expectEqual(@as(u32, 5), g.ctrl);
    try std.testing.expectEqual(@as(u32, 0), f[0]);
}

test "deinit drops the handler before gating; enter_stop keeps it" {
    var log = std.ArrayList(u8).init(std.testing.allocator);
    defer log.deinit();
    var g: life.events.Regs = .{ .ctrl = 3, .sts = 1, .ie = 9, .iclr = 0 };
    var f = [3]u32{ 0, 0, 0 };
    const v = life.View{ .gwca = &g, .fwpc = .{ &f[0], &f[1], &f[2] } };
    try std.testing.expectEqual(life.ok, life.deinit(Ops{ .log = &log }, v));
    try std.testing.expectEqualStrings("CD", log.items);
    try std.testing.expectEqual(@as(u32, 0), g.ctrl | g.ie);
    try std.testing.expectEqual(@as(u32, 1), g.sts);
    log.clearRetainingCapacity();
    g.ctrl = 3;
    try std.testing.expectEqual(life.ok, life.enterStop(Ops{ .log = &log }, v));
    try std.testing.expectEqualStrings("D", log.items);
    try std.testing.expectEqual(@as(u32, 0), g.ctrl);
}
