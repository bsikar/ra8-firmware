//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/ulpt.zig.

const std = @import("std");
const ulpt = @import("ulpt");

/// Two 20-byte channel banks; ULPTCR reads keep TCSTF set for `busy` reads.
const Regs = struct {
    mem: [2][20]u8 = .{[_]u8{0xEE} ** 20} ** 2,
    cnt: [2]u32 = .{ 0xDEAD, 0xBEEF },
    busy: u32 = 0,
    writes: u32 = 0,

    pub fn read8(self: *Regs, ch: u8, off: usize) u8 {
        if (off == ulpt.off_cr and self.busy > 0) {
            self.busy -= 1;
            return self.mem[ch][off] | ulpt.cr_tcstf;
        }
        return self.mem[ch][off];
    }
    pub fn write8(self: *Regs, ch: u8, off: usize, v: u8) void {
        self.mem[ch][off] = v;
        self.writes += 1;
    }
    pub fn write32(self: *Regs, ch: u8, off: usize, v: u32) void {
        std.debug.assert(off == ulpt.off_cnt);
        self.cnt[ch] = v;
        self.writes += 1;
    }
};

const Svc = struct {
    enable_err: u16 = 0,
    fail_on: u16 = 0xFFFF,
    enabled: [2]u16 = .{ 0, 0 },
    enables: usize = 0,
    disabled: u16 = 0,
    last: [*:0]const u8 = "",
    info_val: u32 = 0xFF,

    pub fn mstpEnable(self: *Svc, id: u16) u16 {
        if (id == self.fail_on) return self.enable_err;
        if (self.enables < 2) self.enabled[self.enables] = id;
        self.enables += 1;
        return 0;
    }
    pub fn mstpDisable(self: *Svc, id: u16) u16 {
        self.disabled = id;
        return 0;
    }
    pub fn info(self: *Svc, msg: [*:0]const u8) void {
        self.last = msg;
    }
    pub fn infoVal(self: *Svc, _: [*:0]const u8, v: u32) void {
        self.info_val = v;
    }
    pub fn err(self: *Svc, msg: [*:0]const u8) void {
        self.last = msg;
    }
    pub fn fail(self: *Svc, msg: [*:0]const u8, _: u16) void {
        self.last = msg;
    }
};

test "mstpId picks MSTPE9 for ULPT0 and MSTPE8 for ULPT1" {
    try std.testing.expectEqual(@as(u16, 0x409), ulpt.mstpId(0));
    try std.testing.expectEqual(@as(u16, 0x408), ulpt.mstpId(1));
}

test "init clocks and clears both channels" {
    var r = Regs{};
    var c = Svc{};
    try std.testing.expectEqual(ulpt.ok, ulpt.init(&r, &c));
    try std.testing.expectEqual([2]u16{ 0x409, 0x408 }, c.enabled);
    for (0..2) |ch| {
        for ([_]usize{ ulpt.off_cr, ulpt.off_mr1, ulpt.off_mr2, ulpt.off_mr3, ulpt.off_ioc }) |off| {
            try std.testing.expectEqual(@as(u8, 0), r.mem[ch][off]);
        }
        try std.testing.expectEqual(@as(u32, 0), r.cnt[ch]);
    }
    try std.testing.expectEqualStrings("ulpt_init", std.mem.span(c.last));
}

test "init stops at the channel whose clock fails" {
    var r = Regs{};
    var c = Svc{ .enable_err = 0x201, .fail_on = 0x408 };
    try std.testing.expectEqual(@as(u16, 0x201), ulpt.init(&r, &c));
    try std.testing.expectEqual(@as(u32, 0), r.cnt[0]);
    try std.testing.expectEqual(@as(u32, 0xBEEF), r.cnt[1]);
    try std.testing.expectEqualStrings("ulpt_init: mstp enable", std.mem.span(c.last));
}

test "start loads the period and sets TSTART" {
    var r = Regs{};
    var c = Svc{};
    try std.testing.expectEqual(ulpt.err_invalid_arg, ulpt.start(&r, &c, 2, 5));
    try std.testing.expectEqual(@as(u32, 0), r.writes);
    try std.testing.expectEqual(ulpt.ok, ulpt.start(&r, &c, 1, 32768));
    try std.testing.expectEqual(@as(u32, 32768), r.cnt[1]);
    try std.testing.expectEqual(ulpt.cr_tstart, r.mem[1][ulpt.off_cr]);
    try std.testing.expectEqual(@as(u8, 0), r.mem[1][ulpt.off_mr3]);
    try std.testing.expectEqual(@as(u32, 1), c.info_val);
}

test "stop waits for TCSTF to clear" {
    var r = Regs{ .busy = 4 };
    try std.testing.expectEqual(ulpt.ok, ulpt.stop(&r, 0));
    try std.testing.expectEqual(@as(u8, 0), r.mem[0][ulpt.off_cr]);
    try std.testing.expectEqual(ulpt.err_invalid_arg, ulpt.stop(&r, 2));
}

test "stop times out when the counter never halts" {
    var r = Regs{ .busy = ulpt.stop_poll_max };
    try std.testing.expectEqual(ulpt.err_hw_timeout, ulpt.stop(&r, 1));
}

test "deinit, enterStop and exitStop drive module stop" {
    var r = Regs{};
    var c = Svc{};
    try std.testing.expectEqual(ulpt.ok, ulpt.deinit(&r, &c, 1));
    try std.testing.expectEqual(@as(u16, 0x408), c.disabled);
    try std.testing.expectEqual(@as(u8, 0), r.mem[1][ulpt.off_cr]);
    try std.testing.expectEqual(ulpt.ok, ulpt.enterStop(&c, 0));
    try std.testing.expectEqual(@as(u16, 0x409), c.disabled);
    try std.testing.expectEqual(ulpt.ok, ulpt.exitStop(&c, 1));
    try std.testing.expectEqual(@as(u16, 0x408), c.enabled[0]);
    try std.testing.expectEqual(ulpt.err_invalid_arg, ulpt.deinit(&r, &c, 2));
    try std.testing.expectEqual(ulpt.err_invalid_arg, ulpt.enterStop(&c, 2));
    try std.testing.expectEqual(ulpt.err_invalid_arg, ulpt.exitStop(&c, 9));
}

test "setPeriod writes only the counter" {
    var r = Regs{};
    try std.testing.expectEqual(ulpt.ok, ulpt.setPeriod(&r, 0, 77));
    try std.testing.expectEqual(@as(u32, 77), r.cnt[0]);
    try std.testing.expectEqual(@as(u32, 1), r.writes);
    try std.testing.expectEqual(ulpt.err_invalid_arg, ulpt.setPeriod(&r, 2, 1));
}

test "getStatus checks out_mask before the channel" {
    var r = Regs{};
    var c = Svc{};
    var m: u8 = 0;
    try std.testing.expectEqual(ulpt.err_null_ptr, ulpt.getStatus(&r, &c, 9, null));
    try std.testing.expectEqualStrings("out_mask must not be nullptr", std.mem.span(c.last));
    try std.testing.expectEqual(ulpt.err_invalid_arg, ulpt.getStatus(&r, &c, 2, &m));
    r.mem[1][ulpt.off_cr] = 0x03;
    try std.testing.expectEqual(ulpt.ok, ulpt.getStatus(&r, &c, 1, &m));
    try std.testing.expectEqual(@as(u8, 0x03), m);
}

var seen: u8 = 0xFF;
var seen_ctx: ?*anyopaque = null;
fn onEvent(ctx: ?*anyopaque, ch: u8) callconv(.C) void {
    seen = ch;
    seen_ctx = ctx;
}

test "dispatch calls the handler for a valid channel only" {
    var token: u8 = 0;
    ulpt.dispatch(null, null, 0);
    ulpt.dispatch(&onEvent, &token, 2);
    try std.testing.expectEqual(@as(u8, 0xFF), seen);
    ulpt.dispatch(&onEvent, &token, 1);
    try std.testing.expectEqual(@as(u8, 1), seen);
    try std.testing.expect(seen_ctx == @as(?*anyopaque, &token));
}
