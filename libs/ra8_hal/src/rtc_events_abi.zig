//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_rtc deinit, IRQ enable, status, handler attach and dispatch
//! (RA8FW-852). Logic lives in internal/rtc_events.zig.

const common = @import("abi_common.zig");
const ev = @import("internal/rtc_events.zig");

const tag = "RTC";
/// `k_ra8_rtc_base_addr` (ra8_rtc_regs.h); RCR1 +0x22, RCR2 +0x24.
const rtc_base: usize = 0x40202000;

var handler: ev.Handler = .{};

fn rcr1() *volatile u8 {
    return @ptrFromInt(rtc_base + 0x22);
}

fn rcr2() *volatile u8 {
    return @ptrFromInt(rtc_base + 0x24);
}

export fn ra8_rtc_deinit() u16 {
    ev.deinit(rcr1(), rcr2(), &handler);
    return common.k_ra8_ok;
}

export fn ra8_rtc_set_irq_enable(mask: u8) u16 {
    ev.setIrqEnable(rcr1(), mask);
    return common.k_ra8_ok;
}

export fn ra8_rtc_get_status(out_mask: ?*u8) u16 {
    const out = out_mask orelse {
        common.ra8_log_emit_error(tag, "out_mask must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    out.* = ev.status(rcr1());
    return common.k_ra8_ok;
}

export fn ra8_rtc_clear_status(mask: u8) u16 {
    ev.clearStatus(rcr1(), mask);
    return common.k_ra8_ok;
}

export fn ra8_rtc_attach_handler(func: ?ev.EventFn, ctx: ?*anyopaque) u16 {
    handler = .{ .func = func, .ctx = ctx };
    return common.k_ra8_ok;
}

export fn ra8_rtc_dispatch() void {
    ev.dispatch(rcr1(), &handler);
}
