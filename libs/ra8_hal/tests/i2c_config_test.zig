//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const c = @import("i2c_config");

test "mstpId follows k_ra8_mstp_iicN" {
    try std.testing.expectEqual(@as(u16, 0x109), c.mstpId(0));
    try std.testing.expectEqual(@as(u16, 0x108), c.mstpId(1));
    try std.testing.expectEqual(@as(u16, 0x107), c.mstpId(2));
    try std.testing.expectEqual(@as(u16, 0x107), c.mstpId(0xFF));
}

test "regsAddr follows the ra8_i2c_regs channel table" {
    try std.testing.expectEqual(@as(?usize, 0x4025E000), c.regsAddr(0));
    try std.testing.expectEqual(@as(?usize, 0x4025E100), c.regsAddr(1));
    try std.testing.expectEqual(@as(?usize, 0x4025E200), c.regsAddr(2));
    try std.testing.expectEqual(@as(?usize, null), c.regsAddr(3));
}

test "icferValue sets MALE, NACKE, SCLE and FMPE only for Fm+" {
    try std.testing.expectEqual(@as(u8, 0x52), c.icferValue(false));
    try std.testing.expectEqual(@as(u8, 0xD2), c.icferValue(true));
}

test "applyInit leaves the channel out of reset with the rate programmed" {
    var block: [c.regs_span]u8 = @splat(0xAA);
    c.applyInit(@ptrCast(&block), .{ .cks = 3, .brh = 0x12, .brl = 0x34 }, false);
    try std.testing.expectEqual(c.iccr1_ice, block[c.off_iccr1]);
    try std.testing.expectEqual(@as(u8, 0x30), block[c.off_icmr1]);
    try std.testing.expectEqual(@as(u8, 0x34), block[c.off_icbrl]);
    try std.testing.expectEqual(@as(u8, 0x12), block[c.off_icbrh]);
    try std.testing.expectEqual(@as(u8, 0x52), block[c.off_icfer]);
}

test "applyInit enables FMPE at Fast-mode Plus" {
    var block: [c.regs_span]u8 = @splat(0);
    c.applyInit(@ptrCast(&block), .{ .cks = 0, .brh = 1, .brl = 1 }, true);
    try std.testing.expectEqual(@as(u8, 0xD2), block[c.off_icfer]);
}

test "applyInit truncates CKS into ICMR1 like the C cast" {
    var block: [c.regs_span]u8 = @splat(0);
    c.applyInit(@ptrCast(&block), .{ .cks = 7, .brh = 0, .brl = 0 }, false);
    try std.testing.expectEqual(@as(u8, 0x70), block[c.off_icmr1]);
}

test "applyInit leaves bytes outside the sequence alone" {
    var block: [c.regs_span]u8 = @splat(0xAA);
    c.applyInit(@ptrCast(&block), .{ .cks = 1, .brh = 2, .brl = 3 }, false);
    try std.testing.expectEqual(@as(u8, 0xAA), block[0x01]);
    try std.testing.expectEqual(@as(u8, 0xAA), block[0x03]);
    try std.testing.expectEqual(@as(u8, 0xAA), block[0x09]);
}
