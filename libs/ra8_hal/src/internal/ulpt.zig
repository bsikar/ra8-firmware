//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ULPT0/1 low-power timer (RA8FW-597). Pure: `regs`
//! reaches a channel's registers by offset at their real widths, `c`
//! reaches module stop and logging. Offsets follow r_ulpt_regs_t.

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_hw_timeout: u16 = 0x203;
pub const err_null_ptr: u16 = 0x504;

pub const base0: usize = 0x40220000;
pub const stride: usize = 0x100;
pub const channel_count: u8 = 2;
pub const stop_poll_max: u32 = 0x40000;

pub const off_cnt: usize = 0x00; // u32
pub const off_cr: usize = 0x0C; // u8 from here on
pub const off_mr1: usize = 0x0D;
pub const off_mr2: usize = 0x0E;
pub const off_mr3: usize = 0x0F;
pub const off_ioc: usize = 0x10;

pub const cr_tstart: u8 = 0x01;
pub const cr_tcstf: u8 = 0x02;
pub const cr_tstop: u8 = 0x04;

/// `ra8_ulpt_event_fn_t`.
pub const Handler = ?*const fn (?*anyopaque, u8) callconv(.C) void;

/// MSTPE9 ULPT0, MSTPE8 ULPT1.
pub fn mstpId(channel: u8) u16 {
    return (4 << 8) | @as(u16, 9 - channel);
}

fn clearModes(regs: anytype, ch: u8) void {
    regs.write8(ch, off_mr1, 0);
    regs.write8(ch, off_mr2, 0);
    regs.write8(ch, off_mr3, 0);
}

pub fn init(regs: anytype, c: anytype) u16 {
    for (0..channel_count) |i| {
        const ch: u8 = @intCast(i);
        const err = c.mstpEnable(mstpId(ch));
        if (err != ok) {
            c.fail("ulpt_init: mstp enable", err);
            return err;
        }
        regs.write8(ch, off_cr, 0);
        clearModes(regs, ch);
        regs.write8(ch, off_ioc, 0);
        regs.write32(ch, off_cnt, 0);
    }
    c.info("ulpt_init");
    return ok;
}

pub fn start(regs: anytype, c: anytype, channel: u8, period: u32) u16 {
    if (channel >= channel_count) return err_invalid_arg;
    regs.write8(channel, off_cr, 0);
    clearModes(regs, channel); // timer mode, TCK1 = 0 picks ULPTLCLK
    regs.write32(channel, off_cnt, period);
    regs.write8(channel, off_cr, cr_tstart);
    c.infoVal("start channel", channel);
    return ok;
}

/// Request a stop, then wait for TCSTF low (ra8_hw_wait_flag_clear8).
pub fn stop(regs: anytype, channel: u8) u16 {
    if (channel >= channel_count) return err_invalid_arg;
    regs.write8(channel, off_cr, cr_tstop);
    regs.write8(channel, off_cr, 0);
    for (0..stop_poll_max) |_| {
        if (regs.read8(channel, off_cr) & cr_tcstf == 0) return ok;
    }
    return err_hw_timeout;
}

pub fn deinit(regs: anytype, c: anytype, channel: u8) u16 {
    if (channel >= channel_count) return err_invalid_arg;
    regs.write8(channel, off_cr, 0);
    return c.mstpDisable(mstpId(channel));
}

pub fn setPeriod(regs: anytype, channel: u8, period: u32) u16 {
    if (channel >= channel_count) return err_invalid_arg;
    regs.write32(channel, off_cnt, period);
    return ok;
}

pub fn getStatus(regs: anytype, c: anytype, channel: u8, out: ?*u8) u16 {
    const p = out orelse {
        c.err("out_mask must not be nullptr");
        return err_null_ptr;
    };
    if (channel >= channel_count) return err_invalid_arg;
    p.* = regs.read8(channel, off_cr);
    return ok;
}

pub fn dispatch(handler: Handler, ctx: ?*anyopaque, channel: u8) void {
    if (channel >= channel_count) return;
    if (handler) |f| f(ctx, channel);
}

pub fn enterStop(c: anytype, channel: u8) u16 {
    if (channel >= channel_count) return err_invalid_arg;
    return c.mstpDisable(mstpId(channel));
}

pub fn exitStop(c: anytype, channel: u8) u16 {
    if (channel >= channel_count) return err_invalid_arg;
    return c.mstpEnable(mstpId(channel));
}
