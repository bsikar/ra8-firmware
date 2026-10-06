//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_i2c.h error-status helpers (RA8FW-694), moved out of
//! ra8_i2c_config.c. Init and deinit moved to i2c_config_abi.zig (RA8FW-702).

const common = @import("abi_common.zig");
const st = @import("internal/i2c_status.zig");

/// Owned by i2c_xfer_abi.zig, declared in ra8_i2c_internal.h.
extern const g_i2c_tag: [*:0]const u8;

fn icsr2(channel: u8) ?*volatile u8 {
    const addr = st.icsr2Addr(channel) orelse return null;
    return @ptrFromInt(addr);
}

export fn ra8_i2c_get_errors(channel: u8, out_mask: ?*u8) u16 {
    const out = out_mask orelse {
        common.ra8_log_emit_error(g_i2c_tag, "i2c_get_errors: out_mask");
        return common.k_ra8_err_null_ptr;
    };
    const reg = icsr2(channel) orelse return common.k_ra8_err_invalid_arg;
    out.* = st.decode(reg.*);
    return common.k_ra8_ok;
}

export fn ra8_i2c_clear_errors(channel: u8) u16 {
    const reg = icsr2(channel) orelse return common.k_ra8_err_invalid_arg;
    st.clear(reg);
    return common.k_ra8_ok;
}
