//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for XSPI flash page program and sector erase (RA8FW-871).
//! Logic lives in internal/xspi_program.zig. ra8_xspi_flash.c is gone.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const cmd = @import("internal/xspi_cmd.zig");
const ev = @import("internal/xspi_events.zig");
const pg = @import("internal/xspi_program.zig");
const rd = @import("internal/xspi_read.zig");

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

fn instanceRegs(instance: u8) ?Regs {
    const base = ev.instanceBase(instance) orelse {
        common.ra8_log_emit_error(tag, "instance out of range");
        return null;
    };
    return Regs{ .base = base };
}

export fn ra8_xspi_flash_program(instance: u8, flash_addr: u32, data: ?[*]const u8, len: u32) u16 {
    const src = data orelse {
        common.ra8_log_emit_error(tag, "data must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (len == 0 or len > rd.max_xfer) return common.k_ra8_err_invalid_arg;
    const regs = instanceRegs(instance) orelse return common.k_ra8_err_null_ptr;
    return pg.program(regs, flash_addr, src[0..len]);
}

export fn ra8_xspi_flash_erase_sector(instance: u8, flash_addr: u32) u16 {
    const regs = instanceRegs(instance) orelse return common.k_ra8_err_null_ptr;
    return pg.eraseSector(regs, flash_addr);
}
