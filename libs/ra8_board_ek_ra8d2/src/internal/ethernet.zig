//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The EK-RA8D2's Ethernet bring-up: everything on the chip side of the RGMII
//! link, in the order the hardware demands it. Pin routing, the ESWM clock and
//! power domain, the ETHA1 mode walk, and RMAC1. The off-board PHY's own
//! sequence lives in `eth_phy.zig`.

const busy_wait = @import("busy_wait.zig");
const eth_phy = @import("eth_phy.zig");
const hal = @import("hal.zig");
const pins = @import("eth_pins.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;
pub const Eth = vocab.Eth;

/// ~50 us settle between ETHA mode writes, in nop iterations.
pub const etha_step_iters: u32 = 200_000;

/// Route every Ethernet pin except RSTN to RGMII, then raise drive strength on
/// the six transmit pins.
fn routeAltPins() u32 {
    for (pins.routes) |route| {
        if (route.pin == pins.rstn) continue;
        const err = hal.ra8_pfs_route_peripheral(route.pin, vocab.Psel.ether_rgmii, route.owner);
        if (err != Err.ok) return err;
    }
    for (pins.tx_pins) |pin| {
        // Off target this cannot fail: the table holds only mapped RA8D2 pins.
        const err = hal.ra8_pfs_set_drive_strength(pin, vocab.Dscr.middle);
        if (err != Err.ok) return err;
    }
    return Err.ok;
}

/// Bring up ESWCLK and ESWPHYCLK, release the ESWM module stop, and run the
/// chip-generic COMA sequence. Reports the resulting ESWCLK rate, which the
/// RMAC needs to divide down for MDC.
pub fn eswmBringUp(out_eswclk_hz: *u32) u32 {
    const clk_err = hal.ra8_cgc_eswclk_init();
    if (clk_err != Err.ok) return clk_err;

    // Only fails on a null pointer, which this never is.
    const rate_err = hal.ra8_cgc_eswclk_hz(out_eswclk_hz);
    if (rate_err != Err.ok) return rate_err;

    const mstp_err = hal.ra8_mstp_enable(vocab.Mstp.eswm);
    if (mstp_err != Err.ok) return mstp_err;

    return hal.ra8_eth_coma_bringup();
}

/// Walk ETHA1 from RESET through DISABLE to CONFIG, which is the only mode in
/// which RMAC.MPIC is writable.
pub fn ethaToConfig() u32 {
    const cfg = hal.EthaConfig{
        .initial_mode = Eth.opc_reset,
        .eaeie0_mask = 0,
        .eaeie1_mask = 0,
        .eaeie2_mask = 0,
    };
    const init_err = hal.ra8_etha_init(Eth.etha_port, &cfg);
    if (init_err != Err.ok) return init_err;

    const chain = [_]u8{ Eth.opc_disable, Eth.opc_config };
    for (chain) |mode| {
        const err = setMode(mode);
        if (err != Err.ok) return err;
    }
    return Err.ok;
}

/// One ETHA mode write plus its settle dwell.
fn setMode(mode: u8) u32 {
    const err = hal.ra8_etha_set_mode(Eth.etha_port, mode);
    if (err != Err.ok) return err;
    busy_wait.spin(etha_step_iters);
    return Err.ok;
}

/// Program RMAC1: accept unicast matches and broadcast, no interrupts, and the
/// internal MII interface an external RGMII link presents to the MAC at
/// 100 Mbit full duplex (HUM Table 29.11).
fn rmacProgram(eswclk_hz: u32) u32 {
    const cfg = hal.RmacConfig{
        .rx_filter = Eth.mrafc_unicast_match | Eth.mrafc_broadcast | Eth.mrafc_bc_accept,
        .err_irq_enable = 0,
        .mon0_irq_enable = 0,
        .mon1_irq_enable = 0,
        .mon2_irq_enable = 0,
        .phy_interface = Eth.pis_mii,
        .link_speed = Eth.lsc_100mbit,
        .duplex = Eth.duplex_full,
        .eswclk_hz = eswclk_hz,
        .mdc_hz = Eth.mdc_default_hz,
    };
    return hal.ra8_rmac_init(Eth.rmac_port, &cfg);
}

/// The whole bring-up, in the one order that works.
///
/// The PHY is reset first so it never sees RGMII traffic while held down; the
/// pins are routed before the clock so nothing toggles into a dead mux; ETHA
/// reaches CONFIG before the RMAC is programmed because MPIC is read-only
/// otherwise; and ETHA only reaches OPERATION after that, because MDIO cannot
/// drive MDC until it does.
pub fn init() u32 {
    const reset_err = eth_phy.hardwareReset();
    if (reset_err != Err.ok) return reset_err;

    const route_err = routeAltPins();
    if (route_err != Err.ok) return route_err;

    var eswclk_hz: u32 = 0;
    const eswm_err = eswmBringUp(&eswclk_hz);
    if (eswm_err != Err.ok) return eswm_err;

    const config_err = ethaToConfig();
    if (config_err != Err.ok) return config_err;

    // Only fails on an out-of-range port, and the board's is fixed at 1.
    const select_err = hal.ra8_eth_rgmii_select(Eth.rmac_port);
    if (select_err != Err.ok) return select_err;

    const rmac_err = rmacProgram(eswclk_hz);
    if (rmac_err != Err.ok) return rmac_err;

    const operation_err = setMode(Eth.opc_operation);
    if (operation_err != Err.ok) return operation_err;

    return eth_phy.chipInit();
}
