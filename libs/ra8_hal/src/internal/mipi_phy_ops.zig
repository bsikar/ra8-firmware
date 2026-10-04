//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI D-PHY status decode and dual-mode arbitration (HUM Ch 64). Pure
//! functions over register snapshots; the C ABI and the register reads
//! live in mipi_phy_ops_abi.zig (RA8FW-575).

pub const base_addr: usize = 0x40346C00;
pub const off_sfr: usize = 0x01C;
pub const off_ocr: usize = 0x020;
pub const off_mdc: usize = 0x048;

pub const sfr_pwrsf: u32 = 0x001;
pub const sfr_pllsf: u32 = 0x100;
pub const sfr_ready_mask: u32 = sfr_pwrsf | sfr_pllsf;
pub const ocr_dphyen: u32 = 0x1;
pub const mdc_hosten: u32 = 0x1;

/// DPHYPLFCR MOSC input window, MHz (HUM 64.2.2 p 3823).
pub const mosc_min_mhz: u8 = 8;
pub const mosc_max_mhz: u8 = 48;
/// Line rate per lane = PLL output / 2 (HUM 64.2.2 p 3824).
pub const lane_rate_div: u32 = 2;
pub const hz_per_mhz: u32 = 1_000_000;

/// MSTPCRC bit 13 (provisional slot), as ra8_mstp_t (k_ra8_mstp_reg_c = 2).
pub const mstp_id: u16 = (2 << 8) | 13;

pub const mode_csi_device: u8 = 0;
pub const mode_dsi_host: u8 = 1;

pub const State = enum(u8) { off = 0, idle = 1, ldo_up = 2, pll_run = 3, run = 4 };
pub const Dual = enum(u8) { off = 0, alternate = 1, dsi_priority = 2, csi_priority = 3 };

/// Mirror of ra8_mipi_phy_status_decoded_t.
pub const Status = extern struct {
    ldo_ready: bool,
    pll_locked: bool,
    phy_ready: bool,
    raw: u32,
};

pub fn moscOk(mosc_mhz: u8) bool {
    return mosc_mhz >= mosc_min_mhz and mosc_mhz <= mosc_max_mhz;
}

pub fn decodeStatus(sfr: u32) Status {
    return .{
        .ldo_ready = (sfr & sfr_pwrsf) != 0,
        .pll_locked = (sfr & sfr_pllsf) != 0,
        .phy_ready = (sfr & sfr_ready_mask) == sfr_ready_mask,
        .raw = sfr,
    };
}

/// HUM 64.3.1 start-up order: PWRSF, then PLLSF, then DPHYEN.
pub fn state(stopped: bool, sfr: u32, ocr: u32) State {
    if (stopped) return .off;
    if ((sfr & sfr_pwrsf) == 0) return .idle;
    if ((sfr & sfr_pllsf) == 0) return .ldo_up;
    if ((ocr & ocr_dphyen) != 0) return .run;
    return .pll_run;
}

/// DPHYMDC resets to 0 (CSI device), which is also the stopped answer.
pub fn activeMode(stopped: bool, mdc: u32) u8 {
    if (stopped) return mode_csi_device;
    return if ((mdc & mdc_hosten) != 0) mode_dsi_host else mode_csi_device;
}

pub fn dualFromInt(raw: u8) ?Dual {
    return switch (raw) {
        0...3 => @enumFromInt(raw),
        else => null,
    };
}

pub fn canAcquire(dual: Dual, requestor: u8) bool {
    return switch (requestor) {
        mode_dsi_host => dual != .csi_priority,
        mode_csi_device => dual != .dsi_priority,
        else => false,
    };
}

/// Hz -> whole MHz for RFREQ; null when it overflows the 8-bit field.
pub fn pclkaMhz(hz: u32) ?u8 {
    const mhz = hz / hz_per_mhz;
    return if (mhz > 255) null else @intCast(mhz);
}
