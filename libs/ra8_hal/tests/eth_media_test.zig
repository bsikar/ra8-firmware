//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! eth_media against a register window in ordinary memory.

const std = @import("std");
const eth_media = @import("eth_media");

const Regs = struct {
    miirr: u32 = 0,
    miicr0: u32 = 0,
    miicr1: u32 = 0,

    fn window(self: *Regs) eth_media.Window {
        return .{ .miirr = &self.miirr, .miicr0 = &self.miicr0, .miicr1 = &self.miicr1 };
    }
};

const rgmii_mode = eth_media.miicr_txcide | eth_media.miicr_miisel_rgmii;

test "port 1 selects RGMII on MIICR1 and enables RGRST1 only" {
    var regs = Regs{};
    try eth_media.rgmiiSelect(regs.window(), 1);
    try std.testing.expectEqual(rgmii_mode, regs.miicr1);
    try std.testing.expectEqual(@as(u32, 0), regs.miicr0);
    try std.testing.expectEqual(eth_media.miirr_rgrst1, regs.miirr);
}

test "port 0 selects RGMII on MIICR0 and enables RGRST0 only" {
    var regs = Regs{};
    try eth_media.rgmiiSelect(regs.window(), 0);
    try std.testing.expectEqual(rgmii_mode, regs.miicr0);
    try std.testing.expectEqual(@as(u32, 0), regs.miicr1);
    try std.testing.expectEqual(eth_media.miirr_rgrst0, regs.miirr);
}

test "MIIRR keeps the other port's enable bits" {
    var regs = Regs{ .miirr = eth_media.miirr_rgrst0 | (1 << 8) };
    try eth_media.rgmiiSelect(regs.window(), 1);
    const want = eth_media.miirr_rgrst0 | eth_media.miirr_rgrst1 | (1 << 8);
    try std.testing.expectEqual(want, regs.miirr);
}

test "an out-of-range port is refused and touches nothing" {
    var regs = Regs{};
    try std.testing.expectError(error.InvalidPort, eth_media.rgmiiSelect(regs.window(), 2));
    try std.testing.expectEqual(Regs{}, regs);
}

test "hardware window matches the ESWM map in ra8_ether_regs.h" {
    const window = eth_media.hardware();
    try std.testing.expectEqual(@as(usize, 0x403E_1400), @intFromPtr(window.miirr));
    try std.testing.expectEqual(@as(usize, 0x403E_1404), @intFromPtr(window.miicr0));
    try std.testing.expectEqual(@as(usize, 0x403E_1408), @intFromPtr(window.miicr1));
}
