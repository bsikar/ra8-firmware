//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/spi_b_setup.zig (RA8FW-898).

const std = @import("std");
const setup = @import("spi_b_setup");

fn cfgFor(mode: u8, lsb_first: bool, loopback: bool) setup.Cfg {
    var c = setup.default_cfg;
    c.mode = mode;
    c.lsb_first = lsb_first;
    c.loopback = loopback;
    return c;
}

test "cfg mirrors ra8_spi_cfg_t" {
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(setup.Cfg, "mode"));
    try std.testing.expectEqual(@as(usize, 10), @offsetOf(setup.Cfg, "loopback"));
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(setup.Cfg));
}

test "SPCMD0 follows the SPI mode, bit order and 8-bit frame" {
    try std.testing.expectEqual(@as(u32, 0x0007_0000), setup.spcmd(cfgFor(0, false, false)));
    try std.testing.expectEqual(@as(u32, 0x0007_0001), setup.spcmd(cfgFor(1, false, false)));
    try std.testing.expectEqual(@as(u32, 0x0007_0002), setup.spcmd(cfgFor(2, false, false)));
    try std.testing.expectEqual(@as(u32, 0x0007_1003), setup.spcmd(cfgFor(3, true, false)));
}

test "SPCR enables the controller and SPCR2 follows loopback" {
    try std.testing.expectEqual(@as(u32, 0x4000_1001), setup.spcrController());
    try std.testing.expectEqual(@as(u32, 0), setup.spcr2(cfgFor(0, false, false)));
    try std.testing.expectEqual(@as(u32, 0x0002_0000), setup.spcr2(cfgFor(0, false, true)));
}

test "defaults are 1.9 MHz at PCLKA 125 MHz, mode 0, MSB first" {
    try std.testing.expectEqual(@as(u32, 1_900_000), setup.default_cfg.baud_hz);
    try std.testing.expectEqual(@as(u32, 125_000_000), setup.default_cfg.pclka_hz);
    try std.testing.expectEqual(@as(u8, 0), setup.default_cfg.mode);
}
