//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_sd_hello_m85 (RA8FW-829 and RA8FW-830, under RA8FW-290): CPU0 reads
//! the signed hello-world module `txm_hello_m33.ra8app` off the micro-SD card
//! (card.zig), admits it against the pinned key (verify.zig), and runs it
//! unprivileged through the ThreadX Module Manager (module.zig): memory load,
//! start, ten runs of its start thread, stop, unload. The verdict goes to the
//! SCI8 VCOM console. Zig throughout; ra8_fs is the existing C library.

const std = @import("std");
const card = @import("card.zig");
const verify = @import("verify.zig");
const module = @import("module.zig");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
pub const pass_line = "txm_sd_hello_m85: signed module loaded, ran, exited PASS\r\n";
pub const stack_bytes = 4096;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;

var image: [card.file_max]u8 align(8) = undefined;
/// The admitted file's length, or the step that refused it.
var loaded: anyerror!usize = error.unread;

var thread: [256]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;

extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn _tx_initialize_kernel_enter() void;
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

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn fail(err: anyerror) void {
    var line: [64]u8 = undefined;
    const text = std.fmt.bufPrint(&line, "txm_sd_hello_m85: FAIL {s}\r\n", .{@errorName(err)});
    say(text catch "txm_sd_hello_m85: FAIL\r\n");
}

/// Reads and admits the module before the kernel starts.
fn admit() anyerror!usize {
    const len = try card.readApp(&image);
    if (!verify.admitted(image[0..len])) return error.signature;
    return len;
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    if (loaded) |len| {
        if (module.run(image[verify.header_bytes..len])) say(pass_line) else |err| fail(err);
    } else |err| fail(err);
    while (true) _ = _tx_thread_sleep(module.tick_limit);
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) say("txm_sd_hello_m85: FAIL thread\r\n");
}

export fn main() callconv(.c) c_int {
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(baud);
    loaded = admit();
    _tx_initialize_kernel_enter();
    say("txm_sd_hello_m85: FAIL kernel\r\n");
    park();
}
