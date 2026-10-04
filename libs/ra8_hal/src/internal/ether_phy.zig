//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Clause-22 Ethernet PHY over a pluggable MDIO bus. Port of
//! ra8_ether_phy.c (RA8FW-558). Every operation returns a raw ra8_err_t
//! so an io callback's own error passes straight through, as in the C.

const std = @import("std");

/// ra8_err_t values used here (libs/ra8_core/inc/ra8_err.h).
pub const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const exists: u16 = 0x10C;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_timeout: u16 = 0x203;
};

pub const reg_control: u8 = 0;
pub const reg_status: u8 = 1;
pub const reg_an_partner: u8 = 5;
pub const reg_max: u8 = 31;
pub const addr_max: u8 = 31;
pub const reset_poll_max: u8 = 32;

pub const bmcr_reset: u16 = 0x8000;
pub const bmcr_an_enable: u16 = 0x1000;
pub const bmcr_an_restart: u16 = 0x0200;
pub const bmsr_link_up: u16 = 0x0004;
pub const bmsr_an_complete: u16 = 0x0020;
pub const anar_100full: u16 = 0x0100;
pub const anar_100half: u16 = 0x0080;
pub const anar_10full: u16 = 0x0040;
pub const anar_10half: u16 = 0x0020;

/// `ra8_ether_phy_speed_t`.
pub const Speed = struct {
    pub const no_link: u8 = 0;
    pub const s10h: u8 = 1;
    pub const s10f: u8 = 2;
    pub const s100h: u8 = 3;
    pub const s100f: u8 = 4;
};

pub const ReadFn = *const fn (ctx: ?*anyopaque, phy_addr: u8, reg_addr: u8, out: *u16) callconv(.C) u16;
pub const WriteFn = *const fn (ctx: ?*anyopaque, phy_addr: u8, reg_addr: u8, data: u16) callconv(.C) u16;

/// `ra8_ether_phy_io_t`.
pub const Io = extern struct {
    read: ?ReadFn = null,
    write: ?WriteFn = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_ether_phy_cfg_t`.
pub const Cfg = extern struct {
    io: Io,
    phy_address: u8,
    mii_type: u8,
    reset_wait_us: u16,
};

/// `ra8_ether_phy_link_t`.
pub const Link = extern struct {
    link_up: u8,
    auto_neg_done: u8,
    speed: u8,
    bmsr: u16,
};

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Io) == 3 * ptr);
    std.debug.assert(@offsetOf(Cfg, "phy_address") == 3 * ptr);
    std.debug.assert(@offsetOf(Cfg, "reset_wait_us") == 3 * ptr + 2);
    std.debug.assert(@sizeOf(Link) == 6);
    std.debug.assert(@offsetOf(Link, "bmsr") == 4);
}

/// Resolved speed from the link partner ability, highest first.
pub fn speedFromPartner(lpa: u16) u8 {
    if ((lpa & anar_100full) != 0) return Speed.s100f;
    if ((lpa & anar_100half) != 0) return Speed.s100h;
    if ((lpa & anar_10full) != 0) return Speed.s10f;
    if ((lpa & anar_10half) != 0) return Speed.s10h;
    return Speed.no_link;
}

/// The C `s_state`. Callers null-check cfg and io before `open`.
pub const State = struct {
    opened: bool = false,
    phy_address: u8 = 0,
    mii_type: u8 = 0,
    reset_wait_us: u16 = 0,
    io: Io = .{},
    last_bmsr: u16 = 0,

    fn read(s: *State, reg: u8, out: *u16) u16 {
        return s.io.read.?(s.io.ctx, s.phy_address, reg, out);
    }

    fn write(s: *State, reg: u8, data: u16) u16 {
        return s.io.write.?(s.io.ctx, s.phy_address, reg, data);
    }

    fn resetAndWait(s: *State) u16 {
        const err = s.write(reg_control, bmcr_reset);
        if (err != Code.ok) return err;
        var i: u8 = 0;
        while (i < reset_poll_max) : (i += 1) {
            var bmcr: u16 = 0;
            const rerr = s.read(reg_control, &bmcr);
            if (rerr != Code.ok) return rerr;
            if ((bmcr & bmcr_reset) == 0) return Code.ok;
        }
        return Code.hw_timeout;
    }

    /// `ra8_ether_phy_open` after its null checks.
    pub fn open(s: *State, cfg: Cfg) u16 {
        if (cfg.phy_address > addr_max) return Code.invalid_arg;
        if (s.opened) return Code.exists;
        s.* = .{
            .opened = true,
            .phy_address = cfg.phy_address,
            .mii_type = cfg.mii_type,
            .reset_wait_us = cfg.reset_wait_us,
            .io = cfg.io,
        };
        const err = s.resetAndWait();
        if (err != Code.ok) s.opened = false;
        return err;
    }

    pub fn close(s: *State) u16 {
        if (!s.opened) return Code.invalid_state;
        s.opened = false;
        return Code.ok;
    }

    pub fn mdioRead(s: *State, reg: u8, out: *u16) u16 {
        if (!s.opened) return Code.not_initialized;
        if (reg > reg_max) return Code.invalid_arg;
        return s.read(reg, out);
    }

    pub fn mdioWrite(s: *State, reg: u8, data: u16) u16 {
        if (!s.opened) return Code.not_initialized;
        if (reg > reg_max) return Code.invalid_arg;
        return s.write(reg, data);
    }

    /// Re-arm auto-negotiation: enable + restart.
    pub fn autoNegotiateStart(s: *State) u16 {
        if (!s.opened) return Code.not_initialized;
        return s.write(reg_control, bmcr_an_enable | bmcr_an_restart);
    }

    /// `ra8_ether_phy_link_status_get` after its null check. A failed
    /// partner-ability read leaves the speed at no_link and still succeeds.
    pub fn linkStatus(s: *State, out: *Link) u16 {
        if (!s.opened) return Code.not_initialized;
        var bmsr: u16 = 0;
        const err = s.read(reg_status, &bmsr);
        if (err != Code.ok) return err;
        s.last_bmsr = bmsr;
        out.* = .{
            .link_up = @intFromBool((bmsr & bmsr_link_up) != 0),
            .auto_neg_done = @intFromBool((bmsr & bmsr_an_complete) != 0),
            .speed = Speed.no_link,
            .bmsr = bmsr,
        };
        if (out.link_up != 0 and out.auto_neg_done != 0) {
            var lpa: u16 = 0;
            if (s.read(reg_an_partner, &lpa) == Code.ok) out.speed = speedFromPartner(lpa);
        }
        return Code.ok;
    }
};
