//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the millisecond timebase units (#2851).

const std = @import("std");
const reload_math = @import("time_reload");
const tick = @import("time_tick");
const delay = @import("time_delay");
const hooks = @import("time_hooks");

// ---- reload arithmetic --------------------------------------------------

test "a 1 GHz clock reloads one tick short of a million cycles" {
    try std.testing.expectEqual(@as(u32, 999_999), try reload_math.reloadFor(1_000_000_000));
    try std.testing.expectEqual(@as(u32, 999), try reload_math.reloadFor(1_000_000));
}

test "the lowest usable clock is two tick periods" {
    try std.testing.expectEqual(@as(u32, 1), try reload_math.reloadFor(reload_math.limits.min_cpu_hz));
    try std.testing.expectError(error.ClockTooLow, reload_math.reloadFor(reload_math.limits.min_cpu_hz - 1));
}

test "a zero clock is named apart from a clock that is merely too slow" {
    try std.testing.expectError(error.ZeroClock, reload_math.reloadFor(0));
    try std.testing.expectError(error.ClockTooLow, reload_math.reloadFor(1000));
    try std.testing.expectError(error.ClockTooLow, reload_math.reloadFor(1999));
}

test "a clock below one tick period is refused, not wrapped" {
    // The C divided to zero here and subtracted one, handing the caller
    // 0xFFFFFFFF with an ok status.
    try std.testing.expectError(error.ClockTooLow, reload_math.reloadFor(1));
    try std.testing.expectError(error.ClockTooLow, reload_math.reloadFor(999));
}

test "cycles per millisecond is the clock over the tick rate" {
    try std.testing.expectEqual(@as(u32, 1_000_000), reload_math.cyclesPerMs(1_000_000_000));
    try std.testing.expectEqual(@as(u32, 250_000), reload_math.cyclesPerMs(250_000_000));
}

// ---- the tick counter ---------------------------------------------------

test "the counter advances one millisecond per tick and resets to zero" {
    tick.reset();
    try std.testing.expectEqual(@as(u32, 0), tick.now());
    tick.advance();
    tick.advance();
    tick.advance();
    try std.testing.expectEqual(@as(u32, 3), tick.now());
    tick.reset();
    try std.testing.expectEqual(@as(u32, 0), tick.now());
}

test "the cycles-per-millisecond stamp reads back" {
    tick.setCyclesPerMs(250_000);
    try std.testing.expectEqual(@as(u32, 250_000), tick.cyclesPerMs());
}

// ---- the cycle-counter wait --------------------------------------------

var fake_cycles: u32 = 0;

fn readFake() callconv(.c) u32 {
    const value = fake_cycles;
    fake_cycles +%= 1;
    return value;
}

test "a cycle wait ends once the counter has moved on by the target" {
    fake_cycles = 0;
    delay.byCycles(&readFake, 10);
    // One read stamps the start, then one per comparison up to and including
    // the read whose difference reaches 10: eleven in all.
    try std.testing.expectEqual(@as(u32, 11), fake_cycles);
}

test "a cycle wait that spans the counter's wrap still ends" {
    // DWT_CYCCNT wraps every 2^32 cycles, a few seconds at 1 GHz, so a wait
    // straddling the wrap is ordinary. Subtraction is what keeps it ending on
    // the eighth cycle rather than immediately.
    fake_cycles = 0xFFFF_FFFB;
    delay.byCycles(&readFake, 8);
    try std.testing.expectEqual(@as(u32, 4), fake_cycles);
}

test "a zero-cycle wait ends at its first comparison" {
    fake_cycles = 7;
    delay.byCycles(&readFake, 0);
    try std.testing.expectEqual(@as(u32, 9), fake_cycles);
}

// ---- the subsystem callouts --------------------------------------------

var threadx_ticks: u32 = 0;
var usb_reenables: u32 = 0;

fn countThreadx() callconv(.c) void {
    threadx_ticks += 1;
}

fn countUsb() callconv(.c) void {
    usb_reenables += 1;
}

fn resetCounts() void {
    threadx_ticks = 0;
    usb_reenables = 0;
}

test "an unlinked kernel is not ticked" {
    resetCounts();
    hooks.dispatch(null, null, &countUsb);
    try std.testing.expectEqual(@as(u32, 0), threadx_ticks);
    try std.testing.expectEqual(@as(u32, 1), usb_reenables);
}

test "a linked kernel that has not finished init is not ticked" {
    resetCounts();
    const not_ready: u32 = 0;
    hooks.dispatch(&not_ready, &countThreadx, &countUsb);
    try std.testing.expectEqual(@as(u32, 0), threadx_ticks);
    try std.testing.expectEqual(@as(u32, 1), usb_reenables);
}

test "a ready kernel takes the tick" {
    resetCounts();
    const ready: u32 = 1;
    hooks.dispatch(&ready, &countThreadx, &countUsb);
    try std.testing.expectEqual(@as(u32, 1), threadx_ticks);
    try std.testing.expectEqual(@as(u32, 1), usb_reenables);
}

test "a ready flag with no kernel timer linked is skipped" {
    resetCounts();
    const ready: u32 = 1;
    hooks.dispatch(&ready, null, null);
    try std.testing.expectEqual(@as(u32, 0), threadx_ticks);
    try std.testing.expectEqual(@as(u32, 0), usb_reenables);
}
