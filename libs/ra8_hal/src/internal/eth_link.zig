//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! PHY link status and MAC speed resync for ra8_eth (RA8FW-617). The PHY
//! and MAC are reached through a `phy` ops value so host tests can stand
//! in for MDIO, ETHA and RMAC.

pub const ok: u16 = 0;
pub const not_initialized: u16 = 0x10F;
pub const null_ptr: u16 = 0x504;

pub const phy_addr: u8 = 0;
pub const reg_bmcr: u8 = 0;
pub const reg_bmsr: u8 = 1;
pub const reg_anlpar: u8 = 5;
pub const reg_gbsr: u8 = 10;
pub const bmsr_link_up: u16 = 0x0004;
pub const bmsr_an_done: u16 = 0x0020;
pub const bmcr_speed100: u16 = 0x2000;
pub const bmcr_duplex: u16 = 0x0100;

pub const lsc_10: u8 = 0;
pub const lsc_100: u8 = 1;
pub const lsc_1000: u8 = 2;
pub const half: u8 = 0;
pub const full: u8 = 1;
pub const pis_mii: u8 = 0;
pub const pis_gmii: u8 = 2;
pub const opc_disable: u8 = 1;
pub const opc_config: u8 = 2;
pub const opc_operation: u8 = 3;

pub const an_poll_period_ms: u32 = 50;
pub const an_poll_max_iters: u32 = 80;

/// ra8_eth_link_t: u8, u16, u8, u16 with natural C padding.
pub const Link = extern struct {
    link_up: u8,
    speed_mbps: u16,
    full_duplex: u8,
    bmsr: u16,
};

pub const Neg = struct { speed: u8, duplex: u8 };

/// Later matches win, so the highest advertised mode is kept.
pub fn pickSpeed(anlpar: u16, gbsr: u16) Neg {
    var n = Neg{ .speed = lsc_10, .duplex = half };
    if (anlpar & 0x0020 != 0) n = .{ .speed = lsc_10, .duplex = half };
    if (anlpar & 0x0040 != 0) n = .{ .speed = lsc_10, .duplex = full };
    if (anlpar & 0x0080 != 0) n = .{ .speed = lsc_100, .duplex = half };
    if (anlpar & 0x0100 != 0) n = .{ .speed = lsc_100, .duplex = full };
    if (gbsr & 0x0400 != 0) n = .{ .speed = lsc_1000, .duplex = half };
    if (gbsr & 0x0800 != 0) n = .{ .speed = lsc_1000, .duplex = full };
    return n;
}

pub fn channelToPort(channel: u8) u8 {
    return if (channel == 0) 0 else 1;
}

fn waitForAutoneg(phy: anytype, port: u8) void {
    var bmsr: u16 = 0;
    var i: u32 = 0;
    while (i < an_poll_max_iters) : (i += 1) {
        if (phy.mdioRead(port, reg_bmsr, &bmsr) != ok) break;
        if (bmsr & bmsr_an_done != 0) break;
        phy.delayMs(an_poll_period_ms);
    }
    phy.traceBmsr(bmsr);
}

fn queryNegotiated(phy: anytype, port: u8, out: *Neg) u16 {
    var anlpar: u16 = 0;
    const a_err = phy.mdioRead(port, reg_anlpar, &anlpar);
    if (a_err != ok) return a_err;
    var gbsr: u16 = 0;
    const g_err = phy.mdioRead(port, reg_gbsr, &gbsr);
    if (g_err != ok) return g_err;
    phy.traceAdvert(anlpar, gbsr);
    out.* = pickSpeed(anlpar, gbsr);
    phy.traceResync(out.speed, out.duplex);
    return ok;
}

/// MPIC.PIS tracks link speed (GMII only at 1000). set_link's error wins
/// over the two trailing mode switches, which always run.
fn programMpic(phy: anytype, port: u8, n: Neg) u16 {
    var e = phy.ethaSetMode(port, opc_disable);
    if (e != ok) return e;
    e = phy.ethaSetMode(port, opc_config);
    if (e != ok) return e;
    const pis = if (n.speed == lsc_1000) pis_gmii else pis_mii;
    const set_err = phy.setLink(port, pis, n.speed, n.duplex);
    const dis_err = phy.ethaSetMode(port, opc_disable);
    const op_err = phy.ethaSetMode(port, opc_operation);
    if (set_err != ok) return set_err;
    if (dis_err != ok) return dis_err;
    return op_err;
}

fn resync(phy: anytype, port: u8, resynced: *bool) u16 {
    waitForAutoneg(phy, port);
    var n = Neg{ .speed = lsc_10, .duplex = half };
    const q_err = queryNegotiated(phy, port, &n);
    if (q_err != ok) return q_err;
    const m_err = programMpic(phy, port, n);
    if (m_err != ok) return m_err;
    resynced.* = true;
    phy.info("link_status: MPIC resynced to PHY speed/duplex");
    return ok;
}

fn checked(phy: anytype, e: u16, msg: [*:0]const u8) u16 {
    if (e != ok) {
        phy.err(msg);
        phy.errVal("Error", e);
    }
    return e;
}

fn readLink(phy: anytype, port: u8, out: *Link, bmcr_out: *u16) u16 {
    var bmsr: u16 = 0;
    var e = checked(phy, phy.mdioRead(port, reg_bmsr, &bmsr), "link_status: bmsr read");
    if (e != ok) return e;
    var bmcr: u16 = 0;
    e = checked(phy, phy.mdioRead(port, reg_bmcr, &bmcr), "link_status: bmcr read");
    if (e != ok) return e;
    out.bmsr = bmsr;
    out.link_up = @intFromBool(bmsr & bmsr_link_up != 0);
    out.full_duplex = @intFromBool(bmcr & bmcr_duplex != 0);
    out.speed_mbps = if (bmcr & bmcr_speed100 != 0) 100 else 10;
    bmcr_out.* = bmcr;
    return ok;
}

pub fn linkStatus(phy: anytype, out: ?*Link, opened: bool, channel: u8, resynced: *bool) u16 {
    const o = out orelse {
        phy.err("link_status: out must not be nullptr");
        return null_ptr;
    };
    if (!opened) return not_initialized;
    const port = channelToPort(channel);
    var bmcr: u16 = 0;
    const e = checked(phy, readLink(phy, port, o, &bmcr), "link_status: phy read");
    if (e != ok) return e;
    if (o.link_up == 0 or resynced.*) return ok;
    return resync(phy, port, resynced);
}
