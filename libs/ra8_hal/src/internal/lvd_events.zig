//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Voltage-monitor event layer (RA8FW-625), ported from ra8_lvd_events.c.
//! Registers and the ra8_lvd.c channel-map helpers are reached through an
//! `lvd` ops value so host tests can stand in for the hardware.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const not_supported: u16 = 0x107;

/// Absolute PVD register addresses (HUM 8.2.1 p 302, 8.2.10 p 309).
pub const pvdlr_off: usize = 0x4001EB34;
pub const pvdsar_off: usize = 0x400083CC;

pub const pvdsar_mask_all: u32 = 0x3;
pub const pvdlr_unlock: u8 = 0x00;
pub const pvdlr_relock: u8 = 0x01;
pub const sr_det: u8 = 0x01;
pub const cr0_dfdis: u8 = 0x02;
pub const cr0_cmpe: u8 = 0x04;
pub const cr0_ri: u8 = 0x40;
pub const cr0_rn: u8 = 0x80;

pub const map_count = 4;
pub const nmi_channel_count = 2;
pub const loco_div_max: u8 = 3;
pub const filter_factor: u32 = 2;
pub const filter_extra: u32 = 3;
pub const us_per_sec: u32 = 1_000_000;
pub const loco_hz_default: u32 = 32768;

/// Mirrors ra8_lvd_channel_map_t (ra8_lvd_internal.h).
pub const Map = extern struct {
    cmpcr: usize,
    cr0: usize,
    cr1: usize,
    sr: usize,
    fcr: usize,
    has_irq: bool,
};

/// Mirrors ra8_lvd_event_fn_t.
pub const EventFn = ?*const fn (ctx: ?*anyopaque, channel: u8) callconv(.c) void;

pub const State = struct {
    fn_shared: EventFn = null,
    ctx_shared: ?*anyopaque = null,
    chan_fn: [map_count]EventFn = .{ null, null, null, null },
    chan_ctx: [map_count]?*anyopaque = .{ null, null, null, null },
};

pub fn setSecurity(lvd: anytype, mask: u32) u16 {
    if ((mask & ~pvdsar_mask_all) != 0) return invalid_arg;
    lvd.write32(pvdsar_off, mask);
    return ok;
}

pub fn unlockN(lvd: anytype) u16 {
    lvd.write8(pvdlr_off, pvdlr_unlock);
    return ok;
}

pub fn relockN(lvd: anytype) u16 {
    lvd.write8(pvdlr_off, pvdlr_relock);
    return ok;
}

/// Channel id to map index; logs like RA8_RETURN_ON_ERROR on failure.
fn lookup(lvd: anytype, channel: u8, msg: [*:0]const u8, idx: *u8) u16 {
    const err = lvd.channelToIdx(channel, idx);
    if (err != ok) lvd.errVal(msg, err);
    return err;
}

pub fn enableElcEvent(lvd: anytype, channel: u8) u16 {
    var idx: u8 = 0;
    const err = lookup(lvd, channel, "lvd_enable_elc_event: bad channel", &idx);
    if (err != ok) return err;
    const map = lvd.map(idx);
    if (!map.has_irq) return not_supported;
    // HUM 8.7 p 315: clear DET first, then raise CMPE.
    lvd.write8(map.sr, lvd.read8(map.sr) & ~sr_det);
    lvd.cr0Rmw(&map, 0, cr0_cmpe);
    return ok;
}

pub fn disableElcEvent(lvd: anytype, channel: u8) u16 {
    var idx: u8 = 0;
    const err = lookup(lvd, channel, "lvd_disable_elc_event: bad channel", &idx);
    if (err != ok) return err;
    const map = lvd.map(idx);
    if (!map.has_irq) return not_supported;
    lvd.cr0Rmw(&map, cr0_cmpe, 0);
    return ok;
}

pub fn configureForStandby(lvd: anytype, channel: u8) u16 {
    var idx: u8 = 0;
    const err = lookup(lvd, channel, "lvd_configure_for_standby: bad channel", &idx);
    if (err != ok) return err;
    const map = lvd.map(idx);
    // HUM 8.5(1)/(2) p 311-312: filter off; m channels also clear RI + RN.
    const clr: u8 = if (map.has_irq) cr0_ri | cr0_rn else 0;
    lvd.cr0Rmw(&map, clr, cr0_dfdis);
    return ok;
}

pub fn cancelDeepStandbyPath(lvd: anytype) u16 {
    var i: u8 = 0;
    while (i < nmi_channel_count) : (i += 1) {
        const map = lvd.map(i);
        lvd.cr0Rmw(&map, cr0_ri, 0);
    }
    return ok;
}

/// HUM Table 8.4/8.6 step 8: "2s + 3" LOCO cycles, s = 2^(div+1), +1 us.
pub fn filterDelayUs(div: u8, loco_hz: u32) u32 {
    const safe: u5 = @intCast(@min(div, loco_div_max));
    const local_factor = @as(u32, 1) << (safe + 1);
    const cycles = filter_factor * local_factor + filter_extra;
    const hz = if (loco_hz != 0) loco_hz else loco_hz_default;
    return (cycles *% us_per_sec) / hz + 1;
}

pub fn attachHandler(state: *State, f: EventFn, ctx: ?*anyopaque) u16 {
    state.fn_shared = f;
    state.ctx_shared = ctx;
    return ok;
}

pub fn attachChannelHandler(state: *State, lvd: anytype, channel: u8, f: EventFn, ctx: ?*anyopaque) u16 {
    var idx: u8 = 0;
    const err = lookup(lvd, channel, "lvd_attach_channel_handler: bad channel", &idx);
    if (err != ok) return err;
    if (!lvd.map(idx).has_irq) return not_supported;
    state.chan_fn[idx] = f;
    state.chan_ctx[idx] = ctx;
    return ok;
}

/// Fires the per-channel handler (else the shared one) when DET latched,
/// then clears DET so the next crossing can latch (HUM 8.2.7 p 307).
pub fn dispatch(state: *const State, lvd: anytype, channel: u8) void {
    var idx: u8 = 0;
    if (lvd.channelToIdx(channel, &idx) != ok) return;
    const map = lvd.map(idx);
    if (!map.has_irq) return;
    if ((lvd.read8(map.sr) & sr_det) == 0) return;
    const chan_fn = state.chan_fn[idx];
    const f = if (chan_fn != null) chan_fn else state.fn_shared;
    const ctx = if (chan_fn != null) state.chan_ctx[idx] else state.ctx_shared;
    if (f) |cb| cb(ctx, channel);
    lvd.write8(map.sr, lvd.read8(map.sr) & ~sr_det);
}
