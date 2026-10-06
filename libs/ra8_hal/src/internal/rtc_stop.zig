//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RTC bounded register wait and enter/exit stop (RA8FW-853, was part of
//! ra8_rtc.c, now deleted). Exports live in src/rtc_stop_abi.zig. `hw` supplies
//! eval(reg, iter, cond), the host fake-MMIO seam on hosted builds.

/// `k_ra8_rtc_wait_iters`. Bounded per NASA Rule 2; a timeout returns
/// silently, as the C did.
pub const wait_iters: u32 = 10000;
/// RCR2.START, bit 0 (HUM Ch 26.2.21 p 1232): 0 = stop, 1 = run.
pub const rcr2_start: u8 = 0x01;

pub fn waitBit(hw: anytype, reg: *volatile u8, mask: u8, expect: u8) void {
    var i: u32 = 0;
    while (i < wait_iters) : (i += 1) {
        if (hw.eval(reg, i, (reg.* & mask) == expect)) return;
    }
}

/// Clear START to halt the counter and wait for it to read back.
pub fn enterStop(hw: anytype, rcr2: *volatile u8) void {
    rcr2.* &= ~rcr2_start;
    waitBit(hw, rcr2, rcr2_start, 0);
}

/// Set START to resume the counter and wait for it to read back.
pub fn exitStop(hw: anytype, rcr2: *volatile u8) void {
    rcr2.* |= rcr2_start;
    waitBit(hw, rcr2, rcr2_start, rcr2_start);
}
