//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_manager_cpu1, M85 half (RA8FW-431). Brings up the clock and the SCI8
//! VCOM console, clears the shared block, releases CPU1, then waits for
//! CPU1's Module Manager to report that the hello-world module's start thread
//! has run `shared.pass_runs` times and prints the HIL line. A failed manager
//! step fails at once. The board's C layer is reached through extern; this
//! file is the whole M85 application.

const std = @import("std");
const shared = @import("shared.zig");

pub const baud: u32 = 115_200;
/// Polls of the shared block before giving up: about two seconds at 1 GHz.
pub const spin_limit: u32 = 400_000_000;
pub const pass_line = std.fmt.comptimePrint("txm_manager_cpu1: module ran {d} times PASS\r\n", .{shared.pass_runs});
pub const fail_line = "txm_manager_cpu1: FAIL\r\n";

const ok: c_int = 0;
extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_cpu1_release(mram: *const anyopaque, stack: *const anyopaque) c_int;
extern const g_ra8_ls_cpu1_mram_start: u8;
extern const g_ra8_ls_cpu1_stack_top: u8;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn waitRuns(block: *volatile shared.Block) bool {
    var polls: u32 = 0;
    while (polls < spin_limit) : (polls += 1) {
        if (block.signature != shared.signature) continue;
        if (block.failed_step != shared.Step.none) return false;
        if (block.module_runs >= shared.pass_runs) return true;
    }
    return false;
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn main() callconv(.c) c_int {
    const block = shared.block();
    block.* = .{ .signature = 0, .failed_step = shared.Step.none, .result = 0, .module_runs = 0 };
    asm volatile ("dsb" ::: "memory");
    if (ra8_cgc_init() != ok) park();
    const console = ra8_board_uart_console_init(baud) == ok;
    if (ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top) != ok) {
        if (console) say(fail_line);
        park();
    }
    if (console) say(if (waitRuns(block)) pass_line else fail_line);
    park();
}
