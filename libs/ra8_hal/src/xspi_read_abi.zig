//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the XSPI flash read path (RA8FW-870). Logic lives in
//! internal/xspi_read.zig. The two priv_ exports are prototyped in
//! ra8_xspi_internal.h for the program and erase paths still in
//! ra8_xspi_flash.c.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const cmd = @import("internal/xspi_cmd.zig");
const ev = @import("internal/xspi_events.zig");
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

export fn priv_ra8_xspi_flash_range_check(flash_addr: u32, len: u32) u16 {
    return rd.rangeCheck(flash_addr, len);
}

export fn priv_ra8_xspi_build_chunk_header(reg: *volatile anyopaque, opcode: u8, addr: u32, data_bytes: u8, is_write: u8) void {
    rd.chunkHeader(Regs{ .base = @intFromPtr(reg) }, opcode, addr, data_bytes, is_write);
}

export fn ra8_xspi_flash_read(instance: u8, flash_addr: u32, buf: ?[*]u8, len: u32) u16 {
    const out = buf orelse {
        common.ra8_log_emit_error(tag, "buf must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (len == 0 or len > rd.max_xfer) return common.k_ra8_err_invalid_arg;
    const base = ev.instanceBase(instance) orelse {
        common.ra8_log_emit_error(tag, "instance out of range");
        return common.k_ra8_err_null_ptr;
    };
    return rd.read(Regs{ .base = base }, flash_addr, out[0..len]);
}
