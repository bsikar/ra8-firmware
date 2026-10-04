//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/mipi_phy_ops.zig.

const std = @import("std");
const ops = @import("mipi_phy_ops");

test "Status mirrors ra8_mipi_phy_status_decoded_t" {
    try std.testing.expectEqual(@as(usize, 8), @sizeOf(ops.Status));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(ops.Status, "phy_ready"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(ops.Status, "raw"));
}

test "decodeStatus splits PWRSF and PLLSF and keeps the raw word" {
    const s = ops.decodeStatus(0xDEAD_0101);
    try std.testing.expect(s.ldo_ready and s.pll_locked and s.phy_ready);
    try std.testing.expectEqual(@as(u32, 0xDEAD_0101), s.raw);
    const half = ops.decodeStatus(0x001);
    try std.testing.expect(half.ldo_ready and !half.pll_locked and !half.phy_ready);
}

test "state follows the start-up order" {
    try std.testing.expectEqual(ops.State.off, ops.state(true, 0x101, 1));
    try std.testing.expectEqual(ops.State.idle, ops.state(false, 0x100, 1));
    try std.testing.expectEqual(ops.State.ldo_up, ops.state(false, 0x001, 1));
    try std.testing.expectEqual(ops.State.pll_run, ops.state(false, 0x101, 0));
    try std.testing.expectEqual(ops.State.run, ops.state(false, 0x101, 1));
}

test "activeMode reads HOSTEN and reports CSI while stopped" {
    try std.testing.expectEqual(ops.mode_csi_device, ops.activeMode(true, 1));
    try std.testing.expectEqual(ops.mode_dsi_host, ops.activeMode(false, 1));
    try std.testing.expectEqual(ops.mode_csi_device, ops.activeMode(false, 0xFFFF_FFFE));
}

test "dual-mode values and arbitration" {
    try std.testing.expectEqual(ops.Dual.csi_priority, ops.dualFromInt(3).?);
    try std.testing.expect(ops.dualFromInt(4) == null);
    try std.testing.expect(!ops.canAcquire(.csi_priority, ops.mode_dsi_host));
    try std.testing.expect(ops.canAcquire(.dsi_priority, ops.mode_dsi_host));
    try std.testing.expect(!ops.canAcquire(.dsi_priority, ops.mode_csi_device));
    try std.testing.expect(ops.canAcquire(.alternate, ops.mode_csi_device));
    try std.testing.expect(!ops.canAcquire(.off, 2));
}

test "MOSC window and PCLKA MHz conversion" {
    try std.testing.expect(!ops.moscOk(7));
    try std.testing.expect(ops.moscOk(8) and ops.moscOk(48));
    try std.testing.expect(!ops.moscOk(49));
    try std.testing.expectEqual(@as(u8, 120), ops.pclkaMhz(120_999_999).?);
    try std.testing.expectEqual(@as(u8, 255), ops.pclkaMhz(255_999_999).?);
    try std.testing.expect(ops.pclkaMhz(256_000_000) == null);
}
