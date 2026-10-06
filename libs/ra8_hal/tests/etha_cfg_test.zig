//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/etha_cfg.zig.

const std = @import("std");
const cfg = @import("etha_cfg");

const Regs = struct {
    mem: [0x50]u32 = @splat(0),
    pub fn read32(self: *Regs, off: usize) u32 {
        return self.mem[off / 4];
    }
    pub fn write32(self: *Regs, off: usize, v: u32) void {
        self.mem[off / 4] = v;
    }
};

test "queue arbitration rewrites only its own nibble" {
    var r = Regs{};
    r.mem[cfg.off_eatdqac / 4] = 0xFFFF_FFFF;
    cfg.setQueueArb(&r, 3, 0x5);
    try std.testing.expectEqual(@as(u32, 0xFFFF_5FFF), r.mem[cfg.off_eatdqac / 4]);
    cfg.setQueueArb(&r, 7, 0x0);
    try std.testing.expectEqual(@as(u32, 0x0FFF_5FFF), r.mem[cfg.off_eatdqac / 4]);
}

test "queue level masks current and peak to eleven bits" {
    var r = Regs{};
    r.mem[(cfg.off_eatdqm + 12) / 4] = 0xFFFF_F123;
    r.mem[(cfg.off_eatdqmlm + 12) / 4] = 0x0000_0456;
    try std.testing.expectEqual([2]u16{ 0x123, 0x456 }, cfg.queueLevel(&r, 3));
}

test "preemption packs the byte, cut-through bit and AFS" {
    try std.testing.expectEqual(@as(u32, 0x0002_01AA), cfg.preemption(0xAA, 1, 2));
    try std.testing.expectEqual(@as(u32, 0x0003_0000), cfg.preemption(0, 0, 7));
}

test "IPV remap packs a nibble per class and rejects entries over 7" {
    const map = [_]u8{ 0, 1, 2, 3, 4, 5, 6, 7 };
    try std.testing.expect(cfg.ipvMapOk(&map));
    try std.testing.expectEqual(@as(u32, 0x7654_3210), cfg.ipvPack(&map));
    const bad = [_]u8{ 0, 0, 0, 0, 0, 0, 0, 8 };
    try std.testing.expect(!cfg.ipvMapOk(&bad));
}

test "VLAN mode and tag pack per HUM 32.3.3" {
    try std.testing.expectEqual(@as(u32, 0x0005_0001), cfg.vlanMode(1, 5));
    const c = cfg.VlanTag{ .vid = 0x123, .pcp = 5, .dei = 1 };
    const s = cfg.VlanTag{ .vid = 0xABC, .pcp = 7, .dei = 0 };
    try std.testing.expectEqual(@as(u32, 0x7ABC_D123), cfg.vlanTag(&c, &s));
    try std.testing.expect(cfg.tagOk(&c));
    try std.testing.expect(!cfg.tagOk(&cfg.VlanTag{ .vid = 0x1000, .pcp = 0, .dei = 0 }));
    try std.testing.expect(!cfg.tagOk(&cfg.VlanTag{ .vid = 0, .pcp = 8, .dei = 0 }));
    try std.testing.expect(!cfg.tagOk(&cfg.VlanTag{ .vid = 0, .pcp = 0, .dei = 2 }));
}

test "traffic class range is 0 to 7" {
    try std.testing.expect(cfg.tcOk(7));
    try std.testing.expect(!cfg.tcOk(8));
}
