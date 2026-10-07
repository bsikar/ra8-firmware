//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Where a fault ends on target: a spin halt at a symbol the debugger can
//! name.
//!
//! The fault handler must NOT terminate by escalating to LOCKUP at
//! PC=0xEFFFFFFE, and it must not take the ordinary fatal path either, which
//! issues `bkpt #0`: on a board with no debugger attached that re-enters
//! HardFault and escalates anyway. Parking in a named `wfi` loop instead
//! gives a backtrace that says "we got here from the fault handler" rather
//! than pointing at a random unmapped address.

const builtin = @import("builtin");

/// Whether this build is an image rather than a host test binary.
pub const on_target = builtin.target.os.tag == .freestanding;

/// Mask interrupts and park. Never returns.
///
/// `noinline` so the loop keeps its own symbol in the backtrace instead of
/// being folded into the reporter.
pub noinline fn spin() noreturn {
    if (comptime !on_target) unreachable;
    asm volatile ("cpsid i" ::: .{ .memory = true });
    while (true) {
        asm volatile ("wfi");
    }
}
