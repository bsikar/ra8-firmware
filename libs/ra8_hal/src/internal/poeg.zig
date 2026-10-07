//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! POEG port output enable for GPT (RA8FW-598). Pure: `regs` reaches a
//! group's POEGG register, `c` reaches module stop and logging, and the
//! per-group handler table is passed in as `slots`. HUM Ch 21.2.1.

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_null_ptr: u16 = 0x504;

pub const base0: usize = 0x40212000;
pub const stride: usize = 0x100;
pub const group_count: u8 = 4;

pub const status_pidf: u32 = 0x0000_0001;
pub const status_iocf: u32 = 0x0000_0002;
pub const status_ovrf: u32 = 0x0000_0004;
pub const status_ssf: u32 = 0x0000_0008;
pub const status_st: u32 = 0x0001_0000;
pub const status_all: u32 = status_pidf | status_iocf | status_ovrf | status_ssf | status_st;

pub const en_pide: u32 = 0x0000_0100;
pub const en_iocen: u32 = 0x0000_0200;
pub const en_osten: u32 = 0x0000_0400;
pub const en_inv: u32 = 0x0000_1000;

/// `ra8_poeg_cfg_t`.
pub const Cfg = extern struct {
    enable_pin: bool,
    enable_ioc: bool,
    enable_osc_stop: bool,
    invert_input: bool,
};

/// `ra8_poeg_event_fn_t`.
pub const Handler = ?*const fn (?*anyopaque, u32) callconv(.c) void;

pub const Slot = struct {
    handler: Handler = null,
    ctx: ?*anyopaque = null,
};

/// MSTPD14 group A down to MSTPD11 group D.
pub fn mstpId(group: u8) u16 {
    return (3 << 8) | @as(u16, 14 - group);
}

pub fn cfgToPoegg(cfg: Cfg) u32 {
    var v: u32 = 0;
    if (cfg.enable_pin) v |= en_pide;
    if (cfg.enable_ioc) v |= en_iocen;
    if (cfg.enable_osc_stop) v |= en_osten;
    if (cfg.invert_input) v |= en_inv;
    return v;
}

fn inRange(c: anytype, group: u8) bool {
    if (group < group_count) return true;
    c.err("group out of range");
    return false;
}

pub fn init(regs: anytype, c: anytype, group: u8, cfg: ?*const Cfg) u16 {
    const p = cfg orelse {
        c.err("cfg must not be nullptr");
        return err_null_ptr;
    };
    if (!inRange(c, group)) return err_null_ptr;
    const err = c.mstpEnable(mstpId(group));
    if (err != ok) {
        c.fail("poeg_init: mstp enable", err);
        return err;
    }
    regs.write(group, cfgToPoegg(p.*));
    c.infoVal("init group", group);
    return ok;
}

pub fn deinit(regs: anytype, c: anytype, slots: []Slot, group: u8) u16 {
    if (!inRange(c, group)) return err_null_ptr;
    regs.write(group, 0);
    slots[group] = .{};
    _ = c.mstpDisable(mstpId(group));
    return ok;
}

pub fn triggerStop(regs: anytype, c: anytype, group: u8) u16 {
    if (!inRange(c, group)) return err_null_ptr;
    regs.write(group, regs.read(group) | status_ssf);
    return ok;
}

pub fn getStatus(regs: anytype, c: anytype, group: u8, out: ?*u32) u16 {
    const p = out orelse {
        c.err("out_mask must not be nullptr");
        return err_null_ptr;
    };
    if (!inRange(c, group)) return err_null_ptr;
    p.* = regs.read(group) & status_all;
    return ok;
}

pub fn clearStatus(regs: anytype, c: anytype, group: u8, mask: u32) u16 {
    if (!inRange(c, group)) return err_null_ptr;
    regs.write(group, regs.read(group) & ~(mask & status_all));
    return ok;
}

pub fn attachHandler(slots: []Slot, group: u8, handler: Handler, ctx: ?*anyopaque) u16 {
    if (group >= group_count) return err_invalid_arg;
    slots[group] = .{ .handler = handler, .ctx = ctx };
    return ok;
}

pub fn enterStop(c: anytype, group: u8) u16 {
    if (group >= group_count) return err_invalid_arg;
    return c.mstpDisable(mstpId(group));
}

pub fn exitStop(c: anytype, group: u8) u16 {
    if (group >= group_count) return err_invalid_arg;
    return c.mstpEnable(mstpId(group));
}

/// ISR-safe: snapshot the latched status, then call the group's handler.
pub fn dispatch(regs: anytype, slots: []const Slot, group: u8) void {
    if (group >= group_count) return;
    const mask = regs.read(group) & status_all;
    if (slots[group].handler) |f| f(slots[group].ctx, mask);
}
