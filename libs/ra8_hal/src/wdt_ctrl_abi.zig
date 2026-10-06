//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_wdt_init, ra8_wdt_refresh_deferred, ra8_wdt_refresh_for,
//! the status calls and ra8_wdt_get_counter (RA8FW-891, the last of
//! ra8_wdt.c). Same check order, log text and error codes as the C.
//! HUM Ch 27.2.1 "WDTRR" p 1257, Ch 27.2.3 "WDTSR" pp 1260-1261.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const ctrl = @import("internal/wdt_ctrl.zig");

const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_poll(reg: *const volatile anyopaque, iter: u32, flag_set: bool) bool;
};

const tag = "WDT";
const instance_count: u8 = 2;
const clear_max_polls: u32 = 0x4000;

const Regs = struct {
    base: usize,

    fn wdtrr(self: Regs) *volatile u8 {
        return @ptrFromInt(self.base + 0x00);
    }
    fn wdtcr(self: Regs) *volatile u16 {
        return @ptrFromInt(self.base + 0x02);
    }
    fn wdtsr(self: Regs) *volatile u16 {
        return @ptrFromInt(self.base + 0x04);
    }
    fn wdtrcr(self: Regs) *volatile u8 {
        return @ptrFromInt(self.base + 0x06);
    }
    fn wdtcstpr(self: Regs) *volatile u8 {
        return @ptrFromInt(self.base + 0x08);
    }
    /// The 0x00 / 0xFF pair arms the counter in register-start mode.
    fn refresh(self: Regs) void {
        self.wdtrr().* = 0x00;
        self.wdtrr().* = 0xFF;
    }
};

const wdt0: Regs = .{ .base = 0x4020_2600 };
const wdt1: Regs = .{ .base = 0x4020_2700 };

fn nullOut(message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, message);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_wdt_init(cfg_ptr: ?*const ctrl.Cfg) u16 {
    const cfg = (cfg_ptr orelse return nullOut("cfg must not be nullptr")).*;
    if (!ctrl.valid(cfg)) return common.k_ra8_err_invalid_arg;
    wdt0.wdtcr().* = ctrl.packWdtcr(cfg);
    wdt0.wdtrcr().* = ctrl.rcr(cfg);
    wdt0.wdtcstpr().* = ctrl.cstpr(cfg);
    wdt0.refresh();
    common.ra8_log_emit_info(tag, "wdt_init armed");
    return common.k_ra8_ok;
}

export fn ra8_wdt_refresh_deferred() void {
    wdt0.refresh();
}

export fn ra8_wdt_refresh_for(which: u8) u16 {
    if (which >= instance_count) return common.k_ra8_err_invalid_arg;
    (if (which == 1) wdt1 else wdt0).refresh();
    return common.k_ra8_ok;
}

export fn ra8_wdt_get_status(out_mask: ?*u16) u16 {
    const out = out_mask orelse return nullOut("out_mask must not be nullptr");
    out.* = wdt0.wdtsr().* & ctrl.status_all;
    return common.k_ra8_ok;
}

/// UNDFF / REFEF are write-0-to-clear; the other bits are written back.
export fn ra8_wdt_clear_status() u16 {
    const sr = wdt0.wdtsr();
    sr.* = sr.* & ~ctrl.status_all;
    return common.k_ra8_ok;
}

export fn ra8_wdt_clear_status_blocking(mask: u16) u16 {
    if (!ctrl.clearMaskValid(mask)) return common.k_ra8_err_invalid_arg;
    if (mask == 0) return common.k_ra8_ok;
    const sr = wdt0.wdtsr();
    var poll: u32 = 0;
    while (poll < clear_max_polls) : (poll += 1) {
        sr.* = sr.* & ~mask;
        const cond = sr.* & mask == 0;
        const cleared = if (hosted) seam.ra8_fake_mmio_poll(sr, poll, cond) else cond;
        if (cleared) return common.k_ra8_ok;
    }
    common.ra8_log_emit_error(tag, "wdt_clear_status_blocking timed out");
    return common.k_ra8_err_hw_timeout;
}

export fn ra8_wdt_get_counter(out_count: ?*u16) u16 {
    const out = out_count orelse return nullOut("out_count must not be nullptr");
    out.* = wdt0.wdtsr().* & ctrl.counter_mask;
    return common.k_ra8_ok;
}
