//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const gamma = @import("glcdc_gamma");

var regs: [0x1400 / 4]u32 = undefined;

fn fake() gamma.Window {
    @memset(&regs, 0xDEADBEEF);
    return .{ .base = @intFromPtr(&regs) };
}

fn at(offset: usize) u32 {
    return regs[offset / 4];
}

fn ramp(start: u16) [gamma.lut_depth]u16 {
    var t: [gamma.lut_depth]u16 = undefined;
    for (&t, 0..) |*v, i| v.* = start + @as(u16, @intCast(i));
    return t;
}

test "validate takes channels 0..2 and only a count of 16" {
    try gamma.validate(0, 16);
    try gamma.validate(2, 16);
    try std.testing.expectError(error.ChannelOutOfRange, gamma.validate(3, 16));
    try std.testing.expectError(error.BadCount, gamma.validate(1, 15));
    try std.testing.expectError(error.BadCount, gamma.validate(1, 17));
}

test "pack puts entry 2i high and masks only entry 2i+1" {
    var t = ramp(0);
    t[0] = 0xFFFF;
    t[1] = 0xFFFF;
    try std.testing.expectEqual(@as(u32, 0xFFFF07FF), gamma.pack(&t, 0, gamma.gain_h_shift, gamma.gain_l_mask));
    try std.testing.expectEqual(@as(u32, 0xFFFF03FF), gamma.pack(&t, 0, gamma.area_h_shift, gamma.area_l_mask));
    try std.testing.expectEqual(@as(u32, 0x0002_0003), gamma.pack(&t, 1, 16, 0x7FF));
}

test "writeTables fills the green block's LUT then AREA and nothing else" {
    const w = fake();
    const gain = ramp(0x100);
    const threshold = ramp(0x20);
    gamma.writeTables(w, 1, &gain, &threshold);
    for (0..gamma.reg_count) |i| {
        try std.testing.expectEqual(gamma.pack(&gain, i, 16, 0x7FF), at(0x1340 + 4 * i));
        try std.testing.expectEqual(gamma.pack(&threshold, i, 16, 0x3FF), at(0x1360 + 4 * i));
    }
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), at(0x1300));
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), at(0x133C));
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), at(0x1380));
}

test "writeTables addresses red and blue at 0x1300 and 0x1380" {
    const w = fake();
    const t = ramp(1);
    gamma.writeTables(w, 0, &t, &t);
    try std.testing.expectEqual(@as(u32, 0x0001_0002), at(0x1300));
    gamma.writeTables(w, 2, &t, &t);
    try std.testing.expectEqual(@as(u32, 0x000F_0010), at(0x13BC));
}

test "setEnable writes GAMON or zero to OUT_GAMSW" {
    const w = fake();
    gamma.setEnable(w, true);
    try std.testing.expectEqual(@as(u32, 1), at(0x13D8));
    gamma.setEnable(w, false);
    try std.testing.expectEqual(@as(u32, 0), at(0x13D8));
    try std.testing.expectEqual(@as(u32, 0xDEADBEEF), at(0x13D4));
}
