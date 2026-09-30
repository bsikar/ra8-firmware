//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which clock a delay waits on, and nothing else.
//!
//! PRIMASK decides. With interrupts masked the SysTick IRQ cannot dispatch,
//! the tick counter never advances, and a `wfi` loop waiting on it hangs for
//! good; DWT_CYCCNT keeps counting every CPU cycle regardless, so that is the
//! fallback. With interrupts live the tick path is cheaper and the core sleeps
//! between checks instead of spinning.

const cpu = @import("time_cpu");
const tick = @import("time_tick");

/// A cycle-counter read: DWT_CYCCNT on target, a fake in a test.
pub const Cycles = *const fn () callconv(.c) u32;

/// Spin until the counter has moved `cycles` on. Subtraction, so one wrap of
/// the counter mid-wait does not end the wait early.
pub fn byCycles(read: Cycles, cycles: u32) void {
    const start = read();
    while (read() -% start < cycles) cpu.spin();
}

/// Sleep until the tick counter has moved `ms` on.
pub fn byTick(ms: u32) void {
    const start = tick.now();
    while (tick.now() -% start < ms) cpu.waitForInterrupt();
}

/// Wait `ms` milliseconds on whichever clock can actually advance.
pub fn wait(read: Cycles, ms: u32) void {
    // Off target neither clock runs: there is no SysTick to tick and no DWT
    // to count, so waiting on either would never return. The C returned
    // immediately here too.
    if (!cpu.on_target) return;

    if (cpu.interruptsMasked()) {
        byCycles(read, ms * tick.cyclesPerMs());
    } else {
        byTick(ms);
    }
}
