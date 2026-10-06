//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CANFD block clock, ra8_canfd_init and ra8_canfd_deinit (RA8FW-864, the
//! last of ra8_canfd.c). Exports live in src/canfd_init_abi.zig.
//! HUM Ch 9.2.41 / 9.2.46 (CANFDCKDIVCR, CANFDCKCR), Ch 41 p 2730.

const ctrl = @import("canfd_ctrl.zig");

pub const channels: u8 = 2;
/// `k_ra8_canfd_ckcr_spin`, `k_ra8_canfd_spin`.
pub const ckcr_spin: u32 = 262144;
pub const graminit_spin: u32 = 20000;
/// CANFDCKCR: source MOCO, SREQ bit 6, SRDY bit 7.
pub const src_moco: u8 = 0x01;
pub const sreq: u8 = 1 << 6;
pub const srdy: u8 = 1 << 7;
/// CFDGSTS GRAMINIT, bit 3.
pub const off_gsts: usize = 0x01C;
pub const graminit: u32 = 1 << 3;
pub const prcr_unlock_cgc: u16 = 0xA501;
pub const prcr_lock_all: u16 = 0xA500;
pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

fn clockHandshake(hw: anytype) u16 {
    hw.writeDivcr(0);
    hw.writeCkcr(src_moco | sreq);
    if (!hw.waitSrdy(true)) {
        hw.err("canfd: CANFDCKSRDY=1 timeout");
        return hw_timeout;
    }
    hw.writeCkcr(src_moco);
    if (!hw.waitSrdy(false)) {
        hw.err("canfd: CANFDCKSRDY=0 timeout");
        return hw_timeout;
    }
    return 0;
}

/// Switch CANFDCLK to MOCO /1 once per boot. PRCR is re-locked on every
/// path (the C left it unlocked after an SRDY timeout).
pub fn clockInit(hw: anytype, done: *bool) u16 {
    if (done.*) return 0;
    hw.prcr(prcr_unlock_cgc);
    const rc = clockHandshake(hw);
    hw.prcr(prcr_lock_all);
    if (rc != 0) return rc;
    done.* = true;
    hw.info("canfd block clock stable");
    return 0;
}

/// Clock first: MSTPC26/27 must be written after CANFDCLK is stable.
pub fn init(hw: anytype, channel: u8, done: *bool) u16 {
    if (channel >= channels) {
        hw.err("channel out of range");
        return null_ptr;
    }
    const clk = clockInit(hw, done);
    if (clk != 0) return clk;
    const mst = hw.mstpEnable(ctrl.mstp_ids[channel]);
    if (mst != 0) {
        hw.fail("canfd_init: mstp enable", mst);
        return mst;
    }
    hw.waitGramInit(channel);
    const open = hw.openChannel(channel);
    if (open != 0) return open;
    hw.infoVal("canfd_init ch", channel);
    return 0;
}

pub fn deinit(hw: anytype, channel: u8) u16 {
    if (channel >= channels) {
        hw.err("channel out of range");
        return null_ptr;
    }
    _ = hw.channelReset(channel);
    return 0;
}
