//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J32 MIPI panel configs and its PHY -> DSI -> HS-clock bring-up.

const std = @import("std");
const mipi_panel = @import("mipi_panel");

const call_phy: u8 = 1;
const call_dsi: u8 = 2;
const call_clock: u8 = 3;

var calls: [4]u8 = .{ 0, 0, 0, 0 };
var count: usize = 0;
var fail_at: usize = 0xFF;
var seen_phy: ?*const mipi_panel.PhyConfig = null;
var seen_dsi: ?*const mipi_panel.DsiConfig = null;

const err_timeout: u16 = 0x10B;

fn record(call: u8) u16 {
    if (count < calls.len) calls[count] = call;
    count += 1;
    return if (count - 1 == fail_at) err_timeout else 0;
}

export fn ra8_mipi_phy_init(cfg: *const mipi_panel.PhyConfig) u16 {
    seen_phy = cfg;
    return record(call_phy);
}

export fn ra8_mipi_dsi_init(cfg: *const mipi_panel.DsiConfig) u16 {
    seen_dsi = cfg;
    return record(call_dsi);
}

export fn ra8_mipi_dsi_hs_clock_start() u16 {
    return record(call_clock);
}

fn reset() void {
    count = 0;
    fail_at = 0xFF;
    seen_phy = null;
    seen_dsi = null;
    calls = .{ 0, 0, 0, 0 };
}

test "bring-up runs PHY, then DSI, then the HS clock" {
    reset();
    try std.testing.expectEqual(@as(u32, 0), mipi_panel.init());
    try std.testing.expectEqual(@as(usize, 3), count);
    try std.testing.expectEqualSlices(u8, &.{ call_phy, call_dsi, call_clock }, calls[0..3]);
    try std.testing.expectEqual(&mipi_panel.phy_config, seen_phy.?);
    try std.testing.expectEqual(&mipi_panel.dsi_config, seen_dsi.?);
}

test "a PHY failure stops before the DSI link layer" {
    reset();
    fail_at = 0;
    try std.testing.expectEqual(@as(u32, err_timeout), mipi_panel.init());
    try std.testing.expectEqual(@as(usize, 1), count);
    try std.testing.expect(seen_dsi == null);
}

test "a DSI failure stops before the HS clock" {
    reset();
    fail_at = 1;
    try std.testing.expectEqual(@as(u32, err_timeout), mipi_panel.init());
    try std.testing.expectEqual(@as(usize, 2), count);
}

test "an HS clock failure is returned" {
    reset();
    fail_at = 2;
    try std.testing.expectEqual(@as(u32, err_timeout), mipi_panel.init());
    try std.testing.expectEqual(@as(usize, 3), count);
}

test "PHY config is the 2-lane DSI host at 480 Mbps from a 60 MHz PCLKA" {
    const c = mipi_panel.phy_config;
    try std.testing.expectEqual(@as(u8, 1), c.mode);
    try std.testing.expectEqual(@as(u8, 60), c.pclka_mhz);
    try std.testing.expectEqual(@as(u16, 480), c.line_rate_mbps);
    try std.testing.expectEqual(@as(u8, 2), c.lane_count);
    try std.testing.expectEqual(@as(u8, 0), c.clk_mode);
    try std.testing.expectEqual(@as(u8, 1), c.eotp);
    try std.testing.expectEqual(@as(u8, 0), c.pll.idiv);
    try std.testing.expectEqual(@as(u8, 2), c.pll.pmul);
    try std.testing.expectEqual(@as(u8, 0), c.pll.nfmul);
    try std.testing.expectEqual(@as(u16, 48), c.pll.nmul_int);
    try std.testing.expectEqual(@as(u8, 0), c.escdiv);
    try std.testing.expectEqual(@as(u32, 1), c.p_timing.tinit);
    try std.testing.expectEqual(@as(u8, 0), c.p_timing.tlpx);
}

test "DSI config is 2 lanes, ECC, EOTP and TE on, CRC on VC0 only" {
    const c = mipi_panel.dsi_config;
    try std.testing.expectEqual(@as(u8, 2), c.lane_count);
    try std.testing.expectEqual(@as(u8, 0), c.clock_mode);
    try std.testing.expectEqual(@as(u16, 16), c.max_return_packet_size);
    try std.testing.expectEqual(@as(u8, 0), c.ulps_wakeup_period);
    try std.testing.expect(c.ecc_check_enable);
    try std.testing.expect(c.eotp_enable);
    try std.testing.expect(!c.scramble_enable);
    try std.testing.expect(c.tearing_detect_enable);
    try std.testing.expectEqual(@as(u8, 0x01), c.crc_check_vc_mask);
    try std.testing.expectEqual(@as(u16, 0), c.timing.clock_stop_time);
    try std.testing.expectEqual(@as(u32, 0), c.timeouts.hs_rw_timeout);
}

test "panel geometry" {
    try std.testing.expectEqual(@as(u16, 480), mipi_panel.h_active);
    try std.testing.expectEqual(@as(u16, 854), mipi_panel.v_active);
}
