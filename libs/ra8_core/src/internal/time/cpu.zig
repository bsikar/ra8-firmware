//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three core instructions the delay policy needs, and nothing else.
//!
//! A host test binary has no PRIMASK, no `wfi` and nothing to idle for, so
//! each one degrades to the answer the C reached behind `RA8_OFF_TARGET`:
//! interrupts read as live and the waits do nothing.

const builtin = @import("builtin");

/// True in a freestanding image, false in a host test binary. The C spelled
/// this `RA8_OFF_TARGET`, inverted.
pub const on_target = builtin.target.os.tag == .freestanding;

/// Whether PRIMASK is masking every configurable interrupt right now. With it
/// set the SysTick IRQ cannot dispatch, so nothing advances the tick counter.
pub fn interruptsMasked() bool {
    if (!on_target) return false;
    switch (builtin.cpu.arch) {
        .thumb => {
            const primask = asm volatile ("mrs %[out], primask"
                : [out] "=r" (-> u32),
            );
            return (primask & 1) != 0;
        },
        .riscv32, .riscv64 => {
            const mstatus = asm volatile ("csrr %[out], mstatus"
                : [out] "=r" (-> u32),
            );
            // mstatus.MIE is bit 3 on both RV32 and RV64.
            return (mstatus & 8) != 0;
        },
        else => @compileError("interruptsMasked: no implementation for this architecture"),
    }
}

/// Idle until the next interrupt arrives.
pub fn waitForInterrupt() void {
    if (!on_target) return;
    asm volatile ("wfi");
}

/// One cycle of nothing, so a cycle-counter spin has a body to spin on.
pub fn spin() void {
    if (!on_target) return;
    asm volatile ("nop");
}
