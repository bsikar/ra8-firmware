//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_wdt_timeout_cycles_get, ra8_wdt_pclkb_divisor and
//! ra8_wdt_total_pclkb_cycles (RA8FW-889). Same check order, log text
//! and error codes as the C. Logic lives in internal/wdt_timing.zig.

const common = @import("abi_common.zig");
const timing = @import("internal/wdt_timing.zig");

const tag = "WDT";

fn nullOut(message: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, message);
    return common.k_ra8_err_null_ptr;
}

export fn ra8_wdt_timeout_cycles_get(sel: u8, out_cycles: ?*u16) u16 {
    const out = out_cycles orelse return nullOut("out_cycles must not be nullptr");
    out.* = timing.cycles(sel) orelse return common.k_ra8_err_invalid_arg;
    return common.k_ra8_ok;
}

export fn ra8_wdt_pclkb_divisor(div: u8, out_divisor: ?*u16) u16 {
    const out = out_divisor orelse return nullOut("out_divisor must not be nullptr");
    out.* = timing.divisor(div) orelse return common.k_ra8_err_invalid_arg;
    return common.k_ra8_ok;
}

/// The timeout is checked before the divisor; the first error wins.
export fn ra8_wdt_total_pclkb_cycles(sel: u8, div: u8, out_pclkb_cycles: ?*u32) u16 {
    const out = out_pclkb_cycles orelse return nullOut("out_pclkb_cycles must not be nullptr");
    out.* = timing.total(sel, div) orelse return common.k_ra8_err_invalid_arg;
    return common.k_ra8_ok;
}
