//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The store seam: real MMIO on silicon, a recorded capture off it.
//!
//! Which side is live is decided from the build target rather than a flag a
//! caller can get wrong: a freestanding Thumb build is the firmware, anything
//! else is a host test. That keeps the sequencer in `ra8_tz_secure_boot_abi`
//! written once, with no `#ifdef` threaded through the steps.

const builtin = @import("builtin");
const build_options = @import("build_options");
const boot = @import("boot.zig");

/// True when this build actually has the registers.
pub const on_target: bool =
    builtin.target.os.tag == .freestanding and builtin.target.cpu.arch.isThumb();

/// True when loads and stores reach real addresses. That is silicon, and also
/// the host archive the C suites link: there `tests/mocks/src/ra8_fake_mmap.c`
/// backs these same addresses, as it does for ra8_core's SysTick window, so
/// the suites can stage and inspect them. The Zig unit tests have no such map,
/// so their build records into `boot.host` instead (`-Dhost-capture`).
pub const live_registers: bool = on_target or !build_options.host_capture;

/// Store a 32-bit register.
pub fn write32(addr: usize, value: u32) void {
    if (comptime live_registers) {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    } else {
        boot.host.write32(addr, value);
    }
}

/// Store the 16-bit PRCR_S.
pub fn write16(addr: usize, value: u16) void {
    if (comptime live_registers) {
        @as(*volatile u16, @ptrFromInt(addr)).* = value;
    } else {
        boot.host.write16(value);
    }
}

/// Read a 32-bit register back.
pub fn read32(addr: usize) u32 {
    if (comptime live_registers) {
        return @as(*const volatile u32, @ptrFromInt(addr)).*;
    }
    return boot.host.read32(addr);
}

/// Order the stores above against what follows.
pub fn barrier() void {
    if (comptime on_target) asm volatile ("dsb sy" ::: .{ .memory = true });
}
