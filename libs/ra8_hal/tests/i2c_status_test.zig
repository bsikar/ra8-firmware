//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const s = @import("i2c_status");

test "decode maps each ICSR2 flag to its error bit" {
    try std.testing.expectEqual(s.Err.none, s.decode(0));
    try std.testing.expectEqual(s.Err.arb_lost, s.decode(1 << 1));
    try std.testing.expectEqual(s.Err.nack, s.decode(1 << 4));
    try std.testing.expectEqual(s.Err.timeout, s.decode(1 << 0));
    try std.testing.expectEqual(@as(u8, 0x07), s.decode(s.clear_mask));
}

test "decode ignores unrelated ICSR2 bits" {
    try std.testing.expectEqual(s.Err.none, s.decode(~s.clear_mask));
}

test "clear drops only the error flags" {
    var reg: u8 = 0xFF;
    s.clear(&reg);
    try std.testing.expectEqual(~s.clear_mask, reg);
    try std.testing.expectEqual(s.Err.none, s.decode(reg));
}

test "icsr2Addr follows the ra8_i2c_regs channel table" {
    try std.testing.expectEqual(@as(?usize, 0x4025E009), s.icsr2Addr(0));
    try std.testing.expectEqual(@as(?usize, 0x4025E209), s.icsr2Addr(2));
    try std.testing.expectEqual(@as(?usize, null), s.icsr2Addr(3));
    try std.testing.expectEqual(@as(?usize, null), s.icsr2Addr(0xFF));
}

test "clear mask matches the C enum (AL | NACKF | 1 << TMOF)" {
    try std.testing.expectEqual(@as(u8, 0x13), s.clear_mask);
}
