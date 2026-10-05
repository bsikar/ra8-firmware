//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CPU1 release / halt / run-state logic (RA8FW-766).

const std = @import("std");
const dc = @import("dual_core");

const Rec = struct {
    regs: dc.Fake = .{},
    order: [4]u8 = undefined,
    n: usize = 0,
    polls: u32 = 0,
    errors: u8 = 0,
    stuck: bool = false,

    fn note(r: *Rec, c: u8) void {
        r.order[r.n] = c;
        r.n += 1;
    }
    pub fn actcsrRead(r: *Rec) u16 {
        return r.regs.actcsr;
    }
    pub fn actcsrWrite(r: *Rec, v: u16) void {
        r.note('a');
        if (!r.stuck) r.regs.writeActcsr(v);
    }
    pub fn waitcrRead(r: *Rec) u8 {
        return r.regs.waitcr;
    }
    pub fn waitcrWrite(r: *Rec, v: u8) void {
        r.note('w');
        r.regs.writeWaitcr(v);
    }
    pub fn initvtorWrite(r: *Rec, v: u32) void {
        r.note('v');
        r.regs.initvtor = v;
    }
    pub fn actPoll(r: *Rec, _: u32, cond: bool) bool {
        r.polls += 1;
        return cond;
    }
    pub fn logError(r: *Rec, _: [*:0]const u8) void {
        r.errors += 1;
    }
};

const entry: ?*anyopaque = @ptrFromInt(0x020C_0000);
const stack: ?*anyopaque = @ptrFromInt(0x2204_0000);

test "release rejects null, misaligned and non-CPU0 callers before any write" {
    var r = Rec{};
    try std.testing.expectEqual(dc.null_ptr, dc.release(&r, true, null, stack));
    try std.testing.expectEqual(dc.null_ptr, dc.release(&r, true, entry, null));
    try std.testing.expectEqual(dc.invalid_arg, dc.release(&r, true, @ptrFromInt(0x020C_0040), stack));
    try std.testing.expectEqual(dc.invalid_arg, dc.release(&r, true, entry, @ptrFromInt(0x2204_0004)));
    try std.testing.expectEqual(dc.not_supported, dc.release(&r, false, entry, stack));
    try std.testing.expectEqual(@as(usize, 0), r.n);
    try std.testing.expectEqual(@as(u8, 5), r.errors);
}

test "release writes INITVTOR, WAITCR, ACTCSR in order and sees ACT" {
    var r = Rec{};
    r.regs.waitcr = dc.waitcr_cpuwait;
    try std.testing.expectEqual(dc.ok, dc.release(&r, true, entry, stack));
    try std.testing.expectEqualSlices(u8, "vwa", r.order[0..r.n]);
    try std.testing.expectEqual(@as(u32, 0x020C_0000), r.regs.initvtor);
    try std.testing.expectEqual(@as(u8, 0), r.regs.waitcr);
    try std.testing.expectEqual(@as(u32, 1), r.polls);
    try std.testing.expect(dc.isRunning(&r));
}

test "release times out after the poll budget when ACT never asserts" {
    var r = Rec{ .stuck = true };
    try std.testing.expectEqual(dc.timeout, dc.release(&r, true, entry, stack));
    try std.testing.expectEqual(dc.release_poll_max, r.polls);
    try std.testing.expectEqual(@as(u8, 1), r.errors);
}

test "halt sets CPUWAIT and isRunning follows ACT and CPUWAIT" {
    var r = Rec{};
    try std.testing.expect(!dc.isRunning(&r));
    r.regs.actcsr = dc.actcsr_act;
    try std.testing.expect(dc.isRunning(&r));
    try std.testing.expectEqual(dc.ok, dc.halt(&r, true));
    try std.testing.expectEqual(dc.waitcr_cpuwait, r.regs.waitcr);
    try std.testing.expect(!dc.isRunning(&r));
    try std.testing.expectEqual(dc.not_supported, dc.halt(&r, false));
}

test "fake ACTCSR ignores unkeyed writes and latches ACT on ACTREQ" {
    var f = dc.Fake{};
    f.writeActcsr(dc.actcsr_actreq);
    try std.testing.expectEqual(@as(u16, 0), f.actcsr);
    f.writeActcsr(dc.actreq_word);
    try std.testing.expectEqual(dc.actcsr_act, f.actcsr);
    f.writeWaitcr(0xFF);
    try std.testing.expectEqual(dc.waitcr_cpuwait, f.waitcr);
}
