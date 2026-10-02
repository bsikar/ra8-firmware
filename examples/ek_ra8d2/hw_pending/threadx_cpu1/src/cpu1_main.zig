//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! threadx_cpu1, CPU1 half (RA8FW-404). The `threadx_cpu1` glue owns the
//! vector table and reset path and enters the kernel; this file is what the
//! kernel calls back: `tx_application_define` retunes SysTick for CPU1's clock
//! and creates one thread that stamps the shared block and copies the kernel
//! tick count into it once per tick.

const glue = @import("threadx_cpu1");
const shared = @import("shared.zig");

/// CPU1's clock (the RA8D2's M33 maximum). A slower real clock only makes the
/// ticks slower, never wrong in count.
pub const cpu1_hz: u32 = 250_000_000;
/// sizeof(TX_THREAD) is 176 on this port (measured with the M33 flags); this
/// leaves headroom without a C import.
pub const thread_bytes = 256;
pub const stack_bytes = 1024;
pub const priority: u32 = 1;

const no_time_slice: u32 = 0;
const auto_start: u32 = 1;

var thread: [thread_bytes]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;

extern fn _tx_thread_create(
    thread_ptr: *anyopaque,
    name: [*:0]const u8,
    entry: *const fn (u32) callconv(.c) void,
    input: u32,
    stack_start: *anyopaque,
    stack_size: u32,
    prio: u32,
    preempt_threshold: u32,
    time_slice: u32,
    start: u32,
) callconv(.c) u32;
extern fn _tx_thread_sleep(ticks: u32) callconv(.c) u32;
extern fn _tx_time_get() callconv(.c) u32;

fn ticker(input: u32) callconv(.c) void {
    _ = input;
    const block = shared.block();
    block.signature = shared.signature;
    while (true) {
        block.ticks = _tx_time_get();
        _ = _tx_thread_sleep(1);
    }
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    glue.startTicks(cpu1_hz);
    _ = _tx_thread_create(&thread, "ticker", &ticker, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
}
