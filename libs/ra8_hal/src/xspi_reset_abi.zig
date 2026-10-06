//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for XSPI suspend, resume and software reset (RA8FW-867). The
//! manual-command helpers are Zig exports in xspi_cmd_abi.zig.

const common = @import("abi_common.zig");
const ev = @import("internal/xspi_events.zig");
const rst = @import("internal/xspi_reset.zig");

const tag = "XSPI";

extern fn priv_ra8_xspi_issue_simple_opcode(reg: *volatile anyopaque, opcode: u8) u16;
extern fn priv_ra8_xspi_kick_command(reg: *volatile anyopaque) u16;

const Ops = struct {
    base: usize,

    pub fn write(self: Ops, off: usize, v: u32) void {
        @as(*volatile u32, @ptrFromInt(self.base + off)).* = v;
    }
    pub fn kick(self: Ops) u16 {
        return priv_ra8_xspi_kick_command(@ptrFromInt(self.base));
    }
};

fn badInstance() u16 {
    common.ra8_log_emit_error(tag, "instance out of range");
    return common.k_ra8_err_null_ptr;
}

fn simple(instance: u8, opcode: u8) u16 {
    const b = ev.instanceBase(instance) orelse return badInstance();
    return priv_ra8_xspi_issue_simple_opcode(@ptrFromInt(b), opcode);
}

export fn ra8_xspi_suspend(instance: u8) u16 {
    return simple(instance, rst.op_suspend);
}

export fn ra8_xspi_resume(instance: u8) u16 {
    return simple(instance, rst.op_resume);
}

export fn ra8_xspi_software_reset(instance: u8, cmd_bytes: u8) u16 {
    const b = ev.instanceBase(instance) orelse return badInstance();
    return rst.reset(Ops{ .base = b }, cmd_bytes) catch common.k_ra8_err_invalid_arg;
}
