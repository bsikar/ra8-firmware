//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the RIIC controller bus primitives (RA8FW-886), prototyped
//! in ra8_i2c_internal.h for the controller transfers still in
//! ra8_i2c.c. Logic lives in internal/i2c_bus.zig.

const builtin = @import("builtin");
const bus = @import("internal/i2c_bus.zig");

const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_poll(reg: *const volatile anyopaque, iter: u32, flag_set: bool) bool;
};

const Regs = struct {
    base: usize,

    fn ptr(self: Regs, off: usize) *volatile u8 {
        return @ptrFromInt(self.base + off);
    }
    pub fn read8(self: Regs, off: usize) u8 {
        return self.ptr(off).*;
    }
    pub fn write8(self: Regs, off: usize, v: u8) void {
        self.ptr(off).* = v;
    }
    pub fn poll(self: Regs, off: usize, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_poll(self.ptr(off), iter, cond) else cond;
    }
};

fn regsOf(reg: *volatile anyopaque) Regs {
    return .{ .base = @intFromPtr(reg) };
}

export fn priv_ra8_i2c_bus_wait_icsr2(reg: *volatile anyopaque, mask: u8) u16 {
    return bus.waitIcsr2(regsOf(reg), mask);
}

export fn priv_ra8_i2c_bus_status(icsr2: u8) u16 {
    return bus.status(icsr2);
}

export fn priv_ra8_i2c_bus_clear_status(reg: *volatile anyopaque) void {
    bus.clearStatus(regsOf(reg));
}

export fn priv_ra8_i2c_bus_open(reg: *volatile anyopaque, bus_held: bool) void {
    bus.open(regsOf(reg), bus_held);
}

export fn priv_ra8_i2c_bus_stop_request(reg: *volatile anyopaque) void {
    bus.stopRequest(regsOf(reg));
}

export fn priv_ra8_i2c_bus_stop(reg: *volatile anyopaque) void {
    bus.stop(regsOf(reg));
}

export fn priv_ra8_i2c_bus_wait_free(reg: *volatile anyopaque) void {
    bus.waitFree(regsOf(reg));
}

export fn priv_ra8_i2c_bus_set_nack(reg: *volatile anyopaque) void {
    bus.setNack(regsOf(reg));
}

export fn priv_ra8_i2c_bus_busy_gate(reg: *volatile anyopaque, bus_held: bool) u16 {
    return bus.busyGate(regsOf(reg), bus_held);
}

export fn priv_ra8_i2c_bus_send_address(reg: *volatile anyopaque, byte: u8) u16 {
    return bus.sendAddress(regsOf(reg), byte);
}
