//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for internal/agt.zig (RA8FW-800, port of ra8_agt.c).

const std = @import("std");
const agt = @import("agt");

const expectEqual = std.testing.expectEqual;

const Op = struct { addr: usize, val: u16, wide: bool };

const Fake = struct {
    ops: [32]Op = undefined,
    n: usize = 0,

    pub fn write8(self: *Fake, a: usize, v: u8) void {
        self.ops[self.n] = .{ .addr = a, .val = v, .wide = false };
        self.n += 1;
    }
    pub fn write16(self: *Fake, a: usize, v: u16) void {
        self.ops[self.n] = .{ .addr = a, .val = v, .wide = true };
        self.n += 1;
    }
};

fn b(a: usize, v: u8) Op {
    return .{ .addr = a, .val = v, .wide = false };
}

fn h(a: usize, v: u16) Op {
    return .{ .addr = a, .val = v, .wide = true };
}

fn expectOps(f: *const Fake, want: []const Op) !void {
    try expectEqual(want.len, f.n);
    for (want, f.ops[0..f.n]) |x, y| {
        try expectEqual(x.addr, y.addr);
        try expectEqual(x.val, y.val);
        try expectEqual(x.wide, y.wide);
    }
}

const a0: usize = 0x4022_1000;
const a1: usize = 0x4022_1100;

test "channel addressing and mstp ids" {
    try expectEqual(@as(?usize, a0), agt.regs(0));
    try expectEqual(@as(?usize, 0x4022_1900), agt.regs(9));
    try expectEqual(@as(?usize, null), agt.regs(10));
    try expectEqual(@as(u16, 0x305), agt.mstp_ids[0]);
    try expectEqual(@as(u16, 0x304), agt.mstp_ids[1]);
}

test "pulse cfg validation and field values" {
    const good = agt.PulseCfg{ .period = 1, .duty = 1, .mode = 0, .polarity = 1, .compare = 2 };
    try expectEqual(true, agt.pulseCfgOk(good));
    var bad = good;
    bad.compare = 3;
    try expectEqual(false, agt.pulseCfgOk(bad));
    bad = good;
    bad.polarity = 2;
    try expectEqual(false, agt.pulseCfgOk(bad));
    try expectEqual(@as(u8, 0x05), agt.ioc(agt.polarity_active_high));
    try expectEqual(@as(u8, 0x04), agt.ioc(agt.polarity_active_low));
    try expectEqual(@as(u8, 0x03), agt.cmsr(agt.compare_a));
    try expectEqual(@as(u8, 0x30), agt.cmsr(agt.compare_b));
    try expectEqual(@as(u8, 0x00), agt.cmsr(agt.compare_none));
}

test "cascade clock map" {
    try expectEqual(@as(?u8, 0x00), agt.cascadeTck(0));
    try expectEqual(@as(?u8, 0x10), agt.cascadeTck(1));
    try expectEqual(@as(?u8, 0x30), agt.cascadeTck(2));
    try expectEqual(@as(?u8, null), agt.cascadeTck(3));
}

test "free run start order" {
    var f = Fake{};
    agt.startFreeRun(&f, a1, 0x1234);
    try expectOps(&f, &.{ b(a1 + 0x08, 0), b(a1 + 0x09, 0), b(a1 + 0x0A, 0), h(a1, 0x1234), b(a1 + 0x08, 1) });
}

test "pulse program parks the unused compare" {
    var f = Fake{};
    agt.programPulse(&f, a0, .{ .period = 1000, .duty = 250, .mode = 1, .polarity = 0, .compare = agt.compare_a });
    try expectOps(&f, &.{
        b(a0 + 0x08, 0),      b(a0 + 0x09, 0x01), b(a0 + 0x0A, 0),
        b(a0 + 0x0C, 0x05),   b(a0 + 0x0E, 0x03), h(a0 + 0x02, 250),
        h(a0 + 0x04, 0xFFFF), h(a0, 1000),
    });
}

test "pulse with no compare parks both" {
    var f = Fake{};
    agt.programPulse(&f, a0, .{ .period = 7, .duty = 3, .mode = 0, .polarity = 1, .compare = agt.compare_none });
    try expectEqual(@as(u16, 0xFFFF), f.ops[5].val);
    try expectEqual(@as(u16, 0xFFFF), f.ops[6].val);
    try expectEqual(@as(u16, 0x00), f.ops[4].val);
}

test "cascade splits reload and starts AGT1 first" {
    var f = Fake{};
    agt.armCascade(&f, 0xABCD_1234, 0x10);
    try expectOps(&f, &.{
        b(a0 + 0x08, 0), b(a0 + 0x09, 0x10), b(a0 + 0x0A, 0), b(a0 + 0x0C, 0), b(a0 + 0x0E, 0), h(a0, 0x1234),
        b(a1 + 0x08, 0), b(a1 + 0x09, 0x50), b(a1 + 0x0A, 0), b(a1 + 0x0C, 0), b(a1 + 0x0E, 0), h(a1, 0xABCD),
        b(a1 + 0x08, 1), b(a0 + 0x08, 1),
    });
}

test "cfg layouts" {
    try expectEqual(@as(usize, 8), @sizeOf(agt.PulseCfg));
    try expectEqual(@as(usize, 4), @offsetOf(agt.CascadeCfg, "clock"));
    try expectEqual(@as(usize, 8), @offsetOf(agt.CascadeCfg, "on_underflow"));
}
