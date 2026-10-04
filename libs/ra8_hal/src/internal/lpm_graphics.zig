//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Graphics power domain bring-up (HUM Ch 11.2.14 PDCTRGD, Ch 9.2.18
//! MOCOCR). Port of ra8_lpm_graphics.c (RA8FW-556); the ABI file maps the
//! errors below back onto the C's log lines.

const prcr = @import("prcr.zig");
/// Re-exported for the host tests (the test module sees only this root).
pub const prcr_mod = prcr;

/// SYSC base (`k_ra8_lpm_sysc_base_addr`).
pub const base: usize = 0x4001E000;
pub const off_mococr: usize = 0x038;
pub const off_pdctrgd: usize = 0x110;
pub const off_prcr: usize = prcr.addr - base;
/// Bytes a host test buffer must cover.
pub const window_len: usize = off_prcr + 2;

/// STOP @ bit 0 in every oscillator control register.
pub const clock_stop_mask: u8 = 0x01;
/// PDCSF @ bit 6: control sequence busy.
pub const pdcsf_mask: u8 = 0x40;
/// PDPGSF @ bit 7: 1 = domain gated off.
pub const pdpgsf_mask: u8 = 0x80;
/// `k_ra8_lpm_pd_timeout_default`.
pub const timeout_default: u32 = 100_000;

/// Each failure edge of the C sequence, in order.
pub const Error = error{
    /// timeout_iters == 0.
    ZeroTimeout,
    /// PDCSF never cleared before PDDE was cleared.
    BusyBeforeOn,
    /// PDPGSF never read as set before PDDE was cleared.
    NotReady,
    /// PDCSF never cleared after PDDE was cleared.
    StuckAfterOn,
    /// PDPGSF never cleared after PDDE was cleared.
    StillGated,
};

pub const Outcome = enum { already_on, powered_on };

/// The SYSC block, by base address so host tests can use a buffer.
pub const Block = struct {
    base: usize,

    fn reg(block: Block, comptime T: type, off: usize) *volatile T {
        return @ptrFromInt(block.base + off);
    }

    pub fn pdctrgd(block: Block) *volatile u8 {
        return block.reg(u8, off_pdctrgd);
    }

    pub fn mococr(block: Block) *volatile u8 {
        return block.reg(u8, off_mococr);
    }

    pub fn prcrReg(block: Block) *volatile u16 {
        return block.reg(u16, off_prcr);
    }
};

/// Poll PDCTRGD until `mask` reads as `want_set`, at most `limit` reads.
pub fn waitFlag(block: Block, mask: u8, want_set: bool, limit: u32) bool {
    var i: u32 = 0;
    while (i < limit) : (i += 1) {
        if (((block.pdctrgd().* & mask) != 0) == want_set) return true;
    }
    return false;
}

fn enableMoco(block: Block) void {
    const window = prcr.open(block.prcrReg(), prcr.unlock_cgc);
    defer window.close();
    const r = block.mococr();
    r.* = r.* & ~clock_stop_mask;
}

fn clearPdde(block: Block) void {
    const window = prcr.open(block.prcrReg(), prcr.unlock_lpm);
    defer window.close();
    block.pdctrgd().* = 0;
}

/// `ra8_lpm_graphics_power_on`: idempotent when PDPGSF is already clear.
pub fn powerOn(block: Block, timeout_iters: u32) Error!Outcome {
    if (timeout_iters == 0) return error.ZeroTimeout;
    if ((block.pdctrgd().* & pdpgsf_mask) == 0) return .already_on;
    enableMoco(block);
    if (!waitFlag(block, pdcsf_mask, false, timeout_iters)) return error.BusyBeforeOn;
    if (!waitFlag(block, pdpgsf_mask, true, timeout_iters)) return error.NotReady;
    clearPdde(block);
    if (!waitFlag(block, pdcsf_mask, false, timeout_iters)) return error.StuckAfterOn;
    if (!waitFlag(block, pdpgsf_mask, false, timeout_iters)) return error.StillGated;
    return .powered_on;
}
