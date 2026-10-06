//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/rtc_init.zig (RA8FW-855).

const std = @import("std");
const r = @import("rtc_init");

/// Fake registers plus an event log of each hw call.
const Box = struct {
    rcr1: u8 = 0xFF,
    rcr2: u8 = 0,
    rcr4: u8 = 0xFF,
    rfrh: u16 = 0xFFFF,
    rfrl: u16 = 0xFFFF,
    lococr: u8 = 1,
    sosccr: u8 = 1,
    somcr: u8 = 0xFF,
    osc_ok: bool = true,
    log: std.ArrayList(u8),

    fn view(self: *Box) r.View {
        return .{ .rcr1 = &self.rcr1, .rcr2 = &self.rcr2, .rcr4 = &self.rcr4, .rfrh = &self.rfrh, .rfrl = &self.rfrl, .lococr = &self.lococr, .sosccr = &self.sosccr, .somcr = &self.somcr };
    }
};

const Hw = struct {
    box: *Box,
    fn put(self: Hw, s: []const u8) void {
        self.box.log.appendSlice(s) catch unreachable;
    }
    fn putf(self: Hw, comptime f: []const u8, args: anytype) void {
        var buf: [32]u8 = undefined;
        self.put(std.fmt.bufPrint(&buf, f, args) catch unreachable);
    }
    pub fn wait(self: Hw, _: *volatile u8, mask: u8, expect: u8) void {
        self.putf("w{x}/{x} ", .{ mask, expect });
    }
    pub fn delay(self: Hw, ms: u32) void {
        self.putf("d{d} ", .{ms});
    }
    pub fn prcr(self: Hw, value: u16) void {
        self.put(if (value == r.prcr_unlock_cgc) "unlock " else "lock ");
    }
    pub fn running(self: Hw, _: *volatile u8, _: u8) bool {
        return self.box.osc_ok;
    }
    pub fn info(self: Hw, _: [*:0]const u8) void {
        self.put("info");
    }
    pub fn infoVal(self: Hw, _: [*:0]const u8, value: u32) void {
        self.putf("info{d}", .{value});
    }
    pub fn err(self: Hw, _: [*:0]const u8) void {
        self.put("err");
    }
};

test "init leaves 24h mode running with IRQs masked" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var box = Box{ .log = std.ArrayList(u8).init(arena.allocator()) };
    try std.testing.expectEqual(r.ok, r.init(Hw{ .box = &box }, box.view()));
    try std.testing.expectEqual(@as(u8, 0), box.rcr1);
    try std.testing.expectEqual(@as(u8, 0x41), box.rcr2);
    try std.testing.expectEqualStrings("w80/0 wff/0 w40/40 w1/1 info", box.log.items);
}

test "clockInit LOCO programs RFR and soft-resets in order" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var box = Box{ .rcr2 = 0x41, .log = std.ArrayList(u8).init(arena.allocator()) };
    try std.testing.expectEqual(r.ok, r.clockInit(Hw{ .box = &box }, box.view(), r.clk_loco));
    try std.testing.expectEqual(@as(u8, 0), box.lococr);
    try std.testing.expectEqual(@as(u8, 1), box.rcr4);
    try std.testing.expectEqual(@as(u16, 0), box.rfrh);
    try std.testing.expectEqual(@as(u16, 0xFF), box.rfrl);
    try std.testing.expectEqual(r.rcr2_reset, box.rcr2);
    try std.testing.expectEqualStrings("unlock lock d5 d1 w1/0 d5 w2/0 info1", box.log.items);
}

test "clockInit sub-clock leaves RFR alone and waits for the crystal" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var box = Box{ .log = std.ArrayList(u8).init(arena.allocator()) };
    try std.testing.expectEqual(r.ok, r.clockInit(Hw{ .box = &box }, box.view(), r.clk_subclock));
    try std.testing.expectEqual(@as(u8, 0), box.sosccr | box.somcr | box.rcr4);
    try std.testing.expectEqual(@as(u16, 0xFFFF), box.rfrl);
    try std.testing.expectEqualStrings("unlock lock d1000 d1 w1/0 d5 w2/0 info0", box.log.items);
}

test "clockInit rejects a bad source and reports a dead oscillator" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    var box = Box{ .osc_ok = false, .log = std.ArrayList(u8).init(arena.allocator()) };
    try std.testing.expectEqual(r.invalid_arg, r.clockInit(Hw{ .box = &box }, box.view(), 2));
    try std.testing.expectEqual(r.hw_init_failed, r.clockInit(Hw{ .box = &box }, box.view(), r.clk_loco));
    try std.testing.expectEqual(@as(u8, 0xFF), box.rcr4);
    try std.testing.expectEqualStrings("unlock lock d5 err", box.log.items);
}
