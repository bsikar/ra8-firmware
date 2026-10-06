//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_rtc_enter_stop/exit_stop and the RTC register wait
//! (RA8FW-853). Logic lives in internal/rtc_stop.zig.
//! priv_ra8_rtc_internal_wait_bit serves the C left in ra8_rtc.c.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const s = @import("internal/rtc_stop.zig");

/// `k_ra8_rtc_base_addr` (ra8_rtc_regs.h); RCR2 +0x24.
const rcr2_addr: usize = 0x40202000 + 0x24;

/// Host builds go through the C fake-MMIO wait seam (ra8_hw_err.h).
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

const Hw = struct {
    pub fn eval(_: Hw, reg: *volatile u8, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_wait_eval(reg, iter, cond) else cond;
    }
};

fn rcr2() *volatile u8 {
    return @ptrFromInt(rcr2_addr);
}

export fn priv_ra8_rtc_internal_wait_bit(reg: *volatile u8, mask: u8, expect: u8) void {
    s.waitBit(Hw{}, reg, mask, expect);
}

export fn ra8_rtc_enter_stop() u16 {
    s.enterStop(Hw{}, rcr2());
    return common.k_ra8_ok;
}

export fn ra8_rtc_exit_stop() u16 {
    s.exitStop(Hw{}, rcr2());
    return common.k_ra8_ok;
}
