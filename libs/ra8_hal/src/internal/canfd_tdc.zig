//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CANFD Transmitter Delay Compensation (RA8FW-532, ported from
//! ra8_canfd_tdc.c). The register values match inc/ra8_canfd_regs.h.

/// CANFD channel bases (`k_ra8_canfd0_base_addr`, `k_ra8_canfd1_base_addr`).
/// HUM Ch 41 p 2702.
pub const channel_bases = [_]usize{ 0x4038_0000, 0x4038_2000 };
/// CFDC2[0].FDCFG: CFDC2 at 0x100, FDCFG at +0x04. HUM Ch 41 "CFDCnFDCFG" p 2788.
pub const off_fdcfg: usize = 0x104;

/// TDCO[14:8]: 7-bit offset (`k_ra8_fdcfg_mask_tdco`, `k_ra8_fdcfg_shift_tdco`).
pub const tdco_mask: u32 = 0x7F;
pub const tdco_shift: u5 = 8;
/// TDCOC[15]: use TDCO instead of the measured value.
pub const tdcoc: u32 = 1 << 15;
/// TDE[16]: TDC enable.
pub const tde: u32 = 1 << 16;
/// Largest TDCO (`k_ra8_canfd_tdc_offset_max`).
pub const offset_max: u8 = 127;

/// `ra8_canfd_tdc_cfg_t` (inc/ra8_canfd.h).
pub const Cfg = extern struct {
    enable: bool,
    manual: bool,
    offset: u8,
};

/// The channel's register block base, or null when out of range.
pub fn channelBase(channel: u8) ?usize {
    if (channel >= channel_bases.len) return null;
    return channel_bases[channel];
}

/// FDCFG after applying `cfg` to `old`. FDOE, REFE, CLOE, ESIC and EOCCFG are
/// kept; TDE, TDCOC and the TDCO slot are cleared, then re-stamped when
/// enabled.
pub fn fdcfgValue(old: u32, cfg: Cfg) u32 {
    var value = old & ~(tde | tdcoc | (tdco_mask << tdco_shift));
    if (cfg.enable) {
        value |= tde;
        value |= (@as(u32, cfg.offset) & tdco_mask) << tdco_shift;
        if (cfg.manual) value |= tdcoc;
    }
    return value;
}
