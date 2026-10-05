//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RMAC Clause 22 PHY helpers (RA8FW-744), ported from ra8_rmac_mgmt.c.
//! MDIO reads/writes and logging come in through an `ops` value so the host
//! tests can drive a fake PHY. IEEE 802.3 Clause 22 sec 22.2.4 (BMCR, BMSR)
//! and Clause 28.2.4 (ANAR, ANLPAR); HUM Ch 33.4.1.1 MPSM carries the MDIO.

pub const codes = struct {
    pub const ok: u16 = 0x000;
    pub const invalid_arg: u16 = 0x103;
    pub const hw_timeout: u16 = 0x203;
    pub const null_ptr: u16 = 0x504;
};
const err = codes;

pub const port_count: u8 = 2;
pub const phy_addr_max: u8 = 31;
pub const reset_iter_cap: u32 = 4096;
pub const anwait_iter_cap: u32 = 65536;
pub const anwait_iters_per_ms: u32 = 100;

pub const reg_bmcr: u8 = 0;
pub const reg_bmsr: u8 = 1;
pub const reg_anar: u8 = 4;
pub const reg_anlpar: u8 = 5;

pub const bmcr_an_restart: u16 = 0x0200;
pub const bmcr_an_enable: u16 = 0x1000;
pub const bmcr_reset: u16 = 0x8000;
pub const bmsr_link_up: u16 = 0x0004;
pub const bmsr_an_done: u16 = 0x0020;
pub const anar_selector: u16 = 0x0001;
pub const anlpar_10_hd: u16 = 0x0020;
pub const anlpar_10_fd: u16 = 0x0040;
pub const anlpar_100_hd: u16 = 0x0080;
pub const anlpar_100_fd: u16 = 0x0100;

pub const speed_unknown: u8 = 0;
pub const speed_10_hd: u8 = 1;
pub const speed_10_fd: u8 = 2;
pub const speed_100_hd: u8 = 3;
pub const speed_100_fd: u8 = 4;

/// Mirrors ra8_rmac_phy_link_t: bool up, then the u8 speed enum.
pub const Link = extern struct { up: bool, speed: u8 };

pub fn argsOk(port: u8, phy_addr: u8) bool {
    return port < port_count and phy_addr <= phy_addr_max;
}

/// Best resolved mode wins: 100FD, 100HD, 10FD, 10HD.
pub fn decodeAnlpar(anlpar: u16) u8 {
    if (anlpar & anlpar_100_fd != 0) return speed_100_fd;
    if (anlpar & anlpar_100_hd != 0) return speed_100_hd;
    if (anlpar & anlpar_10_fd != 0) return speed_10_fd;
    if (anlpar & anlpar_10_hd != 0) return speed_10_hd;
    return speed_unknown;
}

/// timeout_ms * 100 reads (10 us each); 0 means the 65536 cap, and a
/// product that wraps to 0 still gets one read.
pub fn waitBudget(timeout_ms: u32) u32 {
    if (timeout_ms == 0) return anwait_iter_cap;
    const cap = timeout_ms *% anwait_iters_per_ms;
    return if (cap == 0) 1 else cap;
}

pub fn reset(ops: anytype, port: u8, phy: u8) u16 {
    if (!argsOk(port, phy)) {
        ops.logError("phy_reset: bad args");
        return err.invalid_arg;
    }
    const w = ops.write(port, phy, reg_bmcr, bmcr_reset);
    if (w != err.ok) {
        ops.logError("phy_reset: bmcr write");
        return w;
    }
    var i: u32 = 0;
    while (i < reset_iter_cap) : (i += 1) {
        var bmcr: u16 = 0;
        const r = ops.read(port, phy, reg_bmcr, &bmcr);
        if (r != err.ok) {
            ops.logError("phy_reset: bmcr read");
            return r;
        }
        if (bmcr & bmcr_reset == 0) return err.ok;
    }
    ops.logError("phy_reset: bmcr.reset never cleared");
    return err.hw_timeout;
}

pub fn setAdvertise(ops: anytype, port: u8, phy: u8, caps: u16) u16 {
    if (!argsOk(port, phy)) {
        ops.logError("phy_set_advertise: bad args");
        return err.invalid_arg;
    }
    return ops.write(port, phy, reg_anar, caps | anar_selector);
}

pub fn autoNegStart(ops: anytype, port: u8, phy: u8) u16 {
    if (!argsOk(port, phy)) {
        ops.logError("phy_auto_neg_start: bad args");
        return err.invalid_arg;
    }
    return ops.write(port, phy, reg_bmcr, bmcr_an_enable | bmcr_an_restart);
}

fn readSpeed(ops: anytype, port: u8, phy: u8, link: *Link) u16 {
    var anlpar: u16 = 0;
    const r = ops.read(port, phy, reg_anlpar, &anlpar);
    if (r == err.ok) link.speed = decodeAnlpar(anlpar);
    return r;
}

pub fn autoNegWait(ops: anytype, port: u8, phy: u8, timeout_ms: u32, out: ?*Link) u16 {
    const link = out orelse {
        ops.logError("phy_auto_neg_wait: out_link null");
        return err.null_ptr;
    };
    if (!argsOk(port, phy)) {
        ops.logError("phy_auto_neg_wait: bad args");
        return err.invalid_arg;
    }
    link.* = .{ .up = false, .speed = speed_unknown };
    const need = bmsr_an_done | bmsr_link_up;
    var i: u32 = 0;
    const cap = waitBudget(timeout_ms);
    while (i < cap) : (i += 1) {
        var bmsr: u16 = 0;
        const r = ops.read(port, phy, reg_bmsr, &bmsr);
        if (r != err.ok) return r;
        if (bmsr & need == need) {
            var resolved: Link = .{ .up = true, .speed = speed_unknown };
            const lp = readSpeed(ops, port, phy, &resolved);
            if (lp != err.ok) return lp;
            link.* = resolved;
            return err.ok;
        }
    }
    ops.logError("phy_auto_neg_wait: timeout");
    return err.hw_timeout;
}

pub fn linkStatus(ops: anytype, port: u8, phy: u8, out: ?*Link) u16 {
    const link = out orelse {
        ops.logError("phy_link_status: out_link null");
        return err.null_ptr;
    };
    if (!argsOk(port, phy)) {
        ops.logError("phy_link_status: bad args");
        return err.invalid_arg;
    }
    link.* = .{ .up = false, .speed = speed_unknown };
    var bmsr: u16 = 0;
    const r = ops.read(port, phy, reg_bmsr, &bmsr);
    if (r != err.ok) return r;
    link.up = bmsr & bmsr_link_up != 0;
    if (link.up and bmsr & bmsr_an_done != 0) return readSpeed(ops, port, phy, link);
    return err.ok;
}
