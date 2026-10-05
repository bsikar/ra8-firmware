//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ids = @import("mstp_ids");

test "register index is the high byte" {
    try std.testing.expectEqual(@as(u8, 0), ids.reg(0x0000));
    try std.testing.expectEqual(@as(u8, 2), ids.reg(0x021F));
    try std.testing.expectEqual(@as(u8, 4), ids.reg(0x0405));
}

test "bit is the low byte" {
    try std.testing.expectEqual(@as(u8, 0x1F), ids.bit(0x021F));
    try std.testing.expectEqual(@as(u8, 5), ids.bit(0x0405));
}

test "full 16-bit range splits cleanly" {
    try std.testing.expectEqual(@as(u8, 0xFF), ids.reg(0xFFFF));
    try std.testing.expectEqual(@as(u8, 0xFF), ids.bit(0xFFFF));
}
