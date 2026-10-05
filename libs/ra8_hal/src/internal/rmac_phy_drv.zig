//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Callback-driven Clause 22 PHY driver logic (RA8FW-765), moved out of
//! ra8_rmac_phy.c. `io` is any value with read(reg, *u16) u16 and
//! write(reg, u16) u16 that already carries the PHY address.
//! IEEE 802.3 Clause 22: BMCR 0, BMSR 1, ANAR 4, ANLPAR 5, 1000T ctrl 9,
//! 1000T status 10.

pub const ok: u16 = 0;
pub const err_hw_timeout: u16 = 0x203;

pub const reg_control: u8 = 0;
pub const reg_status: u8 = 1;
pub const reg_an_advert: u8 = 4;
pub const reg_an_partner: u8 = 5;
pub const reg_1000t_ctrl: u8 = 9;
pub const reg_1000t_status: u8 = 10;
pub const reset_poll_default: u16 = 32;
pub const addr_max: u8 = 31;
pub const reg_max: u8 = 31;
pub const lsi_count: u8 = 8;

pub const bmcr_reset: u16 = 0x8000;
pub const bmcr_an_enable: u16 = 0x1000;
pub const bmcr_an_restart: u16 = 0x0200;
pub const bmsr_link_up: u16 = 0x0004;
pub const bmsr_an_complete: u16 = 0x0020;
pub const lpa_100full: u16 = 0x0100;
pub const lpa_100half: u16 = 0x0080;
pub const lpa_10full: u16 = 0x0040;
pub const lpa_10half: u16 = 0x0020;
pub const msr_1000full: u16 = 0x0800;
pub const msr_1000half: u16 = 0x0400;

pub const speed_no_link: u8 = 0;
pub const speed_10h: u8 = 1;
pub const speed_10f: u8 = 2;
pub const speed_100h: u8 = 3;
pub const speed_100f: u8 = 4;
pub const speed_1000h: u8 = 5;
pub const speed_1000f: u8 = 6;

/// Mirror of ra8_rmac_phy_link_t.
pub const Link = extern struct {
    link_up: u8 = 0,
    auto_neg_done: u8 = 0,
    speed: u8 = speed_no_link,
    bmsr: u16 = 0,
    partner_ability: u16 = 0,
};

comptime {
    if (@sizeOf(Link) != 8) @compileError("ra8_rmac_phy_link_t is 8 bytes");
    if (@offsetOf(Link, "speed") != 2) @compileError("speed at +2");
    if (@offsetOf(Link, "bmsr") != 4) @compileError("bmsr at +4");
    if (@offsetOf(Link, "partner_ability") != 6) @compileError("partner_ability at +6");
}

pub fn speedOk(err: u16, value: u16, mask: u16) bool {
    return err == ok and (value & mask) != 0;
}

/// Write BMCR.RESET, then poll BMCR until the bit self-clears.
pub fn resetAndWait(io: anytype, poll_max: u16) u16 {
    var err = io.write(reg_control, bmcr_reset);
    if (err != ok) return err;
    var i: u16 = 0;
    while (i < poll_max) : (i += 1) {
        var reg: u16 = 0;
        err = io.read(reg_control, &reg);
        if (err != ok) return err;
        if (reg & bmcr_reset == 0) return ok;
    }
    return err_hw_timeout;
}

/// ANAR always; 1000BASE-T control only when gigabit is advertised.
pub fn programAdvertise(io: anytype, local: u16, gbit: u16) u16 {
    const err = io.write(reg_an_advert, local);
    if (err != ok or gbit == 0) return err;
    return io.write(reg_1000t_ctrl, gbit);
}

pub fn speedFromLpa(lpa: u16) ?u8 {
    if (lpa & lpa_100full != 0) return speed_100f;
    if (lpa & lpa_100half != 0) return speed_100h;
    if (lpa & lpa_10full != 0) return speed_10f;
    if (lpa & lpa_10half != 0) return speed_10h;
    return null;
}

/// 1000T status first (when gigabit is advertised), then the link partner.
pub fn resolve(io: anytype, gbit: u16, out: *Link) void {
    if (gbit != 0) {
        var msr: u16 = 0;
        const err = io.read(reg_1000t_status, &msr);
        if (speedOk(err, msr, msr_1000full)) {
            out.speed = speed_1000f;
            return;
        }
        if (speedOk(err, msr, msr_1000half)) {
            out.speed = speed_1000h;
            return;
        }
    }
    var lpa: u16 = 0;
    if (io.read(reg_an_partner, &lpa) != ok) return;
    out.partner_ability = lpa;
    if (speedFromLpa(lpa)) |s| out.speed = s;
}

/// Fill `out` from a BMSR value; speed resolution runs only on link + AN.
pub fn fromBmsr(bmsr: u16, out: *Link) bool {
    out.bmsr = bmsr;
    out.link_up = @intFromBool(bmsr & bmsr_link_up != 0);
    out.auto_neg_done = @intFromBool(bmsr & bmsr_an_complete != 0);
    out.speed = speed_no_link;
    out.partner_ability = 0;
    return out.link_up != 0 and out.auto_neg_done != 0;
}
