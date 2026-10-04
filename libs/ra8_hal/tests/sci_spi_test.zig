//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/sci_spi.zig.

const std = @import("std");
const spi = @import("sci_spi");

test "Cfg mirrors ra8_sci_spi_cfg_t" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(spi.Cfg));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(spi.Cfg, "mode"));
    try std.testing.expectEqual(@as(usize, 9), @offsetOf(spi.Cfg, "lsb_first"));
}

test "regAddr and channel bound cover SCI0..SCI9" {
    try std.testing.expectEqual(@as(usize, 0x40358948), spi.regAddr(9, spi.off_csr));
    try std.testing.expect(spi.channelOk(9));
    try std.testing.expect(!spi.channelOk(10));
}

test "mstpId maps MSTPB31 down to MSTPB22" {
    try std.testing.expectEqual(@as(u16, 0x011F), spi.mstpId(0));
    try std.testing.expectEqual(@as(u16, 0x0116), spi.mstpId(9));
}

test "resolveRate picks the first CKS whose BRR fits" {
    // 120 MHz / (4 * 1 MHz) = 30 -> CKS 0, BRR 29.
    try std.testing.expectEqual(spi.Rate{ .cks = 0, .brr = 29 }, spi.resolveRate(1_000_000, 120_000_000));
    // 120 MHz / (4 * 100 kHz) = 300 > 256 -> CKS 1: 120 MHz / (16 * 100 kHz) = 75.
    try std.testing.expectEqual(spi.Rate{ .cks = 1, .brr = 74 }, spi.resolveRate(100_000, 120_000_000));
}

test "resolveRate saturates at both ends" {
    try std.testing.expectEqual(spi.Rate{ .cks = 0, .brr = 0 }, spi.resolveRate(100_000_000, 120_000_000));
    try std.testing.expectEqual(spi.Rate{ .cks = 3, .brr = 255 }, spi.resolveRate(1, 120_000_000));
    try std.testing.expectEqual(spi.Rate{ .cks = 0, .brr = 0 }, spi.resolveRate(1_000_000, 0));
}

test "ccr2 packs BRR, CKS and the MDDR reset value" {
    try std.testing.expectEqual(@as(u32, 0xFF10_4A00), spi.ccr2(100_000, 120_000_000));
}

test "ccr3 encodes simple-SPI, 8-bit, mode and bit order" {
    const m0 = spi.Cfg{ .baud_hz = 1, .pclk_hz = 1, .mode = 0, .lsb_first = false };
    try std.testing.expectEqual(@as(u32, 0x0003_0200), spi.ccr3(m0));
    const m3 = spi.Cfg{ .baud_hz = 1, .pclk_hz = 1, .mode = 3, .lsb_first = true };
    try std.testing.expectEqual(@as(u32, 0x0003_1203), spi.ccr3(m3));
}
