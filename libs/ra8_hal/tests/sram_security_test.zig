//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const sec = @import("sram_security");

fn blankCpscu() sec.Cpscu {
    var c: sec.Cpscu = undefined;
    @memset(std.mem.asBytes(&c), 0xAA);
    return c;
}

test "setSecurity writes SRAMSAR and rejects undefined bits" {
    var c = blankCpscu();
    try sec.setSecurity(&c, 0x10F);
    try std.testing.expectEqual(@as(u32, 0x10F), c.SRAMSAR);
    try std.testing.expectError(error.InvalidArg, sec.setSecurity(&c, 0x10));
    try std.testing.expectEqual(@as(u32, 0x10F), c.SRAMSAR);
}

test "setEccSecurity writes ESA or zero" {
    var c = blankCpscu();
    sec.setEccSecurity(&c, true);
    try std.testing.expectEqual(@as(u32, 1), c.SRAMESAR);
    sec.setEccSecurity(&c, false);
    try std.testing.expectEqual(@as(u32, 0), c.SRAMESAR);
}

test "setBoundary writes one bank and rejects bad banks and misaligned offsets" {
    var c = blankCpscu();
    try sec.setBoundary(&c, 2, 0x4000);
    try std.testing.expectEqual(@as(u32, 0x4000), c.SRAMSABAR[2]);
    try std.testing.expectEqual(@as(u32, 0xAAAA_AAAA), c.SRAMSABAR[1]);
    try std.testing.expectError(error.InvalidArg, sec.setBoundary(&c, 4, 0));
    try std.testing.expectError(error.InvalidArg, sec.setBoundary(&c, 0, 0x1000));
    try std.testing.expectEqual(@as(u32, 0xAAAA_AAAA), c.SRAMSABAR[0]);
}

const Recorder = struct {
    log: *std.ArrayList([3]usize),

    pub fn fire(r: Recorder, bank: u8, is_2bit: bool, addr: usize) void {
        r.log.append(std.testing.allocator, .{ bank, @intFromBool(is_2bit), addr }) catch unreachable;
    }
};

test "dispatchEsr fires 1-bit then 2-bit per bank and returns the fired mask" {
    var log: std.ArrayList([3]usize) = .empty;
    defer log.deinit(std.testing.allocator);
    var s: sec.Status = .{ .raw_esr = 0b1100_0001 };
    s.addr_1bit = .{ 0x100, 0, 0, 0x400 };
    s.addr_2bit = .{ 0, 0, 0, 0x440 };
    const fired = sec.dispatchEsr(&s, Recorder{ .log = &log });
    try std.testing.expectEqual(@as(u16, 0b1100_0001), fired);
    try std.testing.expectEqualSlices([3]usize, &.{ .{ 0, 0, 0x100 }, .{ 3, 0, 0x400 }, .{ 3, 1, 0x440 } }, log.items);
}

test "dispatchEsr ignores bits above the four banks" {
    var log: std.ArrayList([3]usize) = .empty;
    defer log.deinit(std.testing.allocator);
    const s: sec.Status = .{ .raw_esr = 0xFF00 };
    try std.testing.expectEqual(@as(u16, 0), sec.dispatchEsr(&s, Recorder{ .log = &log }));
    try std.testing.expectEqual(@as(usize, 0), log.items.len);
}
