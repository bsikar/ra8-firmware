//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/etha_tas.zig.

const std = @import("std");
const tas = @import("etha_tas");

/// One port's ETHA block. GLR/GRR report busy for `busy_reads` reads after
/// each GL1/GR write; RIRM reports ready after `ready_reads` reads. A
/// negative count never settles. Writes are logged in order.
const Regs = struct {
    mem: [0x400]u32 = @splat(0),
    log: [64][2]u32 = undefined,
    n: usize = 0,
    busy_reads: i32 = 2,
    ready_reads: i32 = 1,
    left: i32 = 0,
    learned: [8]u32 = undefined,
    nl: usize = 0,

    pub fn read32(self: *Regs, off: usize) u32 {
        if (off == tas.off_glr or off == tas.off_grr) {
            if (self.left != 0) {
                if (self.left > 0) self.left -= 1;
                return self.mem[off / 4] | (1 << 31);
            }
        }
        if (off == tas.off_rirm) {
            if (self.left != 0) {
                if (self.left > 0) self.left -= 1;
                return 0;
            }
            return 2;
        }
        return self.mem[off / 4];
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        if (self.n < self.log.len) self.log[self.n] = .{ @intCast(off), v };
        self.n += 1;
        self.mem[off / 4] = v;
        if (off == tas.off_gl1 or off == tas.off_gr) self.left = self.busy_reads;
        if (off == tas.off_rirm) self.left = self.ready_reads;
        if (off == tas.off_gl1 and self.nl < self.learned.len) {
            self.learned[self.nl] = self.mem[tas.off_gl0 / 4];
            self.nl += 1;
        }
    }
    fn lastWrite(self: *Regs, off: usize) ?u32 {
        var i = @min(self.n, self.log.len);
        while (i > 0) : (i -= 1) if (self.log[i - 1][0] == off) return self.log[i - 1][1];
        return null;
    }
};

const Ops = struct {
    err: ?[]const u8 = null,
    pub fn logError(self: *Ops, msg: [*:0]const u8) void {
        self.err = std.mem.span(msg);
    }
};

const two = [_]tas.Entry{ .{ .gate_time_ns = 500, .gate_open = true }, .{ .gate_time_ns = 1500, .gate_open = false } };
const one = [_]tas.Entry{.{ .gate_time_ns = 0x0FFF_FFFF, .gate_open = true }};

fn queuesOf() tas.Queues {
    var q: tas.Queues = @splat(.{ .entries = null, .count = 0 });
    q[0] = .{ .entries = &two, .count = 2 };
    q[3] = .{ .entries = &one, .count = 1 };
    return q;
}

test "the ABI structs match ra8_etha_types.h on the host" {
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(tas.Entry));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(tas.Entry, "gate_open"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(tas.Queue, "count"));
    try std.testing.expectEqual(@as(?usize, 0x403C_C000), tas.portBase(1));
    try std.testing.expectEqual(@as(?usize, null), tas.portBase(2));
}

test "ram reset starts TASRIOG and waits for TASRR" {
    var r = Regs{ .ready_reads = 3 };
    var o = Ops{};
    try std.testing.expectEqual(tas.ok, tas.ramReset(&r, &o));
    try std.testing.expectEqual([2]u32{ 0x3E4, 1 }, r.log[0]);
    r = .{ .ready_reads = -1 };
    try std.testing.expectEqual(tas.hw_timeout, tas.ramReset(&r, &o));
    try std.testing.expectEqualStrings("etha_tas_ram_reset: EATASRIRM.TASRR never asserted", o.err.?);
}

test "wait gives up after exactly the budget" {
    var r = Regs{ .busy_reads = 5 };
    r.write32(tas.off_gl1, 0);
    try std.testing.expectEqual(tas.hw_timeout, tas.wait(&r, tas.off_glr, 1 << 31, false, 5));
    try std.testing.expectEqual(tas.ok, tas.wait(&r, tas.off_glr, 1 << 31, false, 1));
}

test "validate rejects null entries, wide gate times and an over-full RAM" {
    var o = Ops{};
    var q = queuesOf();
    try std.testing.expectEqual(tas.ok, tas.validate(&o, &q));
    q[5] = .{ .entries = null, .count = 1 };
    try std.testing.expectEqual(tas.null_ptr, tas.validate(&o, &q));
    try std.testing.expectEqualStrings("etha_tas: queue entries null", o.err.?);
    const wide = [_]tas.Entry{.{ .gate_time_ns = 0x1000_0000, .gate_open = false }};
    q[5] = .{ .entries = &wide, .count = 1 };
    try std.testing.expectEqual(tas.invalid_arg, tas.validate(&o, &q));
    try std.testing.expectEqualStrings("etha_tas: gate time exceeds TASGTL[27:0]", o.err.?);
    var many: [120]tas.Entry = @splat(.{ .gate_time_ns = 1, .gate_open = false });
    q = queuesOf();
    q[7] = .{ .entries = &many, .count = 117 };
    try std.testing.expectEqual(tas.invalid_arg, tas.validate(&o, &q));
    try std.testing.expectEqualStrings("etha_tas: total entries exceed TAS RAM capacity", o.err.?);
    q[7].count = 116;
    try std.testing.expectEqual(tas.ok, tas.validate(&o, &q));
}

test "set_schedule programs timing, learns from TASCA and commits TASE" {
    var r = Regs{};
    var o = Ops{};
    r.mem[tas.off_tasc / 4] = 0x0010_0000 | (1 << 1); // TASCA = 0x10, stale TASCC
    var q = queuesOf();
    try std.testing.expectEqual(tas.ok, tas.setSchedule(&r, &o, &q, 0xA5, 1_000_000, 0x1_2345_6789));
    try std.testing.expectEqual([2]u32{ 0x304, 0xA5 }, r.log[0]);
    try std.testing.expectEqual(@as(?u32, 2), r.lastWrite(0x320));
    try std.testing.expectEqual(@as(?u32, 1), r.lastWrite(0x320 + 4 * 3));
    try std.testing.expectEqual(@as(?u32, 0), r.lastWrite(0x320 + 4 * 7));
    try std.testing.expectEqual(@as(?u32, 0x2345_6789), r.lastWrite(0x3A0));
    try std.testing.expectEqual(@as(?u32, 1), r.lastWrite(0x3A4));
    try std.testing.expectEqual(@as(?u32, 1_000_000), r.lastWrite(0x3B0));
    try std.testing.expectEqual(@as(usize, 3), r.nl);
    try std.testing.expectEqual([3]u32{ 0x10, 0x11, 0x12 }, r.learned[0..3].*);
    try std.testing.expectEqual(@as(?u32, 0x1000_0000 | 0x0FFF_FFFF), r.lastWrite(0x3C4));
    try std.testing.expectEqual(@as(?u32, 0x0010_0001), r.lastWrite(0x300));
}

test "set_schedule over a running schedule sets TASCC" {
    var r = Regs{};
    var o = Ops{};
    r.mem[tas.off_tasc / 4] = 1;
    var q = queuesOf();
    try std.testing.expectEqual(tas.ok, tas.setSchedule(&r, &o, &q, 0, 1, 0));
    try std.testing.expectEqual(@as(?u32, 3), r.lastWrite(0x300));
    try std.testing.expectEqual([2]u32{ 0x3C0, 0 }, r.log[12]);
}

test "set_schedule refuses while TASCI is set and writes nothing" {
    var r = Regs{};
    var o = Ops{};
    r.mem[tas.off_tasc / 4] = 1 << 2;
    var q = queuesOf();
    try std.testing.expectEqual(tas.busy, tas.setSchedule(&r, &o, &q, 0, 1, 0));
    try std.testing.expectEqualStrings("etha_set_tas_schedule: EATASC.TASCI set", o.err.?);
    try std.testing.expectEqual(@as(usize, 0), r.n);
}

test "a learn that never lands stops before the commit" {
    var r = Regs{ .busy_reads = -1 };
    var o = Ops{};
    var q = queuesOf();
    try std.testing.expectEqual(tas.hw_timeout, tas.setSchedule(&r, &o, &q, 0, 1, 0));
    try std.testing.expectEqualStrings("etha_tas: EATASGLR.GL never cleared", o.err.?);
    try std.testing.expectEqual(@as(usize, 1), r.nl);
    try std.testing.expectEqual(@as(?u32, null), r.lastWrite(0x300));
}

test "read_entry waits for GR then decodes time and gate state" {
    var r = Regs{};
    var o = Ops{};
    r.mem[tas.off_grr / 4] = 0x1000_0000 | 0x0ABC_DEF0;
    var e: tas.Entry = undefined;
    try std.testing.expectEqual(tas.ok, tas.readEntry(&r, &o, 0x42, &e));
    try std.testing.expectEqual([2]u32{ 0x3D0, 0x42 }, r.log[0]);
    try std.testing.expectEqual(@as(u32, 0x0ABC_DEF0), e.gate_time_ns);
    try std.testing.expect(e.gate_open);
    r.busy_reads = -1;
    try std.testing.expectEqual(tas.hw_timeout, tas.readEntry(&r, &o, 1, &e));
    try std.testing.expectEqualStrings("etha_read_tas_entry: EATASGRR.GR never cleared", o.err.?);
}

test "enable toggles only TASE" {
    var r = Regs{};
    r.mem[tas.off_tasc / 4] = 0x0010_0002;
    try std.testing.expectEqual(tas.ok, tas.enable(&r, true));
    try std.testing.expectEqual(@as(u32, 0x0010_0003), r.mem[tas.off_tasc / 4]);
    try std.testing.expectEqual(tas.ok, tas.enable(&r, false));
    try std.testing.expectEqual(@as(u32, 0x0010_0002), r.mem[tas.off_tasc / 4]);
}
