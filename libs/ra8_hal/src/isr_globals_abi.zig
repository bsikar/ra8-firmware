//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_isr_globals_enable/disable (RA8FW-706), moved out of
//! ra8_isr.c. Prototypes stay in inc/ra8_isr.h.

const builtin = @import("builtin");
const gate = @import("internal/isr_globals.zig");

const hosted = builtin.os.tag != .freestanding;

/// Host seam from ra8_hw_intrinsics.h; on target those are static inline.
extern fn ra8_hw_irq_enable() void;
extern fn ra8_hw_irq_disable() void;

const Hw = struct {
    pub fn irqEnable() void {
        if (hosted) ra8_hw_irq_enable() else asm volatile ("cpsie i" ::: "memory");
    }
    pub fn irqDisable() void {
        if (hosted) ra8_hw_irq_disable() else asm volatile ("cpsid i" ::: "memory");
    }
};

export fn ra8_isr_globals_enable() void {
    gate.enable(Hw);
}

export fn ra8_isr_globals_disable() void {
    gate.disable(Hw);
}
