//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The millisecond counter the SysTick ISR advances, and nothing else.
//!
//! The ISR writes and everything else reads, which is what the C spelled
//! `volatile`. Here each access is one atomic word with monotonic ordering:
//! on Cortex-M that is the same single `ldr` / `str` the volatile produced,
//! and it is what keeps a delay loop from hoisting the load out of the loop.
//! Ordering can stay monotonic because the counter carries no other state
//! with it, and SysTick cannot pre-empt itself, so the read-modify-write in
//! `advance` needs no exclusive pair.

const std = @import("std");

var tick_ms = std.atomic.Value(u32).init(0);
var cycles_per_ms = std.atomic.Value(u32).init(0);

/// One tick. Wraps at 2^32, every ~49.7 days, and callers compare by
/// subtraction so the comparison survives the wrap.
pub fn advance() void {
    tick_ms.store(tick_ms.load(.monotonic) +% 1, .monotonic);
}

/// Milliseconds since the counter was last reset.
pub fn now() u32 {
    return tick_ms.load(.monotonic);
}

/// Start the count again from zero, as arming SysTick does.
pub fn reset() void {
    tick_ms.store(0, .monotonic);
}

/// Stamp the cycles-per-millisecond the cycle-counter delay converts with.
pub fn setCyclesPerMs(cycles: u32) void {
    cycles_per_ms.store(cycles, .monotonic);
}

pub fn cyclesPerMs() u32 {
    return cycles_per_ms.load(.monotonic);
}
