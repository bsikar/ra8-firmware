//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_rpc_cpu1, M85 half (RA8FW-544). Brings up the clock and the SCI8
//! VCOM console, clears the shared block, releases CPU1, then waits for
//! CPU1 to have taken `shared.pass_reports` sums from the module, every
//! one of them the sum expected, and prints the HIL line with the first
//! four. A failed manager step, a failure the module reports or one wrong
//! sum fails at once. The board's C layer is reached through extern; this
//! file is the whole M85 application.

const std = @import("std");
const shared = @import("shared.zig");

pub const baud: u32 = 115_200;
/// Polls of the shared block before giving up: about two seconds at 1 GHz.
pub const spin_limit: u32 = 400_000_000;
pub const pass_line = std.fmt.comptimePrint(
    "txm_rpc_cpu1: add returned {d} {d} {d} {d} PASS\r\n",
    .{
        shared.first_values[0], shared.first_values[1],
        shared.first_values[2], shared.first_values[3],
    },
);
pub const fail_line = "txm_rpc_cpu1: FAIL\r\n";

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

/// True once the first values are in the block and are the expected ones.
fn firstValuesRight(block: *volatile shared.Block) bool {
    for (shared.first_values, 0..) |expected, index| {
        if (block.values[index] != expected) return false;
    }
    return true;
}

fn waitReports(block: *volatile shared.Block) bool {
    var polls: u32 = 0;
    while (polls < spin_limit) : (polls += 1) {
        if (block.signature != shared.signature) continue;
        if (block.failed_step != shared.Step.none) return false;
        if (block.mismatches != 0) return false;
        if (block.reports >= shared.pass_reports) return firstValuesRight(block);
    }
    return false;
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn main() callconv(.c) c_int {
    const block = shared.block();
    block.* = .{
        .signature = 0,
        .failed_step = shared.Step.none,
        .result = 0,
        .reports = 0,
        .mismatches = 0,
        .values = @splat(0),
        .answered = 0,
    };
    asm volatile ("dsb" ::: .{ .memory = true });
    if (ra8_cgc_init() != ok) park();
    const console = ra8_board_uart_console_init(baud) == ok;
    if (ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top) != ok) {
        if (console) say(fail_line);
        park();
    }
    if (console) say(if (waitReports(block)) pass_line else fail_line);
    park();
}
