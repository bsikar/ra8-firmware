//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The GPY111 PHY against a fake MDIO seam. What matters here is that the
//! vendor register's untouched bits survive the skew write, that the soft
//! reset gives up rather than spinning forever, and that the board never
//! advertises 1000BASE-T.

const std = @import("std");
const eth_phy = @import("eth_phy");

const Access = struct { reg: u8, value: u16 };

var writes: [16]Access = undefined;
var writes_len: usize = 0;
var reads_len: usize = 0;
var write_err: u32 = 0;
var read_err: u32 = 0;
var bmcr_clears_after: u32 = 0;
var miictrl_value: u16 = 0;
var gpio_pin: u16 = 0;
var gpio_level: u32 = 0xFFFF;
var gpio_init_err: u32 = 0;
var gpio_write_err: u32 = 0;
var gpio_write_level: u32 = 0xFFFF;
var last_port: u8 = 0xFF;
var last_addr: u8 = 0xFF;

fn reset() void {
    writes_len = 0;
    reads_len = 0;
    write_err = 0;
    read_err = 0;
    bmcr_clears_after = 0;
    miictrl_value = 0;
    gpio_pin = 0;
    gpio_level = 0xFFFF;
    gpio_init_err = 0;
    gpio_write_err = 0;
    gpio_write_level = 0xFFFF;
    last_port = 0xFF;
    last_addr = 0xFF;
}

export fn ra8_rmac_mdio_c22_write(port: u8, phy_addr: u8, reg_addr: u8, value: u16) u32 {
    last_port = port;
    last_addr = phy_addr;
    if (write_err != 0) return write_err;
    writes[writes_len] = .{ .reg = reg_addr, .value = value };
    writes_len += 1;
    return 0;
}

export fn ra8_rmac_mdio_c22_read(port: u8, phy_addr: u8, reg_addr: u8, out_value: *u16) u32 {
    last_port = port;
    last_addr = phy_addr;
    if (read_err != 0) return read_err;
    reads_len += 1;
    if (reg_addr == eth_phy.Reg.miictrl) {
        out_value.* = miictrl_value;
        return 0;
    }
    const cleared = reads_len > bmcr_clears_after;
    out_value.* = if (cleared) 0 else eth_phy.Bits.bmcr_reset;
    return 0;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    if (gpio_init_err != 0) return gpio_init_err;
    gpio_pin = pin;
    gpio_level = init_level;
    return 0;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    if (gpio_write_err != 0) return gpio_write_err;
    gpio_pin = pin;
    gpio_write_level = level;
    return 0;
}

fn find(reg: u8) ?u16 {
    for (writes[0..writes_len]) |w| {
        if (w.reg == reg) return w.value;
    }
    return null;
}

test "the hardware reset drives RSTN low then releases it" {
    reset();
    try std.testing.expectEqual(eth_phy.Err.ok, eth_phy.hardwareReset());
    try std.testing.expectEqual(@as(u32, 0), gpio_level);
    try std.testing.expectEqual(@as(u32, 1), gpio_write_level);
}

test "a refused reset pin stops the sequence there" {
    reset();
    gpio_init_err = eth_phy.Err.invalid_arg;
    try std.testing.expectEqual(eth_phy.Err.invalid_arg, eth_phy.hardwareReset());
    try std.testing.expectEqual(@as(u32, 0xFFFF), gpio_write_level);
}

test "a failed release is forwarded rather than swallowed" {
    reset();
    gpio_write_err = eth_phy.Err.invalid_arg;
    try std.testing.expectEqual(eth_phy.Err.invalid_arg, eth_phy.hardwareReset());
}

test "chip init resets, skews and negotiates in that order" {
    reset();
    try std.testing.expectEqual(eth_phy.Err.ok, eth_phy.chipInit());
    try std.testing.expectEqual(@as(u8, eth_phy.Reg.bmcr), writes[0].reg);
    try std.testing.expectEqual(eth_phy.Bits.bmcr_reset, writes[0].value);
    try std.testing.expectEqual(@as(u8, eth_phy.Reg.miictrl), writes[1].reg);
    try std.testing.expectEqual(@as(u8, eth_phy.Reg.anar), writes[2].reg);
    try std.testing.expectEqual(@as(u8, eth_phy.Reg.gbcr), writes[3].reg);
    try std.testing.expectEqual(@as(u8, eth_phy.Reg.bmcr), writes[4].reg);
}

test "the skew write preserves every bit outside RXSKEW" {
    reset();
    miictrl_value = 0x8FFF;
    try std.testing.expectEqual(eth_phy.Err.ok, eth_phy.chipInit());
    const written = find(eth_phy.Reg.miictrl).?;
    try std.testing.expectEqual(eth_phy.Bits.rxskew_1p0ns, written & eth_phy.Bits.rxskew_mask);
    try std.testing.expectEqual(
        @as(u16, 0x8FFF) & ~eth_phy.Bits.rxskew_mask,
        written & ~eth_phy.Bits.rxskew_mask,
    );
}

test "1000BASE-T is never advertised" {
    reset();
    try std.testing.expectEqual(eth_phy.Err.ok, eth_phy.chipInit());
    try std.testing.expectEqual(@as(u16, 0), find(eth_phy.Reg.gbcr).?);
    try std.testing.expectEqual(eth_phy.Bits.anar_value, find(eth_phy.Reg.anar).?);
}

test "auto-negotiation is enabled and restarted in the closing BMCR write" {
    reset();
    try std.testing.expectEqual(eth_phy.Err.ok, eth_phy.chipInit());
    const last = writes[writes_len - 1];
    try std.testing.expectEqual(@as(u8, eth_phy.Reg.bmcr), last.reg);
    try std.testing.expectEqual(
        eth_phy.Bits.bmcr_an_enable | eth_phy.Bits.bmcr_an_restart,
        last.value,
    );
}

test "a BMCR.RESET that never self-clears times out instead of hanging" {
    reset();
    bmcr_clears_after = eth_phy.Reset.soft_spin + 1;
    try std.testing.expectEqual(eth_phy.Err.hw_timeout, eth_phy.chipInit());
    try std.testing.expectEqual(eth_phy.Reset.soft_spin, @as(u32, @intCast(reads_len)));
}

test "a late self-clear inside the cap still succeeds" {
    reset();
    bmcr_clears_after = eth_phy.Reset.soft_spin - 1;
    try std.testing.expectEqual(eth_phy.Err.ok, eth_phy.chipInit());
}

test "an MDIO read failure during the reset poll is forwarded" {
    reset();
    read_err = eth_phy.Err.hw_timeout;
    try std.testing.expectEqual(eth_phy.Err.hw_timeout, eth_phy.chipInit());
}

test "an MDIO write failure stops chip init at the first write" {
    reset();
    write_err = eth_phy.Err.invalid_arg;
    try std.testing.expectEqual(eth_phy.Err.invalid_arg, eth_phy.chipInit());
    try std.testing.expectEqual(@as(usize, 0), writes_len);
}

test "every MDIO access targets RMAC1 and the strapped PHY address" {
    reset();
    try std.testing.expectEqual(eth_phy.Err.ok, eth_phy.chipInit());
    try std.testing.expectEqual(@as(u8, 1), last_port);
    try std.testing.expectEqual(eth_phy.addr, last_addr);
}
