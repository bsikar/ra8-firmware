//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the I3C legacy-I2C controller error flags
//! (internal/i3c_i2c_errors.zig, RA8FW-693). Check order and log line
//! match the deleted C in ra8_i3c_i2c_control.c.

const common = @import("abi_common.zig");
const p = @import("internal/i3c_i2c_peripheral.zig");
const errs = @import("internal/i3c_i2c_errors.zig");

const tag = "IIC_B";

fn bst(channel: u8) ?*volatile u32 {
    const block = p.regsFor(channel) orelse return null;
    return block.reg(p.off_bst);
}

/// `ra8_err_t ra8_i3c_i2c_get_errors(uint8_t, uint8_t*)`.
export fn ra8_i3c_i2c_get_errors(channel: u8, out_mask: ?*u8) u16 {
    const out = out_mask orelse {
        common.ra8_log_emit_error(tag, "iic_b_get_errors: out_mask");
        return common.k_ra8_err_null_ptr;
    };
    const reg = bst(channel) orelse return common.k_ra8_err_invalid_arg;
    out.* = errs.decode(reg.*);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_i3c_i2c_clear_errors(uint8_t)`.
export fn ra8_i3c_i2c_clear_errors(channel: u8) u16 {
    const reg = bst(channel) orelse return common.k_ra8_err_invalid_arg;
    errs.clear(reg);
    return common.k_ra8_ok;
}
