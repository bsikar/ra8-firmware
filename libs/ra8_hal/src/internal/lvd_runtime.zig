//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Voltage-monitor runtime controls (RA8FW-739), ported from
//! ra8_lvd_runtime.c. Registers and the lvd_abi.zig channel-map helpers are
//! reached through an `lvd` ops value so host tests can stand in for the
//! hardware.

const ev = @import("lvd_events.zig");

pub const Map = ev.Map;
pub const ok = ev.ok;
pub const invalid_arg = ev.invalid_arg;
pub const not_supported = ev.not_supported;
pub const invalid_state: u16 = 0x104;
pub const null_ptr: u16 = 0x504;

pub const cr0_rie: u8 = 0x01;
pub const cr0_re: u8 = 0x01;
pub const cr0_dfdis: u8 = 0x02;
pub const cr0_cmpe: u8 = 0x04;
pub const cr0_fsamp: u8 = 0x30;
pub const cr0_fsamp_shift: u3 = 4;
pub const cr0_ri: u8 = 0x40;
pub const cr0_rn: u8 = 0x80;
pub const cmpcr_pvde: u8 = 0x80;
pub const sr_det: u8 = 0x01;
pub const sr_mon: u8 = 0x02;
pub const fcr_rhsel: u8 = 0x01;
pub const hysteresis_hvd: u8 = 1;
pub const negate_after_assert: u8 = 1;

/// Mirrors ra8_lvd_status_t.
pub const Status = extern struct {
    crossed: bool,
    above: bool,
};

/// Channel id to map, logging like RA8_RETURN_ON_ERROR on failure.
fn lookup(lvd: anytype, channel: u8, msg: [*:0]const u8, out: *Map) u16 {
    var idx: u8 = 0;
    const err = lvd.channelToIdx(channel, &idx);
    if (err != ok) {
        lvd.errVal(msg, err);
        return err;
    }
    out.* = lvd.map(idx);
    return ok;
}

pub fn enableIrq(lvd: anytype, channel: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_enable_irq: bad channel", &map);
    if (err != ok) return err;
    if (!map.has_irq) return not_supported;
    // HUM 8.2.4 "PVDmCR0" p 305: set RIE.
    lvd.cr0Rmw(&map, 0, cr0_rie);
    return ok;
}

pub fn disableIrq(lvd: anytype, channel: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_disable_irq: bad channel", &map);
    if (err != ok) return err;
    if (!map.has_irq) return not_supported;
    lvd.cr0Rmw(&map, cr0_rie, 0);
    return ok;
}

/// m channels set RI then RIE; n channels only have RE (HUM 8.2.4/8.2.5).
pub fn enableReset(lvd: anytype, channel: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_enable_reset: bad channel", &map);
    if (err != ok) return err;
    const set: u8 = if (map.has_irq) cr0_ri | cr0_rie else cr0_re;
    lvd.cr0Rmw(&map, 0, set);
    return ok;
}

pub fn disableReset(lvd: anytype, channel: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_disable_reset: bad channel", &map);
    if (err != ok) return err;
    const clr: u8 = if (map.has_irq) cr0_rie else cr0_re;
    lvd.cr0Rmw(&map, clr, 0);
    return ok;
}

pub fn enableCmpe(lvd: anytype, channel: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_enable_cmpe: bad channel", &map);
    if (err != ok) return err;
    lvd.cr0Rmw(&map, 0, cr0_cmpe);
    return ok;
}

pub fn disableCmpe(lvd: anytype, channel: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_disable_cmpe: bad channel", &map);
    if (err != ok) return err;
    lvd.cr0Rmw(&map, cr0_cmpe, 0);
    return ok;
}

/// Disable the filter, program FSAMP, then drop DFDIS only after FSAMP has
/// landed (HUM 8.2.4 p 305 / 8.2.5 p 306).
pub fn setFilter(lvd: anytype, channel: u8, div: u8, enable: bool) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_set_filter: bad channel", &map);
    if (err != ok) return err;
    const div_err = lvd.validateDiv(div);
    if (div_err != ok) {
        lvd.errVal("lvd_set_filter: bad div", div_err);
        return div_err;
    }
    lvd.cr0Rmw(&map, 0, cr0_dfdis);
    const fsamp = (div << cr0_fsamp_shift) & cr0_fsamp;
    lvd.cr0Rmw(&map, cr0_fsamp, fsamp);
    if (enable) lvd.cr0Rmw(&map, cr0_dfdis, 0);
    return ok;
}

/// RHSEL may only change with PVDE clear; HVD on an m channel needs RI set
/// first (HUM 8.2.8 p 308). PVDE is restored to its previous value.
pub fn setHysteresisMode(lvd: anytype, channel: u8, hyst: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_set_hysteresis_mode: bad channel", &map);
    if (err != ok) return err;
    if (hyst > hysteresis_hvd) return invalid_arg;
    if (map.has_irq and hyst == hysteresis_hvd and lvd.readRi(&map) == 0) return invalid_state;
    const prev = lvd.read8(map.cmpcr);
    const pvde_was = prev & cmpcr_pvde;
    lvd.write8(map.cmpcr, prev & ~cmpcr_pvde);
    lvd.write8(map.fcr, hyst & fcr_rhsel);
    lvd.write8(map.cmpcr, lvd.read8(map.cmpcr) | pvde_was);
    return ok;
}

/// RN after-assert is illegal while RHSEL is set (HUM 8.2.4 p 306, 8.2.8).
pub fn setNegateMode(lvd: anytype, channel: u8, negate: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_set_negate_mode: bad channel", &map);
    if (err != ok) return err;
    if (!map.has_irq) return not_supported;
    if (negate > negate_after_assert) return invalid_arg;
    const after = negate == negate_after_assert;
    if (after and (lvd.read8(map.fcr) & fcr_rhsel) != 0) return invalid_state;
    lvd.cr0Rmw(&map, cr0_rn, if (after) cr0_rn else 0);
    return ok;
}

/// HUM 8.2.7 "PVDmSR" p 307: DET is the latch, MON the live comparator.
pub fn getStatus(lvd: anytype, channel: u8, out: ?*Status) u16 {
    const dst = out orelse {
        lvd.err("out must not be nullptr");
        return null_ptr;
    };
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_get_status: bad channel", &map);
    if (err != ok) return err;
    if (!map.has_irq) return not_supported;
    const sr = lvd.read8(map.sr);
    dst.* = .{ .crossed = (sr & sr_det) != 0, .above = (sr & sr_mon) != 0 };
    return ok;
}

pub fn clearStatus(lvd: anytype, channel: u8) u16 {
    var map: Map = undefined;
    const err = lookup(lvd, channel, "lvd_clear_status: bad channel", &map);
    if (err != ok) return err;
    if (!map.has_irq) return not_supported;
    lvd.write8(map.sr, lvd.read8(map.sr) & ~sr_det);
    return ok;
}
