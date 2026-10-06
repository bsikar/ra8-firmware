// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
//! Two ThreadX workers keep distinct S0-S31 values live across PendSV so the
//! emulator corpus exercises eager software and lazy architectural FP frames.

const std = @import("std");

pub const panic = std.debug.no_panic;

const console_baud: u32 = 115200;
const tx_thread_bytes = 176;
const thread_stack_bytes = 4096;
const thread_priority: u32 = 4;
const tx_no_time_slice: u32 = 0;
const tx_auto_start: u32 = 1;
const tx_success: u32 = 0;
const rounds: u32 = 8;
const register_count: u32 = 32;
const completion_all: u32 = 3;
const completion_waits: u32 = 8;

const fpccr_address: usize = 0xE000EF34;
const fpccr_aspen: u32 = 1 << 31;
const fpccr_lspen: u32 = 1 << 30;
const control_fpca: u32 = 1 << 2;

const seed_a: u32 = 0x3F000000;
const seed_b: u32 = 0x41000000;
const round_stride: u32 = 1 << 12;

const Entry = *const fn (u32) callconv(.c) void;

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
// Load S0-S31, record CONTROL, relinquish, then store S0-S31. One assembly
// routine so the compiler cannot spill caller-saved S0-S15 around the call;
// S16-S31 are callee-saved, so the routine keeps the caller's copy on its stack.
comptime {
    asm (
        \\.fpu fpv5-d16
        \\.section .text.fpctx_switch,"ax",%progbits
        \\.balign 4
        \\.global fpctx_switch
        \\.type fpctx_switch,%function
        \\.thumb_func
        \\fpctx_switch:
        \\    push {r4-r6, lr}
        \\    mov r4, r0
        \\    mov r5, r1
        \\    mov r6, r2
        \\    vstmdb sp!, {s16-s31}
        \\    vldmia r4, {s0-s31}
        \\    mrs r3, control
        \\    str r3, [r6]
        \\    bl _tx_thread_relinquish
        \\    vstmia r5, {s0-s31}
        \\    vldmia sp!, {s16-s31}
        \\    pop {r4-r6, pc}
        \\.size fpctx_switch, .-fpctx_switch
        \\.text
    );
}

extern fn fpctx_switch(
    expected: *const [register_count]u32,
    observed: *[register_count]u32,
    control: *u32,
) callconv(.c) void;
extern fn _tx_thread_relinquish() u32;
extern fn ra8_cgc_init() u16;
extern fn ra8_board_uart_console_init(baud: u32) u32;
extern fn ra8_board_uart_console_write(data: ?[*]const u8, len: usize) u32;

var thread_a: [tx_thread_bytes]u8 align(8) = undefined;
var thread_b: [tx_thread_bytes]u8 align(8) = undefined;
var stack_a: [thread_stack_bytes]u8 align(8) = undefined;
var stack_b: [thread_stack_bytes]u8 align(8) = undefined;
var completion_mask: u32 = 0;
var final_printed: u32 = 0;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

fn volatileLoad(word: *u32) u32 {
    const ptr: *volatile u32 = @ptrCast(word);
    return ptr.*;
}

fn volatileStore(word: *u32, value: u32) void {
    const ptr: *volatile u32 = @ptrCast(word);
    ptr.* = value;
}

fn mismatch(thread: u8, round: u32, reg: u32) noreturn {
    var buffer: [64]u8 = undefined;
    const line = std.fmt.bufPrint(
        &buffer,
        "fpctx: thread={c} FAIL round={d} reg={d}\r\n",
        .{ thread, round, reg },
    ) catch "fpctx: FAIL format\r\n";
    say(line);
    park();
}

fn peerTimeout(thread: u8) noreturn {
    if (thread == 'A') {
        say("fpctx: thread=A FAIL peer timeout\r\n");
    } else {
        say("fpctx: thread=B FAIL peer timeout\r\n");
    }
    park();
}

fn worker(thread: u8, seed: u32, completion_bit: u32) noreturn {
    var expected: [register_count]u32 align(8) = undefined;
    var observed: [register_count]u32 align(8) = undefined;
    var round: u32 = 0;
    while (round < rounds) : (round += 1) {
        var reg: u32 = 0;
        while (reg < register_count) : (reg += 1) {
            expected[reg] = seed + round * round_stride + reg;
            observed[reg] = 0;
        }

        var control: u32 = 0;
        fpctx_switch(&expected, &observed, &control);
        if (control & control_fpca == 0) mismatch(thread, round, 0);

        reg = 0;
        while (reg < register_count) : (reg += 1) {
            if (observed[reg] != expected[reg]) mismatch(thread, round, reg);
        }
    }

    if (thread == 'A') {
        say("fpctx: thread=A PASS\r\n");
    } else {
        say("fpctx: thread=B PASS\r\n");
    }
    volatileStore(&completion_mask, volatileLoad(&completion_mask) | completion_bit);

    var wait_count: u32 = 0;
    while (volatileLoad(&completion_mask) != completion_all and wait_count < completion_waits) : (wait_count += 1) {
        _ = _tx_thread_relinquish();
    }
    if (volatileLoad(&completion_mask) != completion_all) peerTimeout(thread);

    if (volatileLoad(&final_printed) == 0) {
        volatileStore(&final_printed, 1);
        say("fpctx: PASS\r\n");
        park();
    }
    park();
}

fn threadA(_: u32) callconv(.c) void {
    worker('A', seed_a, 1);
}

fn threadB(_: u32) callconv(.c) void {
    worker('B', seed_b, 2);
}

export fn tx_application_define(first_unused_memory: ?*anyopaque) void {
    _ = first_unused_memory;
    if (_tx_thread_create(
        &thread_a,
        "fpctx_a",
        &threadA,
        0,
        &stack_a,
        thread_stack_bytes,
        thread_priority,
        thread_priority,
        tx_no_time_slice,
        tx_auto_start,
    ) != tx_success) {
        say("fpctx: FAIL create A\r\n");
        park();
    }
    if (_tx_thread_create(
        &thread_b,
        "fpctx_b",
        &threadB,
        0,
        &stack_b,
        thread_stack_bytes,
        thread_priority,
        thread_priority,
        tx_no_time_slice,
        tx_auto_start,
    ) != tx_success) {
        say("fpctx: FAIL create B\r\n");
        park();
    }
}

export fn main() void {
    if (ra8_cgc_init() != 0) park();
    if (ra8_board_uart_console_init(console_baud) != 0) park();

    const fpccr: *volatile u32 = @ptrFromInt(fpccr_address);
    fpccr.* |= fpccr_aspen | fpccr_lspen;
    if (fpccr.* & (fpccr_aspen | fpccr_lspen) != fpccr_aspen | fpccr_lspen) {
        say("fpctx: FAIL fpccr\r\n");
        park();
    }

    say("fpctx: start\r\n");
    _tx_initialize_kernel_enter();
    say("fpctx: FAIL kernel returned\r\n");
    park();
}
