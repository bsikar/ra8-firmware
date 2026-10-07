//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! cpu1_routed_irq, M85 half (RA8FW-809). Brings up the clock and the SCI8
//! VCOM console, clears the shared block, releases CPU1 and waits for it to
//! arm, hands GPT0's overflow event to CPU1 in INTSELR, starts GPT0, and
//! prints PASS once CPU1's handler has reported. Any failed step prints FAIL.

const shared = @import("shared.zig");

pub const baud: u32 = 115_200;
/// Polls of the shared block before giving up: about two seconds at 1 GHz.
pub const spin_limit: u32 = 400_000_000;
pub const pass_line = "cpu1_routed_irq: CPU1 handled GPT0 PASS\r\n";
pub const fail_line = "cpu1_routed_irq: FAIL\r\n";
/// GPT0's period in PCLKD cycles.
pub const period: u32 = 0x1000;

/// ra8_gpt_cfg_t (inc/ra8_gpt.h).
const GptCfg = extern struct {
    mode: u8,
    prescaler: u8,
    period: u32,
    duty_a: u32,
    duty_b: u32,
    auto_start: bool,
};
const saw_pwm: u8 = 0;
const div_1: u8 = 0;
const gpt0: u8 = 0;

const ok: c_int = 0;
const ok_err: u16 = 0;
extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_cpu1_release(mram: *const anyopaque, stack: *const anyopaque) c_int;
extern fn ra8_gpt_init(channel: u8, cfg: *const GptCfg) u16;
extern fn ra8_gpt_start_free_run(channel: u8, period: u32) u16;
extern const g_ra8_ls_cpu1_mram_start: u8;
extern const g_ra8_ls_cpu1_stack_top: u8;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn waitSet(word: *volatile u32) bool {
    var polls: u32 = 0;
    while (polls < spin_limit) : (polls += 1) {
        if (word.* != 0) return true;
    }
    return false;
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

/// Hands the event to CPU1 and starts GPT0; false if the timer refused.
fn fire() bool {
    shared.intselr().* |= shared.intselrBit();
    const cfg = GptCfg{ .mode = saw_pwm, .prescaler = div_1, .period = period, .duty_a = 0, .duty_b = 0, .auto_start = false };
    if (ra8_gpt_init(gpt0, &cfg) != ok_err) return false;
    return ra8_gpt_start_free_run(gpt0, period) == ok_err;
}

fn run(block: *volatile shared.Block) bool {
    if (ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top) != ok) return false;
    if (!waitSet(&block.armed)) return false;
    if (!fire()) return false;
    return waitSet(&block.irq_count);
}

export fn main() callconv(.c) c_int {
    const block = shared.block();
    block.* = .{ .armed = 0, .irq_count = 0 };
    asm volatile ("dsb" ::: .{ .memory = true });
    if (ra8_cgc_init() != ok) park();
    const console = ra8_board_uart_console_init(baud) == ok;
    const passed = run(block);
    if (console) say(if (passed) pass_line else fail_line);
    park();
}
