//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The chip-side bring-up against a fake HAL. The order is the whole point:
//! every step here exists because the one before it left the hardware in the
//! only state that makes it legal, so the suite checks the sequence as much as
//! the values.

const std = @import("std");
const ethernet = @import("ethernet");

const Routed = struct { pin: u16, psel: u32 };

var routed: [24]Routed = undefined;
var routed_len: usize = 0;
var drive: [8]u16 = undefined;
var drive_len: usize = 0;
var modes: [8]u8 = undefined;
var modes_len: usize = 0;
var trace: [64]u8 = undefined;
var trace_len: usize = 0;

var route_err: u32 = 0;
var drive_err: u32 = 0;
var gpio_init_err: u32 = 0;
var eswclk_init_err: u32 = 0;
var eswclk_hz_err: u32 = 0;
var mstp_err: u32 = 0;
var coma_err: u32 = 0;
var etha_init_err: u32 = 0;
var etha_mode_err: u32 = 0;
var rgmii_select_err: u32 = 0;
var rmac_err: u32 = 0;
var mdio_err: u32 = 0;

var eswclk_hz: u32 = 125_000_000;
var mstp_id: u16 = 0;
var rgmii_port: u8 = 0xFF;
var etha_port: u8 = 0xFF;
var rmac_port: u8 = 0xFF;
var etha_cfg_mode: u8 = 0xFF;
var rmac_cfg: ?Rmac = null;

const Rmac = struct {
    rx_filter: u32,
    phy_interface: u8,
    link_speed: u8,
    duplex: u8,
    eswclk_hz: u32,
    mdc_hz: u32,
    irqs_off: bool,
};

// Step tags, so a test can assert what ran before what.
const step_gpio: u8 = 1;
const step_route: u8 = 2;
const step_eswm: u8 = 3;
const step_etha_init: u8 = 4;
const step_rgmii: u8 = 5;
const step_rmac: u8 = 6;
const step_mdio: u8 = 7;

fn note(tag: u8) void {
    trace[trace_len] = tag;
    trace_len += 1;
}

fn reset() void {
    routed_len = 0;
    drive_len = 0;
    modes_len = 0;
    trace_len = 0;
    route_err = 0;
    drive_err = 0;
    gpio_init_err = 0;
    eswclk_init_err = 0;
    eswclk_hz_err = 0;
    mstp_err = 0;
    coma_err = 0;
    etha_init_err = 0;
    etha_mode_err = 0;
    rgmii_select_err = 0;
    rmac_err = 0;
    mdio_err = 0;
    eswclk_hz = 125_000_000;
    mstp_id = 0;
    rgmii_port = 0xFF;
    etha_port = 0xFF;
    rmac_port = 0xFF;
    etha_cfg_mode = 0xFF;
    rmac_cfg = null;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u32) u32 {
    _ = pin;
    _ = init_level;
    note(step_gpio);
    return gpio_init_err;
}

export fn ra8_gpio_write(pin: u16, level: u32) u32 {
    _ = pin;
    _ = level;
    return 0;
}

export fn ra8_pfs_route_peripheral(pin: u16, psel: u32, owner: [*:0]const u8) u32 {
    _ = owner;
    note(step_route);
    if (route_err != 0) return route_err;
    routed[routed_len] = .{ .pin = pin, .psel = psel };
    routed_len += 1;
    return 0;
}

export fn ra8_pfs_set_drive_strength(pin: u16, dscr: u8) u32 {
    if (drive_err != 0) return drive_err;
    try_record: {
        if (dscr != 1) break :try_record;
        drive[drive_len] = pin;
        drive_len += 1;
    }
    return 0;
}

export fn ra8_cgc_eswclk_init() u32 {
    note(step_eswm);
    return eswclk_init_err;
}

export fn ra8_cgc_eswclk_hz(out_hz: *u32) u32 {
    if (eswclk_hz_err != 0) return eswclk_hz_err;
    out_hz.* = eswclk_hz;
    return 0;
}

export fn ra8_mstp_enable(id: u16) u32 {
    mstp_id = id;
    return mstp_err;
}

export fn ra8_eth_coma_bringup() u32 {
    return coma_err;
}

export fn ra8_etha_init(port: u8, cfg: *const EthaCfg) u32 {
    note(step_etha_init);
    etha_port = port;
    etha_cfg_mode = cfg.initial_mode;
    return etha_init_err;
}

const EthaCfg = extern struct {
    initial_mode: u8,
    eaeie0_mask: u32,
    eaeie1_mask: u32,
    eaeie2_mask: u32,
};

const RmacCfg = extern struct {
    rx_filter: u32,
    err_irq_enable: u32,
    mon0_irq_enable: u32,
    mon1_irq_enable: u32,
    mon2_irq_enable: u32,
    phy_interface: u8,
    link_speed: u8,
    duplex: u8,
    eswclk_hz: u32,
    mdc_hz: u32,
};

export fn ra8_etha_set_mode(port: u8, mode: u8) u32 {
    etha_port = port;
    if (etha_mode_err != 0) return etha_mode_err;
    modes[modes_len] = mode;
    modes_len += 1;
    return 0;
}

export fn ra8_eth_rgmii_select(port: u8) u32 {
    note(step_rgmii);
    rgmii_port = port;
    return rgmii_select_err;
}

export fn ra8_rmac_init(port: u8, cfg: *const RmacCfg) u32 {
    note(step_rmac);
    rmac_port = port;
    rmac_cfg = .{
        .rx_filter = cfg.rx_filter,
        .phy_interface = cfg.phy_interface,
        .link_speed = cfg.link_speed,
        .duplex = cfg.duplex,
        .eswclk_hz = cfg.eswclk_hz,
        .mdc_hz = cfg.mdc_hz,
        .irqs_off = cfg.err_irq_enable == 0 and cfg.mon0_irq_enable == 0 and
            cfg.mon1_irq_enable == 0 and cfg.mon2_irq_enable == 0,
    };
    return rmac_err;
}

export fn ra8_rmac_mdio_c22_write(port: u8, phy_addr: u8, reg_addr: u8, value: u16) u32 {
    _ = port;
    _ = phy_addr;
    _ = reg_addr;
    _ = value;
    note(step_mdio);
    return mdio_err;
}

export fn ra8_rmac_mdio_c22_read(port: u8, phy_addr: u8, reg_addr: u8, out_value: *u16) u32 {
    _ = port;
    _ = phy_addr;
    _ = reg_addr;
    out_value.* = 0;
    return mdio_err;
}

fn firstIndexOf(tag: u8) ?usize {
    for (trace[0..trace_len], 0..) |t, i| {
        if (t == tag) return i;
    }
    return null;
}

test "the full bring-up succeeds and reaches the PHY last" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(usize, 0), firstIndexOf(step_gpio).?);
    try std.testing.expect(firstIndexOf(step_route).? < firstIndexOf(step_eswm).?);
    try std.testing.expect(firstIndexOf(step_eswm).? < firstIndexOf(step_etha_init).?);
    try std.testing.expect(firstIndexOf(step_etha_init).? < firstIndexOf(step_rgmii).?);
    try std.testing.expect(firstIndexOf(step_rgmii).? < firstIndexOf(step_rmac).?);
    try std.testing.expect(firstIndexOf(step_rmac).? < firstIndexOf(step_mdio).?);
}

test "fifteen pins are routed to RGMII and the reset line is not one of them" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(usize, 15), routed_len);
    for (routed[0..routed_len]) |r| {
        try std.testing.expectEqual(@as(u32, 0x18), r.psel);
        try std.testing.expect(r.pin != 0x0708);
    }
}

test "only the six transmit pins get middle drive strength" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(usize, 6), drive_len);
    for (drive[0..drive_len]) |pin| {
        try std.testing.expectEqual(@as(u16, 0x03), pin >> 8);
    }
}

test "ETHA walks reset, disable, config, operation in that order" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(u8, 0), etha_cfg_mode);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 1, 2, 3 }, modes[0..modes_len]);
}

test "operation is only entered after the RMAC is programmed" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(usize, 2), modes_len - 1);
    try std.testing.expect(firstIndexOf(step_rmac).? < firstIndexOf(step_mdio).?);
}

test "the RMAC is told the real ESWCLK rate it has to divide for MDC" {
    reset();
    eswclk_hz = 96_000_000;
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(u32, 96_000_000), rmac_cfg.?.eswclk_hz);
    try std.testing.expectEqual(@as(u32, 1_000_000), rmac_cfg.?.mdc_hz);
}

test "the RMAC is programmed for 100 Mbit full duplex over the internal MII" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(u8, 0), rmac_cfg.?.phy_interface);
    try std.testing.expectEqual(@as(u8, 1), rmac_cfg.?.link_speed);
    try std.testing.expectEqual(@as(u8, 1), rmac_cfg.?.duplex);
    try std.testing.expect(rmac_cfg.?.irqs_off);
}

test "the receive filter accepts unicast matches and broadcast" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(u32, 0x0001 | 0x0001_0000 | 0x0004 | 0x0004_0000 | 0x0040), rmac_cfg.?.rx_filter);
}

test "ETHA1 and RMAC1 are the instances touched" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.init());
    try std.testing.expectEqual(@as(u8, 1), etha_port);
    try std.testing.expectEqual(@as(u8, 1), rmac_port);
    try std.testing.expectEqual(@as(u8, 1), rgmii_port);
    try std.testing.expectEqual(@as(u16, (2 << 8) | 30), mstp_id);
}

test "a pin claimed by another board function stops the bring-up" {
    reset();
    route_err = ethernet.Err.not_supported;
    try std.testing.expectEqual(ethernet.Err.not_supported, ethernet.init());
    try std.testing.expectEqual(@as(usize, 0), modes_len);
}

test "a refused drive-strength write is forwarded" {
    reset();
    drive_err = ethernet.Err.invalid_arg;
    try std.testing.expectEqual(ethernet.Err.invalid_arg, ethernet.init());
}

test "each ESWM step failure is reported and stops the sequence" {
    reset();
    eswclk_init_err = ethernet.Err.not_supported;
    try std.testing.expectEqual(ethernet.Err.not_supported, ethernet.init());

    reset();
    eswclk_hz_err = ethernet.Err.null_ptr;
    try std.testing.expectEqual(ethernet.Err.null_ptr, ethernet.init());

    reset();
    mstp_err = ethernet.Err.invalid_arg;
    try std.testing.expectEqual(ethernet.Err.invalid_arg, ethernet.init());

    reset();
    coma_err = ethernet.Err.hw_timeout;
    try std.testing.expectEqual(ethernet.Err.hw_timeout, ethernet.init());
}

test "an ETHA failure stops before the RMAC is touched" {
    reset();
    etha_init_err = ethernet.Err.invalid_arg;
    try std.testing.expectEqual(ethernet.Err.invalid_arg, ethernet.init());
    try std.testing.expectEqual(@as(?Rmac, null), rmac_cfg);

    reset();
    etha_mode_err = ethernet.Err.not_initialized;
    try std.testing.expectEqual(ethernet.Err.not_initialized, ethernet.init());
    try std.testing.expectEqual(@as(?Rmac, null), rmac_cfg);
}

test "an RMAC failure stops before any MDIO access" {
    reset();
    rmac_err = ethernet.Err.invalid_arg;
    try std.testing.expectEqual(ethernet.Err.invalid_arg, ethernet.init());
    try std.testing.expectEqual(@as(?usize, null), firstIndexOf(step_mdio));
}

test "a refused RGMII mux selection is forwarded" {
    reset();
    rgmii_select_err = ethernet.Err.invalid_arg;
    try std.testing.expectEqual(ethernet.Err.invalid_arg, ethernet.init());
}

test "eswmBringUp reports the rate on its own" {
    reset();
    var hz: u32 = 0;
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.eswmBringUp(&hz));
    try std.testing.expectEqual(@as(u32, 125_000_000), hz);
}

test "ethaToConfig stops at CONFIG, never reaching OPERATION" {
    reset();
    try std.testing.expectEqual(ethernet.Err.ok, ethernet.ethaToConfig());
    try std.testing.expectEqualSlices(u8, &[_]u8{ 1, 2 }, modes[0..modes_len]);
}
