//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/poeg.zig.

const std = @import("std");
const poeg = @import("poeg");

const Regs = struct {
    poegg: [poeg.group_count]u32 = .{ 0, 0, 0, 0 },
    writes: u32 = 0,

    pub fn read(self: *Regs, group: u8) u32 {
        return self.poegg[group];
    }
    pub fn write(self: *Regs, group: u8, v: u32) void {
        self.poegg[group] = v;
        self.writes += 1;
    }
};

const Svc = struct {
    enable_err: u16 = 0,
    enabled: u16 = 0,
    disabled: u16 = 0,
    last: [*:0]const u8 = "",
    info_val: u32 = 0xFF,

    pub fn mstpEnable(self: *Svc, id: u16) u16 {
        self.enabled = id;
        return self.enable_err;
    }
    pub fn mstpDisable(self: *Svc, id: u16) u16 {
        self.disabled = id;
        return 0x0BAD;
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

const all_on: poeg.Cfg = .{ .enable_pin = true, .enable_ioc = true, .enable_osc_stop = true, .invert_input = true };

test "mstp ids follow MSTPD14 down to MSTPD11" {
    try std.testing.expectEqual(@as(u16, 0x030E), poeg.mstpId(0));
    try std.testing.expectEqual(@as(u16, 0x030B), poeg.mstpId(3));
}

test "cfg maps each flag to its POEGG enable bit" {
    try std.testing.expectEqual(@as(u32, 0x1700), poeg.cfgToPoegg(all_on));
    const pin_only: poeg.Cfg = .{ .enable_pin = true, .enable_ioc = false, .enable_osc_stop = false, .invert_input = false };
    try std.testing.expectEqual(poeg.en_pide, poeg.cfgToPoegg(pin_only));
    try std.testing.expectEqual(@as(usize, 4), @sizeOf(poeg.Cfg));
}

test "init enables the module and writes POEGG" {
    var r = Regs{};
    var c = Svc{};
    try std.testing.expectEqual(poeg.ok, poeg.init(&r, &c, 2, &all_on));
    try std.testing.expectEqual(poeg.mstpId(2), c.enabled);
    try std.testing.expectEqual(@as(u32, 0x1700), r.poegg[2]);
    try std.testing.expectEqual(@as(u32, 2), c.info_val);
}

test "init rejects a null cfg before the group, and a bad group" {
    var r = Regs{};
    var c = Svc{};
    try std.testing.expectEqual(poeg.err_null_ptr, poeg.init(&r, &c, 9, null));
    try std.testing.expectEqualStrings("cfg must not be nullptr", std.mem.span(c.last));
    try std.testing.expectEqual(poeg.err_null_ptr, poeg.init(&r, &c, 4, &all_on));
    try std.testing.expectEqualStrings("group out of range", std.mem.span(c.last));
    try std.testing.expectEqual(@as(u32, 0), r.writes);
}

test "init returns the mstp error and leaves POEGG alone" {
    var r = Regs{};
    var c = Svc{ .enable_err = 0x201 };
    try std.testing.expectEqual(@as(u16, 0x201), poeg.init(&r, &c, 0, &all_on));
    try std.testing.expectEqual(@as(u32, 0), r.writes);
}

test "deinit clears POEGG and the slot and ignores the mstp result" {
    var r = Regs{ .poegg = .{ 0, 0x1700, 0, 0 } };
    var c = Svc{};
    var slots = [_]poeg.Slot{.{}} ** poeg.group_count;
    var dummy: u8 = 0;
    slots[1].ctx = &dummy;
    try std.testing.expectEqual(poeg.ok, poeg.deinit(&r, &c, &slots, 1));
    try std.testing.expectEqual(@as(u32, 0), r.poegg[1]);
    try std.testing.expect(slots[1].ctx == null);
    try std.testing.expectEqual(poeg.mstpId(1), c.disabled);
    try std.testing.expectEqual(poeg.err_null_ptr, poeg.deinit(&r, &c, &slots, 4));
}

test "trigger stop sets SSF and keeps the enables" {
    var r = Regs{ .poegg = .{ 0x0100, 0, 0, 0 } };
    var c = Svc{};
    try std.testing.expectEqual(poeg.ok, poeg.triggerStop(&r, &c, 0));
    try std.testing.expectEqual(@as(u32, 0x0108), r.poegg[0]);
    try std.testing.expectEqual(poeg.err_null_ptr, poeg.triggerStop(&r, &c, 4));
}

test "get status masks to the status bits" {
    var r = Regs{ .poegg = .{ 0, 0, 0, 0x0001_170F } };
    var c = Svc{};
    var out: u32 = 0;
    try std.testing.expectEqual(poeg.ok, poeg.getStatus(&r, &c, 3, &out));
    try std.testing.expectEqual(@as(u32, 0x0001_000F), out);
    try std.testing.expectEqual(poeg.err_null_ptr, poeg.getStatus(&r, &c, 3, null));
    try std.testing.expectEqualStrings("out_mask must not be nullptr", std.mem.span(c.last));
    try std.testing.expectEqual(poeg.err_null_ptr, poeg.getStatus(&r, &c, 4, &out));
}

test "clear status only clears status bits in the mask" {
    var r = Regs{ .poegg = .{ 0x0001_170F, 0, 0, 0 } };
    var c = Svc{};
    try std.testing.expectEqual(poeg.ok, poeg.clearStatus(&r, &c, 0, 0xFFFF_FFF3));
    try std.testing.expectEqual(@as(u32, 0x0000_170C), r.poegg[0]);
    try std.testing.expectEqual(poeg.err_null_ptr, poeg.clearStatus(&r, &c, 4, 1));
}

test "stop entry and exit toggle module stop, range checked" {
    var c = Svc{};
    try std.testing.expectEqual(@as(u16, 0x0BAD), poeg.enterStop(&c, 3));
    try std.testing.expectEqual(poeg.mstpId(3), c.disabled);
    try std.testing.expectEqual(poeg.ok, poeg.exitStop(&c, 0));
    try std.testing.expectEqual(poeg.mstpId(0), c.enabled);
    try std.testing.expectEqual(poeg.err_invalid_arg, poeg.enterStop(&c, 4));
    try std.testing.expectEqual(poeg.err_invalid_arg, poeg.exitStop(&c, 4));
}

var seen_mask: u32 = 0;
var seen_ctx: ?*anyopaque = null;
fn record(ctx: ?*anyopaque, mask: u32) callconv(.C) void {
    seen_ctx = ctx;
    seen_mask = mask;
}

test "dispatch hands the latched status to the attached handler" {
    var r = Regs{ .poegg = .{ 0, 0, 0x0001_1709, 0 } };
    var slots = [_]poeg.Slot{.{}} ** poeg.group_count;
    var token: u8 = 7;
    try std.testing.expectEqual(poeg.ok, poeg.attachHandler(&slots, 2, record, &token));
    try std.testing.expectEqual(poeg.err_invalid_arg, poeg.attachHandler(&slots, 4, record, null));
    poeg.dispatch(&r, &slots, 2);
    try std.testing.expectEqual(@as(u32, 0x0001_0009), seen_mask);
    try std.testing.expect(seen_ctx == @as(?*anyopaque, &token));
    seen_mask = 0;
    poeg.dispatch(&r, &slots, 1); // no handler
    poeg.dispatch(&r, &slots, 4); // out of range
    try std.testing.expectEqual(@as(u32, 0), seen_mask);
}
