//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! cpu1_pingpong_ra8p1, M85 half (RA8FW-496). Brings up the clock and the
//! console, clears the shared block, releases CPU1, then runs
//! `shared.rounds` ping-pong round trips and prints the verdict.

const std = @import("std");
const shared = @import("shared.zig");

pub const baud: u32 = 115_200;
/// Polls per round trip before giving up, the EK cpu1_pingpong budget.
pub const poll_budget: u32 = 2_000_000;
pub const pass_line = std.fmt.comptimePrint("cpu1_pingpong_ra8p1: {d} rounds PASS\r\n", .{shared.rounds});
pub const fail_line = "cpu1_pingpong_ra8p1: FAIL\r\n";

const ok: c_int = 0;
extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_cpu1_release(mram: *const anyopaque, stack: *const anyopaque) c_int;
extern const g_ra8_ls_cpu1_mram_start: u8;
extern const g_ra8_ls_cpu1_stack_top: u8;

/// Completed round trips, for a memory probe when there is no console.
export var g_cpu1_pingpong_ra8p1_match: u32 = 0;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn roundTrip(block: *volatile shared.Block, seq: u32) bool {
    block.ping_payload = shared.magic_ping;
    asm volatile ("dsb" ::: "memory");
    block.ping_seq = seq;
    var polls: u32 = 0;
    while (polls < poll_budget) : (polls += 1) {
        if (block.pong_seq == seq) return block.pong_payload == shared.magic_pong;
    }
    return false;
}

fn pingPong(block: *volatile shared.Block) bool {
    var seq: u32 = 1;
    while (seq <= shared.rounds) : (seq += 1) {
        if (!roundTrip(block, seq)) return false;
        @as(*volatile u32, &g_cpu1_pingpong_ra8p1_match).* += 1;
    }
    return true;
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

export fn main() callconv(.c) c_int {
    const block = shared.block();
    block.* = .{ .ping_seq = 0, .pong_seq = 0, .ping_payload = 0, .pong_payload = 0 };
    asm volatile ("dsb" ::: "memory");
    if (ra8_cgc_init() != ok) park();
    const console = ra8_board_uart_console_init(baud) == ok;
    if (ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top) != ok) {
        if (console) say(fail_line);
        park();
    }
    const passed = pingPong(block);
    if (console) say(if (passed) pass_line else fail_line);
    park();
}
