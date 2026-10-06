//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! WDT control-register logic (RA8FW-891, was the last of ra8_wdt.c):
//! cfg validation and the WDTCR / WDTRCR / WDTCSTPR values ra8_wdt_init
//! writes. HUM Ch 27.2.2 "WDTCR" pp 1258-1259, Ch 27.2.4 "WDTRCR" p 1262,
//! Ch 27.2.5 "WDTCSTPR" p 1262. Exports live in src/wdt_ctrl_abi.zig.

const timing = @import("wdt_timing.zig");

/// Mirrors `ra8_wdt_cfg_t`: six `uint8_t` enums in declaration order.
pub const Cfg = extern struct {
    timeout: u8,
    clock_div: u8,
    window_start: u8,
    window_end: u8,
    on_expiry: u8,
    stop_in_sleep: u8,
};

pub const rcr_rstirqs: u8 = 0x80;
pub const cstpr_slcstp: u8 = 0x80;
pub const status_all: u16 = 0xC000; // UNDFF bit 14 | REFEF bit 15
pub const counter_mask: u16 = 0x3FFF;

/// TOPS must name a cycle count and CKS must be one of the six legal codes.
pub fn valid(cfg: Cfg) bool {
    return timing.divisor(cfg.clock_div) != null and timing.cycles(cfg.timeout) != null;
}

/// TOPS[1:0] | CKS[7:4] | RPES[9:8] | RPSS[13:12].
pub fn packWdtcr(cfg: Cfg) u16 {
    const tops: u16 = cfg.timeout & 0x3;
    const cks: u16 = cfg.clock_div & 0xF;
    const rpes: u16 = cfg.window_end & 0x3;
    const rpss: u16 = cfg.window_start & 0x3;
    return tops | (cks << 4) | (rpes << 8) | (rpss << 12);
}

/// RSTIRQS = 1 selects an internal reset; anything else routes to NMI.
pub fn rcr(cfg: Cfg) u8 {
    return if (cfg.on_expiry == 1) rcr_rstirqs else 0;
}

/// SLCSTP = 1 halts the counter in Sleep.
pub fn cstpr(cfg: Cfg) u8 {
    return if (cfg.stop_in_sleep == 1) cstpr_slcstp else 0;
}

/// A blocking clear may only name UNDFF and REFEF.
pub fn clearMaskValid(mask: u16) bool {
    return mask & ~status_all == 0;
}
