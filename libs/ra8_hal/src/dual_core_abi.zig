//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_dual_core.h (RA8FW-766); replaces ra8_dual_core.c. Logic
//! lives in internal/dual_core.zig. Host builds run on the off-target fake
//! registers and export ra8_dual_core_test_actcsr_key so the C suites can
//! inject a stuck ACT through the fake-MMIO wait seam. Freestanding builds
//! drive the CPU_CTRL block at 0x4000F000.

const std = @import("std");
const builtin = @import("builtin");
const common = @import("abi_common.zig");
const dc = @import("internal/dual_core.zig");

const tag = "ra8_dual_core";
const hosted = builtin.os.tag != .freestanding;
/// CPU1 is the Cortex-M33 (the C keyed this on RA8_BUILD_FOR_CPU1).
const is_cpu0 = !std.mem.eql(u8, builtin.cpu.model.name, "cortex_m33");

/// `k_ra8_dual_core_ctrl_base_addr` and the CPU1 register offsets.
const ctrl_base: usize = 0x4000F000;
const off_initvtor: usize = 0x044;
const off_waitcr: usize = 0x054;
const off_actcsr: usize = 0x064;

const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

var fake: dc.Fake = .{};

fn actcsrTestKey() callconv(.c) *const volatile anyopaque {
    return &fake.actcsr;
}

comptime {
    if (hosted) @export(&actcsrTestKey, .{ .name = "ra8_dual_core_test_actcsr_key" });
}

const Hw = struct {
    pub fn actcsrRead(_: Hw) u16 {
        if (hosted) return fake.actcsr;
        return @as(*volatile u16, @ptrFromInt(ctrl_base + off_actcsr)).*;
    }
    pub fn actcsrWrite(_: Hw, value: u16) void {
        if (hosted) return fake.writeActcsr(value);
        @as(*volatile u16, @ptrFromInt(ctrl_base + off_actcsr)).* = value;
    }
    pub fn waitcrRead(_: Hw) u8 {
        if (hosted) return fake.waitcr;
        return @as(*volatile u8, @ptrFromInt(ctrl_base + off_waitcr)).*;
    }
    pub fn waitcrWrite(_: Hw, value: u8) void {
        if (hosted) return fake.writeWaitcr(value);
        @as(*volatile u8, @ptrFromInt(ctrl_base + off_waitcr)).* = value;
    }
    pub fn initvtorWrite(_: Hw, value: u32) void {
        if (hosted) {
            fake.initvtor = value;
            return;
        }
        @as(*volatile u32, @ptrFromInt(ctrl_base + off_initvtor)).* = value;
    }
    pub fn actPoll(_: Hw, iter: u32, cond: bool) bool {
        return if (hosted) seam.ra8_fake_mmio_wait_eval(&fake.actcsr, iter, cond) else cond;
    }
    pub fn logError(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

export fn ra8_cpu1_release(entry: ?*anyopaque, sp: ?*anyopaque) u16 {
    return dc.release(Hw{}, is_cpu0, entry, sp);
}

export fn ra8_cpu1_halt() u16 {
    return dc.halt(Hw{}, is_cpu0);
}

export fn ra8_cpu1_is_running() bool {
    return dc.isRunning(Hw{});
}
