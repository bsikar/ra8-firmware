//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ESWM media mux: select a port's RGMII mode and release its block.
//!
//! [Ring 2 / HAL] {World: S}
//!
//! The media half of the Ethernet HAL, kept apart from the frame-level NIC
//! API in ra8_eth.c. Choosing how a port talks to its PHY, and taking that
//! port's interface block out of reset, happens once during bring-up and has
//! nothing in common with opening a NIC or moving frames.
//!
//! The register window is a parameter, so the write order can be tested on
//! ordinary memory; `hardware()` is the one place the ESWM addresses live,
//! matching inc/ra8_ether_regs.h.

/// ESWM register block base (inc/ra8_ether_regs.h `k_ra8_eswm_base_addr`).
pub const eswm_base: usize = 0x403C_8000;
/// MIIRR, MIICR0 and MIICR1 offsets from the ESWM base.
pub const off_miirr: usize = 0x1_9400;
pub const off_miicr0: usize = 0x1_9404;
pub const off_miicr1: usize = 0x1_9408;

/// MIICR.MIISEL = RGMII (bits 1:0).
pub const miicr_miisel_rgmii: u32 = 1;
/// MIICR.TXCIDE: TXC internal delay.
pub const miicr_txcide: u32 = 1 << 12;
/// MIIRR.RGRSTm: 0 = reset, 1 = enable.
pub const miirr_rgrst0: u32 = 1 << 0;
pub const miirr_rgrst1: u32 = 1 << 1;

/// Highest valid `ra8_eth_mii_port_t` (`k_ra8_eth_mii_port_1`).
pub const port_max: u8 = 1;

pub const Error = error{InvalidPort};

/// The three media-interface registers one select touches.
pub const Window = struct {
    miirr: *volatile u32,
    miicr0: *volatile u32,
    miicr1: *volatile u32,
};

/// The real ESWM registers.
pub fn hardware() Window {
    return .{
        .miirr = @ptrFromInt(eswm_base + off_miirr),
        .miicr0 = @ptrFromInt(eswm_base + off_miicr0),
        .miicr1 = @ptrFromInt(eswm_base + off_miicr1),
    };
}

/// Route `port`'s pins to RGMII, then bring its RGMII block out of reset.
///
/// MIICR is written before MIIRR.RGRSTm is set so the pin mux is in the
/// right mode the instant the data pins go live.
/// HUM Ch 29 "Layer 3 Ethernet Switch Module (ESWM)" p 1287;
/// HUM Ch 29.2.1.2 "MIIRR : Media-independent Interface Reset Register" p 1289.
pub fn rgmiiSelect(window: Window, port: u8) Error!void {
    if (port > port_max) return error.InvalidPort;
    const miicr = if (port == 0) window.miicr0 else window.miicr1;
    const rgrst = if (port == 0) miirr_rgrst0 else miirr_rgrst1;
    miicr.* = miicr_txcide | miicr_miisel_rgmii;
    window.miirr.* = window.miirr.* | rgrst;
}
