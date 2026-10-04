//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ESWM clock bring-up: ESWCLK = PLL1P/4 (250 MHz), ESWPHYCLK = PLL1P/2
//! (RA8FW-584, was ra8_cgc_eswclk.c). Pure: R_SYSTEM comes in through a
//! `regs` value (read8/write8/write16 by offset) and the HOCO helper, MSTP
//! and logging through an `ops` value. Sequence from FSP bsp_clocks.c
//! bsp_peripheral_clock_set; offsets from inc/ra8_system_regs.h.

const prcr = @import("prcr.zig");

/// R_SYSTEM base and the byte registers this file drives.
pub const system_base: usize = 0x4001_E000;
pub const off_eswckdivcr: usize = 0x0D5;
pub const off_eswpckdivcr: usize = 0x0D6;
pub const off_eswckcr: usize = 0x0DB;
pub const off_eswpckcr: usize = 0x0DC;
pub const off_pdctreswm: usize = 0x118;
pub const off_prcr: usize = prcr.addr - system_base;

/// ESWCKCR / ESWPCKCR: CKSEL[3:0], CKSREQ (bit 6), CKSRDY (bit 7).
pub const sel_mask: u8 = 0x0F;
pub const sel_pll1p: u8 = 5;
pub const sreq: u8 = 1 << 6;
pub const srdy: u8 = 1 << 7;
/// ESWCKDIVCR codes: /2 and /4.
pub const div2: u8 = 1;
pub const div4: u8 = 2;
/// PDCTRESWM: PDDE (bit 0), PDCSF (bit 6), PDPGSF (bit 7).
pub const pdde: u8 = 1 << 0;
pub const pdcsf: u8 = 1 << 6;
pub const pdpgsf: u8 = 1 << 7;

/// MSTPC28 ETHPHYCLK: (k_ra8_mstp_reg_c << 8) | 28.
pub const mstp_ethphyclk: u16 = (2 << 8) | 28;
pub const poll_limit: u32 = 200_000;
pub const eswclk_hz: u32 = 250_000_000;

pub const ok: u16 = 0;
pub const hw_timeout: u16 = 0x203;

/// Poll `off` until `mask` reads set (any bit) or clear (all bits), at most
/// poll_limit reads (ra8_hw_wait_flag_set8 / ra8_hw_wait_flag_clear8).
fn wait(regs: anytype, off: usize, mask: u8, set: bool) u16 {
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        if ((regs.read8(off) & mask != 0) == set) return ok;
    }
    return hw_timeout;
}

/// Switch one clock to PLL1P with divider `div`: SREQ, wait SRDY, divider,
/// source|SREQ|SRDY (SRDY in the same write is mandatory), clear SREQ,
/// wait SRDY clear.
pub fn switchToPll1p(regs: anytype, ckcr: usize, divcr: usize, div: u8) u16 {
    regs.write8(ckcr, regs.read8(ckcr) | sreq);
    const err = wait(regs, ckcr, srdy, true);
    if (err != ok) return err;
    regs.write8(divcr, div);
    regs.write8(ckcr, (sel_pll1p & sel_mask) | sreq | srdy);
    regs.write8(ckcr, regs.read8(ckcr) & ~sreq);
    return wait(regs, ckcr, srdy, false);
}

/// Power the ESWM domain on when it is gated (PDCSF=0, PDPGSF=1), then wait
/// for both status flags to clear outside the PRCR window.
pub fn powerOnDomain(regs: anytype, ops: anytype) u16 {
    regs.write16(off_prcr, prcr.unlock_lpm);
    const state = regs.read8(off_pdctreswm);
    if (state & pdcsf == 0 and state & pdpgsf != 0) {
        regs.write8(off_pdctreswm, state & ~pdde);
    }
    regs.write16(off_prcr, prcr.lock_all);
    if (wait(regs, off_pdctreswm, pdcsf, false) != ok) {
        ops.err("eswclk: PDCSF stuck");
        return hw_timeout;
    }
    if (wait(regs, off_pdctreswm, pdpgsf, false) != ok) {
        ops.err("eswclk: PDPGSF stuck");
        return hw_timeout;
    }
    return ok;
}

/// ESWCKCR to PLL1P/4, then ESWPCKCR to PLL1P/2, under the CGC unlock.
/// PRCR is re-locked on every path (the C `break` skipped the re-lock).
pub fn programClocks(regs: anytype, ops: anytype) u16 {
    regs.write16(off_prcr, prcr.unlock_cgc);
    defer regs.write16(off_prcr, prcr.lock_all);
    var err = switchToPll1p(regs, off_eswckcr, off_eswckdivcr, div4);
    if (err != ok) {
        ops.err("eswclk: ESWCKCR handshake timeout");
        return err;
    }
    err = switchToPll1p(regs, off_eswpckcr, off_eswpckdivcr, div2);
    if (err != ok) ops.err("eswclk: ESWPCKCR handshake timeout");
    return err;
}

/// ra8_cgc_eswclk_init: HOCO, power domain, MSTPC28, clock switch. On
/// success `hz` becomes 250 MHz; on any error it is left as it was.
pub fn init(regs: anytype, ops: anytype, hz: *u32) u16 {
    ops.info("eswclk init (PLL1P/4 = 250 MHz, eswphyclk PLL1P/2 = 500 MHz)");
    const hoco = ops.hoco();
    if (hoco != ok) {
        ops.err("eswclk: HOCO stabilization timeout");
        return hoco;
    }
    const pd = powerOnDomain(regs, ops);
    if (pd != ok) return pd;
    const mstp = ops.mstpEnable(mstp_ethphyclk);
    if (mstp != ok) {
        ops.err("eswclk: ethphyclk MSTP release failed");
        return mstp;
    }
    const cks = programClocks(regs, ops);
    if (cks != ok) return cks;
    hz.* = eswclk_hz;
    ops.info("eswclk ready");
    return ok;
}
