// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
//! threadx_stkof: ThreadX on CPU0 with one thread that overflows its stack on
//! purpose (RA8FW-503).
//!
//! The ThreadX Cortex-M85 port loads each thread's stack start into PSPLIM on
//! every switch, so a push below it raises a UsageFault with UFSR.STKOF. The
//! project's tx_initialize_low_level.S leaves UsageFault to the board's weak
//! trampoline, which reports every fault the same way. This image overrides
//! that trampoline with the port's own STKOF path: clear the flag, drop any
//! lazy FP state, and hand the current thread to
//! `_tx_thread_stack_error_handler`, which calls the callback registered with
//! `tx_thread_stack_error_notify`. Every other UsageFault still goes to the
//! board reporter.
//!
//! Console (SCI8, 115200): `stkof: start`, then on a caught overflow
//! `stkof: thread=overflow caught` and `stkof: PASS`. If the recursion ever
//! returns without a fault the thread prints `stkof: FAIL`. Either way the
//! image parks once it has printed its verdict.

const std = @import("std");

pub const panic = std.debug.no_panic;

const console_baud: u32 = 115200;

/// sizeof(TX_THREAD) for ports/cortex_m85/gnu with port/threadx/inc/tx_user.h,
/// measured with arm-none-eabi-gcc (176 bytes, 4-byte aligned). Storage is
/// handed to `_tx_thread_create`, which skips the size check `_txe_` makes.
const tx_thread_bytes = 176;
const tx_no_time_slice: u32 = 0;
const tx_auto_start: u32 = 1;
const tx_success: u32 = 0;

const overflow_stack_bytes = 1024;
const overflow_priority: u32 = 10;
/// Deep enough to pass the 1 KiB stack several times over: each frame holds
/// at least the 64-byte buffer below plus the saved registers.
const overflow_depth: u32 = 64;

const Entry = *const fn (u32) callconv(.c) void;
const StackErrorHandler = *const fn (?*anyopaque) callconv(.c) void;

extern fn _tx_initialize_kernel_enter() void;
extern fn _tx_thread_create(
    thread: *anyopaque,
    name: [*:0]const u8,
    entry: Entry,
    input: u32,
    stack: *anyopaque,
    stack_size: u32,
    priority: u32,
    preempt_threshold: u32,
    time_slice: u32,
    auto_start: u32,
) u32;
extern fn _tx_thread_stack_error_notify(handler: ?StackErrorHandler) u32;
extern fn ra8_cgc_init() u16;
extern fn ra8_board_uart_console_init(baud: u32) u32;
extern fn ra8_board_uart_console_write(data: ?[*]const u8, len: usize) u32;

var overflow_thread: [tx_thread_bytes]u8 align(8) = undefined;
var overflow_stack: [overflow_stack_bytes]u8 align(8) = undefined;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

/// One frame of the recursion. The volatile buffer keeps every frame on the
/// stack, and the add after the call keeps the call out of tail position.
fn descend(depth: u32) u32 {
    var frame: [16]u32 = undefined;
    const live: *volatile [16]u32 = &frame;
    live[0] = depth;
    if (depth == 0) return live[0];
    return @call(.never_inline, descend, .{depth - 1}) +% live[0];
}

fn overflowEntry(input: u32) callconv(.c) void {
    _ = descend(overflow_depth + input);
    say("stkof: FAIL (recursion returned without a stack fault)\r\n");
    park();
}

/// Runs in UsageFault context from `_tx_thread_stack_error_handler`.
fn onStackError(thread: ?*anyopaque) callconv(.c) void {
    if (thread == @as(?*anyopaque, @ptrCast(&overflow_thread))) {
        say("stkof: thread=overflow caught\r\nstkof: PASS\r\n");
    } else {
        say("stkof: FAIL (stack error on an unexpected thread)\r\n");
    }
    park();
}

export fn tx_application_define(first_unused_memory: ?*anyopaque) void {
    _ = first_unused_memory;
    if (_tx_thread_stack_error_notify(onStackError) != tx_success) {
        say("stkof: FAIL (stack error notify rejected)\r\n");
        park();
    }
    const created = _tx_thread_create(
        &overflow_thread,
        "overflow",
        overflowEntry,
        0,
        &overflow_stack,
        overflow_stack_bytes,
        overflow_priority,
        overflow_priority,
        tx_no_time_slice,
        tx_auto_start,
    );
    if (created != tx_success) {
        say("stkof: FAIL (thread create rejected)\r\n");
        park();
    }
}

export fn main() void {
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(console_baud);
    say("stkof: start\r\n");
    _tx_initialize_kernel_enter();
    say("stkof: FAIL (kernel enter returned)\r\n");
    park();
}

/// The port's STKOF path (ports/cortex_m85/gnu tx_initialize_low_level.S),
/// with the board trampoline's reporter call for every other UsageFault.
export fn UsageFault_Handler() callconv(.naked) void {
    asm volatile (
        \\ movw  r0, #0xED28
        \\ movt  r0, #0xE000
        \\ ldr   r1, [r0]
        \\ tst   r1, #0x100000
        \\ beq   1f
        \\ str   r1, [r0]
        \\ movw  r0, #0xEF34
        \\ movt  r0, #0xE000
        \\ ldr   r1, [r0]
        \\ bic   r1, r1, #1
        \\ str   r1, [r0]
        \\ movw  r0, :lower16:_tx_thread_current_ptr
        \\ movt  r0, :upper16:_tx_thread_current_ptr
        \\ ldr   r0, [r0]
        \\ push  {r0, lr}
        \\ bl    _tx_thread_stack_error_handler
        \\ pop   {r0, lr}
        \\ movs  r1, #0
        \\ movw  r0, :lower16:_tx_thread_current_ptr
        \\ movt  r0, :upper16:_tx_thread_current_ptr
        \\ str   r1, [r0]
        \\ movw  r0, #0xED04
        \\ movt  r0, #0xE000
        \\ mov   r1, #0x10000000
        \\ str   r1, [r0]
        \\ dsb
        \\ cpsie i
        \\ bx    lr
        \\1:
        \\ tst   lr, #4
        \\ ite   eq
        \\ mrseq r0, msp
        \\ mrsne r0, psp
        \\ movs  r1, #6
        \\ b     ra8_exception_report
    );
}
