//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const br = @import("i2c_bitrate");

test "100 kHz from 100 MHz PCLKB needs CKS 4" {
    // 1000 cycles: halved 4 times to 62, half 31 fits, field 30.
    const r = br.solve(100_000, 100_000_000);
    try std.testing.expectEqual(@as(u8, 4), r.cks);
    try std.testing.expectEqual(@as(u8, 0xE0 | 30), r.brh);
    try std.testing.expectEqual(r.brh, r.brl);
}

test "a fast bus keeps CKS 0 and clamps the half-period to 1" {
    const r = br.solve(1_000_000, 1_000_000);
    try std.testing.expectEqual(@as(u8, 0), r.cks);
    try std.testing.expectEqual(@as(u8, 0xE0), r.brh);
}

test "pickCks stops at the divider ceiling" {
    var total: u32 = 0xFFFF_FFFF;
    try std.testing.expectEqual(br.cks_max, br.pickCks(&total));
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF >> 8), total);
}

test "clampHalf clamps to the 5-bit field" {
    try std.testing.expectEqual(@as(u8, 0), br.clampHalf(0));
    try std.testing.expectEqual(@as(u8, 0), br.clampHalf(3));
    try std.testing.expectEqual(@as(u8, 31), br.clampHalf(64));
    try std.testing.expectEqual(@as(u8, 31), br.clampHalf(1000));
}

test "icmr1WithCks replaces only CKS[6:4]" {
    try std.testing.expectEqual(@as(u8, 0x8F | (3 << 4)), br.icmr1WithCks(0xFF, 3));
    try std.testing.expectEqual(@as(u8, 0x70), br.icmr1WithCks(0x00, 7));
}
