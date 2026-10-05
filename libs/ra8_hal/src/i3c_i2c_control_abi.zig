//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the I3C legacy-I2C controller error flags
//! (internal/i3c_i2c_errors.zig) and bus probe (internal/i3c_i2c_scan.zig),
//! RA8FW-693. Check order and log line
//! match the deleted C in ra8_i3c_i2c_control.c.

const common = @import("abi_common.zig");
const p = @import("internal/i3c_i2c_peripheral.zig");
const errs = @import("internal/i3c_i2c_errors.zig");
const scan = @import("internal/i3c_i2c_scan.zig");

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

extern fn priv_i3c_i2c_start(reg: *anyopaque) void;
extern fn priv_i3c_i2c_stop(reg: *anyopaque) void;
extern fn priv_i3c_i2c_clear_bst(reg: *anyopaque) void;
extern fn priv_i3c_i2c_send_address(reg: *anyopaque, address_byte: u8) u16;

/// The C transaction helpers in ra8_i3c_i2c.c, bound to one block.
const CBus = struct {
    reg: *anyopaque,

    pub fn start(self: CBus) void {
        priv_i3c_i2c_start(self.reg);
    }
    pub fn stop(self: CBus) void {
        priv_i3c_i2c_stop(self.reg);
    }
    pub fn clearBst(self: CBus) void {
        priv_i3c_i2c_clear_bst(self.reg);
    }
    pub fn sendAddress(self: CBus, byte: u8) u16 {
        return priv_i3c_i2c_send_address(self.reg, byte);
    }
};

/// `ra8_err_t ra8_i3c_i2c_scan(uint8_t, uint8_t, bool*)`.
export fn ra8_i3c_i2c_scan(channel: u8, target_7b: u8, out_acked: ?*bool) u16 {
    const block = p.regsFor(channel) orelse {
        common.ra8_log_emit_error(tag, "iic_b_scan: channel");
        return common.k_ra8_err_null_ptr;
    };
    const out = out_acked orelse {
        common.ra8_log_emit_error(tag, "iic_b_scan: out_acked");
        return common.k_ra8_err_null_ptr;
    };
    const bus = CBus{ .reg = @ptrFromInt(block.base) };
    return scan.run(bus, block.reg(p.off_bst), target_7b, out);
}
