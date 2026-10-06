//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_region.zig.

const std = @import("std");
const region = @import("dotf_region");
const state = region.state_mod;

const FakeRegs = struct {
    log: [2]u32 = .{ 0, 0 },
    order: [2]u8 = .{ 0, 0 },
    n: usize = 0,
    pub fn writeEnd(self: *FakeRegs, v: u32) void {
        self.log[self.n] = v;
        self.order[self.n] = 'E';
        self.n += 1;
    }
    pub fn writeStart(self: *FakeRegs, v: u32) void {
        self.log[self.n] = v;
        self.order[self.n] = 'S';
        self.n += 1;
    }
};

fn fresh() [2]state.ChanState {
    var s: [2]state.ChanState = @splat(.{});
    for (&s) |*st| state.reset(st);
    return s;
}

const good0 = state.Region{ .start_addr = 0x8000_0000, .end_addr = 0x8000_F000, .region_id = 1 };

test "windows follow the XSPI pairing" {
    try std.testing.expectEqual(@as(u32, 0x8000_0000), region.windowLo(0));
    try std.testing.expectEqual(@as(u32, 0x9FFF_FFFF), region.windowHi(0));
    try std.testing.expectEqual(@as(u32, 0x7000_0000), region.windowLo(1));
    try std.testing.expectEqual(@as(u32, 0x7FFF_FFFF), region.windowHi(1));
}

test "validate rejects each bad field" {
    try std.testing.expectEqual(region.ok, region.validate(0, &good0));
    var r = good0;
    r.start_addr += 1;
    try std.testing.expectEqual(region.invalid_arg, region.validate(0, &r));
    r = good0;
    r.end_addr += 0x10;
    try std.testing.expectEqual(region.invalid_arg, region.validate(0, &r));
    r = good0;
    r.start_addr = 0x8001_0000;
    try std.testing.expectEqual(region.invalid_arg, region.validate(0, &r));
    r = good0;
    r.region_id = 4;
    try std.testing.expectEqual(region.invalid_arg, region.validate(0, &r));
    try std.testing.expectEqual(region.invalid_arg, region.validate(1, &good0));
}

test "set arms the slot; overlap with the other live region is a conflict" {
    var s = fresh();
    try std.testing.expectEqual(region.ok, region.set(&s, 0, &good0));
    try std.testing.expectEqual(@as(u8, 1), s[0].region_valid[1]);
    try std.testing.expectEqual(good0.end_addr, s[0].regions[1].end_addr);
    // Fabricated: channel 1 live over an address range that hits good0.
    s[1].regions[0] = .{ .start_addr = 0x8000_8000, .end_addr = 0x8001_0000 };
    s[1].active_region_id = 0;
    try std.testing.expect(region.overlaps(&s, 0, &good0));
    s[0].region_valid[1] = 0;
    try std.testing.expectEqual(region.conflict, region.set(&s, 0, &good0));
    try std.testing.expectEqual(@as(u8, 0), s[0].region_valid[1]);
    // A channel never conflicts with itself.
    try std.testing.expect(!region.overlaps(&s, 1, &s[1].regions[0]));
}

test "select writes end then start and records the active slot" {
    var s = fresh();
    var regs = FakeRegs{};
    try std.testing.expectEqual(region.invalid_arg, region.select(&s[0], 4, &regs));
    try std.testing.expectEqual(region.invalid_state, region.select(&s[0], 1, &regs));
    try std.testing.expectEqual(@as(?state.Region, null), region.active(&s[0]));
    try std.testing.expectEqual(region.ok, region.set(&s, 0, &good0));
    try std.testing.expectEqual(region.ok, region.select(&s[0], 1, &regs));
    try std.testing.expectEqualSlices(u8, "ES", &regs.order);
    try std.testing.expectEqual([2]u32{ 0x8000_F000, 0x8000_0000 }, regs.log);
    try std.testing.expectEqual(@as(u8, 1), s[0].active_region_id);
    try std.testing.expectEqual(good0.start_addr, region.active(&s[0]).?.start_addr);
}
