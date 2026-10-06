//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/spi_b_clock.zig (RA8FW-892).

const std = @import("std");
const clock = @import("spi_b_clock");

test "SPBR follows PCLKA / (2 * baud) - 1" {
    try std.testing.expectEqual(@as(u8, 31), clock.spbr(1_900_000, 125_000_000));
    try std.testing.expectEqual(@as(u8, 0), clock.spbr(62_500_000, 125_000_000));
}

test "SPBR is 0 for unusable inputs and clamps at 0xFF" {
    try std.testing.expectEqual(@as(u8, 0), clock.spbr(0, 125_000_000));
    try std.testing.expectEqual(@as(u8, 0), clock.spbr(1_000_000, 0));
    try std.testing.expectEqual(@as(u8, 0), clock.spbr(200_000_000, 125_000_000));
    try std.testing.expectEqual(@as(u8, 0xFF), clock.spbr(1_000, 125_000_000));
}

test "SPCR3 keeps every bit outside SPBR" {
    try std.testing.expectEqual(@as(u32, 0x0700_1F0F), clock.withSpbr(0x0700_AB0F, 0x1F));
}

test "SPSR error flags map to the k_ra8_spi_err bits" {
    try std.testing.expectEqual(@as(u8, 0), clock.errMask(0xE080_0000));
    try std.testing.expectEqual(@as(u8, 0x0F), clock.errMask(0x1D00_0000));
    try std.testing.expectEqual(@as(u8, 0x02), clock.errMask(0x0400_0000));
    try std.testing.expectEqual(@as(u8, 0x09), clock.errMask(0x1100_0000));
}
