//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CAN FD acceptance filter list (RA8FW-583), ported from ra8_canfd_afl.c.
//! Validation, word packing and the write sequence for page 0; register
//! access comes in through a `regs` value so the host tests can record it.

pub const ok: u16 = 0x000;
pub const invalid_arg: u16 = 0x103;

/// Mirrors ra8_canfd_afl_rule_t (12 bytes).
pub const Rule = extern struct {
    id: u32,
    mask: u32,
    extended: bool,
    rtr: bool,
    target_rx: u8,
};

pub const rule_capacity: u8 = 16;
pub const rx_fifo_count: u8 = 2;

pub const id_std_mask: u32 = 0x0000_07FF;
pub const id_ext_mask: u32 = 0x1FFF_FFFF;
pub const id_rtr: u32 = 1 << 30;
pub const id_ide: u32 = 1 << 31;
pub const gaflm_rtrm: u32 = 1 << 30;
pub const gaflm_idem: u32 = 1 << 31;
pub const gaflp1_fdp0: u32 = 1 << 0;
pub const ectr_aflpn_mask: u32 = 0xF;
pub const ectr_afldae: u32 = 1 << 8;
pub const cfg0_rnc0_shift: u5 = 16;
pub const cfg0_rnc0_mask: u32 = 0x1F;
const page0: u32 = 0;

/// HUM Ch 41.2.17 to 41.2.22 (p 2734 to 2740).
pub const off_ectr: usize = 0x028;
pub const off_cfg0: usize = 0x02C;
pub const off_gafl: usize = 0x120;
pub const gafl_stride: usize = 16;

fn idMask(rule: Rule) u32 {
    return if (rule.extended) id_ext_mask else id_std_mask;
}

pub fn validateRule(rule: Rule) u16 {
    if (rule.target_rx >= rx_fifo_count) return invalid_arg;
    const m = idMask(rule);
    if (rule.id & ~m != 0 or rule.mask & ~m != 0) return invalid_arg;
    return ok;
}

pub fn validate(rules: []const Rule) u16 {
    for (rules) |r| {
        const v = validateRule(r);
        if (v != ok) return v;
    }
    return ok;
}

pub fn idWord(rule: Rule) u32 {
    var w = rule.id & idMask(rule);
    if (rule.extended) w |= id_ide;
    if (rule.rtr) w |= id_rtr;
    return w;
}

pub fn maskWord(rule: Rule) u32 {
    return (rule.mask & idMask(rule)) | gaflm_idem | gaflm_rtrm;
}

pub fn p1Word(rule: Rule) u32 {
    return gaflp1_fdp0 << @intCast(rule.target_rx);
}

pub fn cfg0With(cfg0: u32, count: u8) u32 {
    const field = cfg0_rnc0_mask << cfg0_rnc0_shift;
    return (cfg0 & ~field) | ((@as(u32, count) & cfg0_rnc0_mask) << cfg0_rnc0_shift);
}

/// Opens the AFL window on page 0, sets RNC0, writes each slot, re-locks.
/// The caller has already validated `rules` (1..rule_capacity entries).
pub fn program(regs: anytype, rules: []const Rule) void {
    regs.write(off_ectr, (page0 & ectr_aflpn_mask) | ectr_afldae);
    regs.write(off_cfg0, cfg0With(regs.read(off_cfg0), @intCast(rules.len)));
    for (rules, 0..) |r, i| {
        const slot = off_gafl + i * gafl_stride;
        regs.write(slot + 0x0, idWord(r));
        regs.write(slot + 0x4, maskWord(r));
        regs.write(slot + 0x8, 0);
        regs.write(slot + 0xC, p1Word(r));
    }
    regs.write(off_ectr, 0);
}
