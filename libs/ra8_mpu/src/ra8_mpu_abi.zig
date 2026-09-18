//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_mpu/inc/ra8_mpu.h`. The validation and
//! encoding rules live in `internal/root.zig`; this file owns the exported
//! symbols, the MMIO register block, the argument guards in their original
//! order, the `ra8_err_t` mapping, and the one diagnostic line per guard the
//! C emitted through `RA8_CHECK_NULL_PTR`.
//!
//! Register layout and field positions follow the Arm Cortex-M85 TRM "MPU
//! register summary"; SHCSR.MEMFAULTENA follows Armv8-M ARM B3.2.10.

const std = @import("std");
const builtin = @import("builtin");
const build_config = @import("build_config");
const implementation = @import("internal/root.zig");

/// One MPU region descriptor (`ra8_mpu_region_t`).
pub const Region = implementation.Region;
/// Whole-MPU static configuration (`ra8_mpu_cfg_t`).
pub const Config = implementation.Config;

/// Subset of `ra8_err_t` this library returns.
pub const MpuError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    null_ptr = 0x504,
};

/// Component tag on the library's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "MPU";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

// The host build routes the barriers through the stub translation unit the C
// header declares under RA8_OFF_TARGET; a freestanding build emits them
// inline. `dsb sy` / `isb sy` are the assembler spellings of the C's
// `dsb 0xF` / `isb 0xF`: option 0xF is SY.
extern fn ra8_hw_dsb() void;
extern fn ra8_hw_isb() void;

inline fn dsb() void {
    if (build_config.off_target) {
        ra8_hw_dsb();
    } else {
        asm volatile ("dsb sy" ::: "memory");
    }
}

inline fn isb() void {
    if (build_config.off_target) {
        ra8_hw_isb();
    } else {
        asm volatile ("isb sy" ::: "memory");
    }
}

/// Architectural Cortex-M85 MPU register block (`r_mpu_regs_t`).
pub const Regs = extern struct {
    TYPE: u32,
    CTRL: u32,
    RNR: u32,
    RBAR: u32,
    RLAR: u32,
    RBAR_A1: u32,
    RLAR_A1: u32,
    RBAR_A2: u32,
    RLAR_A2: u32,
    RBAR_A3: u32,
    RLAR_A3: u32,
    reserved0: u32,
    MAIR0: u32,
    MAIR1: u32,
};

comptime {
    std.debug.assert(@offsetOf(Regs, "TYPE") == 0x00);
    std.debug.assert(@offsetOf(Regs, "CTRL") == 0x04);
    std.debug.assert(@offsetOf(Regs, "RNR") == 0x08);
    std.debug.assert(@offsetOf(Regs, "RBAR") == 0x0C);
    std.debug.assert(@offsetOf(Regs, "RLAR") == 0x10);
    std.debug.assert(@offsetOf(Regs, "MAIR0") == 0x30);
    std.debug.assert(@offsetOf(Regs, "MAIR1") == 0x34);
    std.debug.assert(@sizeOf(Regs) == 0x38);
}

/// Architectural MPU base (`k_ra8_mpu_core_base_addr`).
pub const core_base_addr: usize = 0xE000ED90;
/// SCB->SHCSR (Armv8-M ARM B3.2.10).
pub const shcsr_addr: usize = 0xE000ED24;
/// SHCSR.MEMFAULTENA, bit 16.
pub const shcsr_memfaultena: u32 = 1 << 16;

// A `zig build test` binary has neither an MPU nor the host suite's fake
// mapping at 0xE000ED90, so the test build addresses a module-local block
// instead. The declarations exist only in a test build: the shipped archive
// keeps the literal MMIO addresses and writes no `.data` / `.bss`, which is
// what keeps `ra8_mpu_apply_boot_map()` callable from `SystemInit()` before
// the `.data` copy has run.
const fake = if (builtin.is_test) struct {
    var regs_block: Regs = std.mem.zeroes(Regs);
    var shcsr_word: u32 = 0;
} else struct {};

inline fn regs() *volatile Regs {
    if (builtin.is_test) return &fake.regs_block;
    return @ptrFromInt(core_base_addr);
}

inline fn shcsr() *volatile u32 {
    if (builtin.is_test) return &fake.shcsr_word;
    return @ptrFromInt(shcsr_addr);
}

/// Test-only view of the register block the exported symbols drive.
pub fn testRegs() *volatile Regs {
    comptime std.debug.assert(builtin.is_test);
    return regs();
}

/// Test-only view of the SHCSR word the exported symbols drive.
pub fn testShcsr() *volatile u32 {
    comptime std.debug.assert(builtin.is_test);
    return shcsr();
}

fn dregionCount() u8 {
    return implementation.dregionOf(regs().TYPE);
}

fn programRegion(region: u8, r: *const Region) void {
    const mpu = regs();
    mpu.RNR = region;
    mpu.RBAR = implementation.buildRbar(r);
    mpu.RLAR = implementation.buildRlar(r);
}

fn clearRegion(region: u8) void {
    const mpu = regs();
    mpu.RNR = region;
    mpu.RLAR = 0;
}

fn writeCtrl(ctrl: u32) void {
    regs().CTRL = ctrl;
}

fn writeMair(mair0: u32, mair1: u32) void {
    const mpu = regs();
    mpu.MAIR0 = mair0;
    mpu.MAIR1 = mair1;
}

/// Validate the whole configuration before any register write, in the C's
/// order: capacity, then the region table pointer, then each descriptor.
fn validateCfg(cfg: *const Config) MpuError {
    if (cfg.region_count > dregionCount()) return .invalid_arg;
    if (cfg.region_count > 0 and cfg.regions == null) return .null_ptr;
    const regions = cfg.regions orelse return .ok;
    var i: u8 = 0;
    while (i < cfg.region_count) : (i += 1) {
        if (implementation.checkRegion(&regions[i]) != .ok) return .invalid_arg;
    }
    return .ok;
}

/// Program every MPU region from a static configuration.
///
/// Mirrors `ra8_mpu_configure`: validate everything first so a rejected
/// configuration changes no MPU state, then disable, write MAIR, install the
/// table, clear the unused tail, re-enable, and finally enable MemManage
/// delivery through SHCSR.MEMFAULTENA.
pub export fn ra8_mpu_configure(cfg: ?*const Config) callconv(.c) u16 {
    const config = cfg orelse {
        ra8_log_emit_error(tag, "cfg must not be nullptr");
        return @intFromEnum(MpuError.null_ptr);
    };

    const verdict = validateCfg(config);
    if (verdict != .ok) return @intFromEnum(verdict);

    // Arm Cortex-M85 TRM "MPU_CTRL": disable before reprogramming.
    writeCtrl(0);
    writeMair(config.mair0, config.mair1);

    if (config.regions) |regions| {
        var i: u8 = 0;
        while (i < config.region_count) : (i += 1) {
            programRegion(i, &regions[i]);
        }
    }
    const implemented = dregionCount();
    var i: u8 = config.region_count;
    while (i < implemented) : (i += 1) {
        clearRegion(i);
    }
    writeCtrl(implementation.buildCtrl(config));

    // Without SHCSR.MEMFAULTENA every MPU permission violation escalates to
    // HardFault and the strong MemManage_Handler the application installed
    // never runs. Callers that genuinely want the escalation clear the bit
    // again after this call.
    shcsr().* |= shcsr_memfaultena;
    return @intFromEnum(MpuError.ok);
}

/// Set MPU_CTRL.ENABLE.
pub export fn ra8_mpu_enable() callconv(.c) u16 {
    const mpu = regs();
    mpu.CTRL = mpu.CTRL | implementation.ctrl_enable;
    return @intFromEnum(MpuError.ok);
}

/// Clear MPU_CTRL.ENABLE.
pub export fn ra8_mpu_disable() callconv(.c) u16 {
    const mpu = regs();
    mpu.CTRL = mpu.CTRL & ~implementation.ctrl_enable;
    return @intFromEnum(MpuError.ok);
}

/// Program a single region without disabling the MPU.
///
/// Guard order is the contract: the null descriptor is reported before the
/// region index is range-checked, and the index before the descriptor's own
/// rules, so each rejection keeps the code the C suite expects.
pub export fn ra8_mpu_set_region(region: u8, region_cfg: ?*const Region) callconv(.c) u16 {
    const descriptor = region_cfg orelse {
        ra8_log_emit_error(tag, "region_cfg must not be nullptr");
        return @intFromEnum(MpuError.null_ptr);
    };
    if (region >= dregionCount()) return @intFromEnum(MpuError.invalid_arg);
    if (implementation.checkRegion(descriptor) != .ok) return @intFromEnum(MpuError.invalid_arg);
    programRegion(region, descriptor);
    return @intFromEnum(MpuError.ok);
}

/// Return the canonical boot memory-attribute map region table.
pub export fn ra8_mpu_boot_map(out_count: ?*u8) callconv(.c) ?[*]const Region {
    const count = out_count orelse return null;
    count.* = implementation.boot_region_count;
    return &implementation.boot_regions;
}

/// Install the canonical 5-region boot map and enable the MPU.
///
/// Reads only the `.rodata` region table and MMIO, writes no `.data` /
/// `.bss`, and never logs, so the reset path can call it before the `.data`
/// copy. The stale high-index regions are cleared first so an aborted
/// reprogram never leaves an enabled region beside the new map.
pub export fn ra8_mpu_apply_boot_map() callconv(.c) u16 {
    const implemented = dregionCount();
    if (implemented < implementation.boot_region_count) {
        return @intFromEnum(MpuError.invalid_arg);
    }

    writeCtrl(0);
    writeMair(implementation.boot_mair0, implementation.boot_mair1);

    var i: u8 = implementation.boot_region_count;
    while (i < implemented) : (i += 1) {
        clearRegion(i);
    }
    var j: u8 = 0;
    while (j < implementation.boot_region_count) : (j += 1) {
        programRegion(j, &implementation.boot_regions[j]);
    }

    // Retire the region writes, enable with PRIVDEFENA, then flush the
    // pipeline so the very next access sees the new attribute map.
    dsb();
    writeCtrl(implementation.ctrl_enable | implementation.ctrl_privdefena);
    dsb();
    isb();
    return @intFromEnum(MpuError.ok);
}

/// Report whether MPU_CTRL.ENABLE is set.
pub export fn ra8_mpu_is_enabled() callconv(.c) bool {
    return (regs().CTRL & implementation.ctrl_enable) != 0;
}
