//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for XSPI XIP, DTR mode and DQS calibration (RA8FW-866). The
//! register logic is in internal/xspi_xip.zig.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const ev = @import("internal/xspi_events.zig");
const xip = @import("internal/xspi_xip.zig");

const tag = "XSPI";

/// Host C tests arm failures through this seam, as ra8_hw_wait_flag_clear32
/// does under UNIT_TEST. Freestanding builds never see it.
const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
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
    pub fn eval(self: Regs, off: usize, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_wait_eval(self.ptr(off), iter, cond) else cond;
    }
};

fn regs(instance: u8) ?Regs {
    const b = ev.instanceBase(instance) orelse return null;
    return .{ .base = b };
}

fn badInstance() u16 {
    common.ra8_log_emit_error(tag, "instance out of range");
    return common.k_ra8_err_null_ptr;
}

export fn ra8_xspi_xip_enter(instance: u8, enter_code: u8, exit_code: u8) u16 {
    const r = regs(instance) orelse return badInstance();
    xip.enter(r, enter_code, exit_code);
    return 0;
}

export fn ra8_xspi_xip_exit(instance: u8) u16 {
    const r = regs(instance) orelse return badInstance();
    xip.exit(r);
    return 0;
}

export fn ra8_xspi_set_xip_mode(instance: u8, enable: bool, read_cmd: u8, addr_bytes: u8) u16 {
    const r = regs(instance) orelse return badInstance();
    xip.setMode(r, enable, read_cmd, addr_bytes) catch return common.k_ra8_err_invalid_arg;
    return 0;
}

export fn ra8_xspi_set_dtr_mode(instance: u8, enable: bool) u16 {
    const r = regs(instance) orelse return badInstance();
    xip.setDtr(r, enable);
    return 0;
}

export fn ra8_xspi_calibrate_dqs(instance: u8) u16 {
    const r = regs(instance) orelse return badInstance();
    xip.calibrate(r) catch return common.k_ra8_err_hw_timeout;
    return 0;
}
