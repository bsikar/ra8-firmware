//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the XSPI manual-command core, ra8_xspi_flash_read_status and
//! ra8_xspi_flash_read_id (RA8FW-869). Logic lives in internal/xspi_cmd.zig.
//! The priv_ exports keep their C names and prototypes in
//! ra8_xspi_internal.h; ra8_xspi_flash.c and xspi_reset_abi.zig call them.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const cmd = @import("internal/xspi_cmd.zig");
const ev = @import("internal/xspi_events.zig");

const tag = "XSPI";
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_poll(reg: *const volatile anyopaque, iter: u32, flag_set: bool) bool;
};

const Regs = struct {
    base: usize,

    fn ptr(self: Regs, off: usize) *volatile u32 {
        return @ptrFromInt(self.base + off);
    }
    pub fn read(self: Regs, off: usize) u32 {
        return self.ptr(off).*;
    }
    pub fn write(self: Regs, off: usize, v: u32) void {
        self.ptr(off).* = v;
    }
    pub fn poll(self: Regs, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_poll(self.ptr(cmd.off_ints), iter, cond) else cond;
    }
};

fn regsOf(reg: *volatile anyopaque) Regs {
    return .{ .base = @intFromPtr(reg) };
}

export fn priv_ra8_xspi_make_cdt(opcode: u8, cmd_bytes: u8, addr_bytes: u8, data_bytes: u8, is_write: u8) u32 {
    return cmd.makeCdt(opcode, cmd_bytes, addr_bytes, data_bytes, is_write);
}

export fn priv_ra8_xspi_kick_command(reg: *volatile anyopaque) u16 {
    return cmd.kick(regsOf(reg));
}

export fn priv_ra8_xspi_issue_simple_opcode(reg: *volatile anyopaque, opcode: u8) u16 {
    return cmd.issue(regsOf(reg), opcode, 0);
}

fn instanceRegs(instance: u8) ?Regs {
    const b = ev.instanceBase(instance) orelse {
        common.ra8_log_emit_error(tag, "instance out of range");
        return null;
    };
    return .{ .base = b };
}

export fn ra8_xspi_flash_read_status(instance: u8, out_status: ?*u8) u16 {
    const out = out_status orelse {
        common.ra8_log_emit_error(tag, "out_status must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    const regs = instanceRegs(instance) orelse return common.k_ra8_err_null_ptr;
    return cmd.readStatus(regs, out);
}

export fn ra8_xspi_flash_read_id(instance: u8, out_id: ?*u32) u16 {
    const out = out_id orelse {
        common.ra8_log_emit_error(tag, "out_id must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    const regs = instanceRegs(instance) orelse return common.k_ra8_err_null_ptr;
    return cmd.readId(regs, out);
}
