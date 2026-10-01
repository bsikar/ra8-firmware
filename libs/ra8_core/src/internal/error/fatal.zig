//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! How the fatal path stops the machine.
//!
//! Three steps, in this order and for this reason: mask interrupts so nothing
//! runs after the decision to stop, `bkpt #0` so an attached debugger halts
//! with the call stack intact, then park.
//!
//! The `bkpt` is deliberate even with no debugger attached: it faults to
//! HardFault, and by the time anything calls in here continuing is already
//! unsafe. That is the opposite trade from the fault reporter's halt
//! (`internal/fault/halt.zig`), which must NOT issue `bkpt`, because it runs
//! inside HardFault already and would escalate to LOCKUP. Two halts, two
//! situations, two files.
//!
//! The inline assembly is kept here rather than reached for through CMSIS so
//! a failure during CMSIS init cannot stop the trap from running.

const builtin = @import("builtin");

/// Whether this build is an image rather than a host test binary.
pub const on_target = builtin.target.os.tag == .freestanding;

/// Mask every maskable interrupt: PRIMASK.PM = 1.
pub fn maskInterrupts() void {
    if (comptime !on_target) return;
    asm volatile ("cpsid i" ::: "memory");
}

/// Halt under an attached debugger; faults to HardFault without one.
pub fn breakpoint() void {
    if (comptime !on_target) return;
    asm volatile ("bkpt #0");
}

/// Park forever. On the host there is nothing to park, so trap instead and
/// let the test runner fail loudly rather than spin out its timeout.
///
/// `noinline` so the loop keeps its own symbol in a backtrace.
pub noinline fn park() noreturn {
    if (comptime !on_target) @trap();
    while (true) {
        asm volatile ("wfi");
    }
}
