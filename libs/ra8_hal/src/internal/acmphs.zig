//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! High-speed analog comparator helpers (HUM Ch 56). Pure code; the C
//! ABI and register access live in acmphs_abi.zig (RA8FW-578).

/// FSP R_ACMPHS0_BASE; channels sit 0x100 apart.
pub const base: usize = 0x40236000;
pub const stride: usize = 0x100;
pub const channel_count: u8 = 6;

pub const off_cmpctl: usize = 0x00;
pub const off_cmpsel0: usize = 0x04;
pub const off_cmpsel1: usize = 0x08;
pub const off_cmpmon: usize = 0x0C;
pub const off_cpioc: usize = 0x10;

pub const mask_cinv: u8 = 0x01;
pub const mask_coe: u8 = 0x02;
pub const mask_ceg: u8 = 0x18;
pub const mask_cdfs: u8 = 0x60;
pub const mask_hcen: u8 = 0x80;
/// CMPMON.CMPMON, the comparator output level.
pub const mask_hcmon: u8 = 0x01;
/// The CMPCTL bits ra8_acmphs_get_status reports.
pub const ctl_mask: u8 = mask_hcen | mask_ceg | mask_cinv | mask_coe | mask_cdfs;

const ceg_shift: u3 = 3;
const cdfs_shift: u3 = 5;
const cdfs_enabled: u8 = 1;

pub const level_low: u8 = 0;
pub const level_high: u8 = 1;

/// MSTPD28..25 (k_ra8_mstp_reg_d = 3) for channels 0..3; 4 and 5 have
/// no module-stop bit on this package.
pub const mstp_ids = [_]u16{ (3 << 8) | 28, (3 << 8) | 27, (3 << 8) | 26, (3 << 8) | 25 };

/// Mirror of ra8_acmphs_cfg_t.
pub const Cfg = extern struct {
    ivpsel: u8,
    ivrefsel: u8,
    edge: u8,
    filter_en: bool,
    invert_out: bool,
};

pub fn channelOk(channel: u8) bool {
    return channel < channel_count;
}

pub fn regAddr(channel: u8, offset: usize) usize {
    return base + @as(usize, channel) * stride + offset;
}

/// The module-stop id for a channel, or null when it has none.
pub fn mstpId(channel: u8) ?u16 {
    if (channel >= mstp_ids.len) return null;
    return mstp_ids[channel];
}

/// CMPCTL for channel_init: enabled, edge in CEG[4:3], optional
/// inversion, filter mapped to CDFS = 1 (there is no CMPFIR on RA8D2).
pub fn packCtl(cfg: Cfg) u8 {
    var ctl: u8 = mask_hcen;
    ctl |= cfg.edge << ceg_shift;
    if (cfg.invert_out) ctl |= mask_cinv;
    if (cfg.filter_en) ctl |= cdfs_enabled << cdfs_shift;
    return ctl;
}

pub fn levelOf(cmpmon: u8) u8 {
    return if (cmpmon & mask_hcmon != 0) level_high else level_low;
}
