//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_sci_baud_calculate and the CCR encoders ra8_sci.c
//! still calls (RA8FW-905). HUM Ch 38.2.6-38.2.8.

const common = @import("abi_common.zig");
const cfg_mod = @import("internal/sci_cfg.zig");

const tag = "SCI";

export fn priv_ra8_sci_brr(pclk_hz: u32, baud: u32) u8 {
    return cfg_mod.brr(pclk_hz, baud);
}

export fn priv_ra8_sci_ccr1(cfg: *const cfg_mod.Cfg) u32 {
    return cfg_mod.ccr1(cfg.*);
}

export fn priv_ra8_sci_ccr2(cfg: *const cfg_mod.Cfg) u32 {
    return cfg_mod.ccr2(cfg.*);
}

export fn priv_ra8_sci_ccr3(cfg: *const cfg_mod.Cfg) u32 {
    return cfg_mod.ccr3(cfg.*);
}

export fn ra8_sci_baud_calculate(baud: u32, pclk_hz: u32, brr_out: ?*u16, clk_div_out: ?*u8) u16 {
    const b = brr_out orelse {
        common.ra8_log_emit_error(tag, "baud_calc: brr_out");
        return common.k_ra8_err_null_ptr;
    };
    const c = clk_div_out orelse {
        common.ra8_log_emit_error(tag, "baud_calc: clk_div_out");
        return common.k_ra8_err_null_ptr;
    };
    const r = cfg_mod.baudCalculate(baud, pclk_hz) orelse return common.k_ra8_err_invalid_arg;
    b.* = r.brr;
    c.* = r.cks;
    return common.k_ra8_ok;
}
