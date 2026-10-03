//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const iwdt = @import("iwdt");

var regs: [8]u8 align(4) = undefined;

fn fake(sr_value: u16) iwdt.Window {
    @memset(&regs, 0xAA);
    std.mem.writeInt(u16, regs[iwdt.off_iwdtsr..][0..2], sr_value, .little);
    return .{ .base = @intFromPtr(&regs) };
}

fn sr() u16 {
    return std.mem.readInt(u16, regs[iwdt.off_iwdtsr..][0..2], .little);
}

test "refresh leaves 0xFF in IWDTRR after the 0x00 step and touches nothing else" {
    const w = fake(0x1234);
    iwdt.refresh(w);
    try std.testing.expectEqual(iwdt.refresh_b, regs[iwdt.off_iwdtrr]);
    try std.testing.expectEqual(@as(u8, 0xAA), regs[1]);
    try std.testing.expectEqual(@as(u16, 0x1234), sr());
}

test "status keeps only UNDFF and REFEF; counter keeps only CNTVAL" {
    const w = fake(0xFFFF);
    try std.testing.expectEqual(@as(u16, 0xC000), iwdt.status(w));
    try std.testing.expectEqual(@as(u16, 0x3FFF), iwdt.counter(w));
    _ = fake(0x4123);
    try std.testing.expectEqual(iwdt.status_underflow, iwdt.status(w));
    try std.testing.expectEqual(@as(u16, 0x0123), iwdt.counter(w));
}

test "clearStatus zeroes both flags and writes CNTVAL back unchanged" {
    const w = fake(0xC5A5);
    iwdt.clearStatus(w, iwdt.status_all);
    try std.testing.expectEqual(@as(u16, 0x05A5), sr());
}

const Seen = struct { calls: u32 = 0, mask: u16 = 0 };

fn record(ctx: ?*anyopaque, mask: u16) callconv(.c) void {
    const seen: *Seen = @ptrCast(@alignCast(ctx.?));
    seen.calls += 1;
    seen.mask = mask;
}

test "dispatch clears the latched flags before the handler sees the snapshot" {
    var seen: Seen = .{};
    const w = fake(0x8007);
    iwdt.dispatch(w, .{ .func = record, .ctx = &seen });
    try std.testing.expectEqual(@as(u32, 1), seen.calls);
    try std.testing.expectEqual(iwdt.status_refresh, seen.mask);
    try std.testing.expectEqual(@as(u16, 0x0007), sr());
}

test "dispatch with no handler still clears the flags" {
    const w = fake(0x4001);
    iwdt.dispatch(w, .{});
    try std.testing.expectEqual(@as(u16, 0x0001), sr());
}
