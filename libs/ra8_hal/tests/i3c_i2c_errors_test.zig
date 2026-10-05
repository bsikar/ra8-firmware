//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const e = @import("i3c_i2c_errors");

test "decode maps each BST flag to its error bit" {
    try std.testing.expectEqual(e.Err.none, e.decode(0));
    try std.testing.expectEqual(e.Err.arb_lost, e.decode(1 << 16));
    try std.testing.expectEqual(e.Err.nack, e.decode(1 << 4));
    try std.testing.expectEqual(e.Err.timeout, e.decode(1 << 20));
    try std.testing.expectEqual(@as(u8, 0x07), e.decode(e.clear_mask));
}

test "decode ignores unrelated BST bits" {
    try std.testing.expectEqual(e.Err.none, e.decode(~e.clear_mask));
}

test "clear drops only the error flags" {
    var word: u32 = 0xFFFF_FFFF;
    e.clear(&word);
    try std.testing.expectEqual(~e.clear_mask, word);
    try std.testing.expectEqual(e.Err.none, e.decode(word));
}

test "masks match inc/ra8_i3c_i2c_regs.h" {
    try std.testing.expectEqual(@as(u32, 0x0011_0010), e.clear_mask);
}
