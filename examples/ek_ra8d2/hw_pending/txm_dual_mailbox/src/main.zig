//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_dual_mailbox, M85 half (RA8FW-843, under RA8EMU-159): a ThreadX module
//! on each core of one image. The M85 links `threadx_m85_modules` and carries
//! txm_hello_m33 in its own `.txm_module` (linker_append.ld). main clears the
//! mailbox block, releases CPU1 and enters the kernel; the manager thread
//! loads and starts the M85's module, then waits for its start thread and
//! CPU1's (reported in the block, cpu1_main.zig) to have run `pass_runs`
//! times each, and prints the verdict on the SCI8 VCOM console.

const std = @import("std");
const shared = @import("shared.zig");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
/// Ticks the manager waits for both modules before failing.
pub const tick_limit: u32 = 2000;
pub const pass_line = std.fmt.comptimePrint("txm_dual_mailbox: modules ran {d} times on both cores PASS\r\n", .{shared.pass_runs});
pub const fail_line = "txm_dual_mailbox: FAIL\r\n";
pub const cpu1_fail_line = "txm_dual_mailbox: FAIL cpu1\r\n";

/// sizeof(TX_THREAD) is 232 on the cortex_m33 module port; headroom.
pub const thread_bytes = 256;
/// sizeof(TXM_MODULE_INSTANCE) is 1196 on the same port and defines.
pub const instance_bytes = 1280;
pub const stack_bytes = 2048;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;
const ok: c_int = 0;

var thread: [thread_bytes]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;

/// The start of the M85's `.txm_module`, set by linker_append.ld.
extern const g_ra8_ls_txm_module_start: u8;
extern const g_ra8_ls_cpu1_mram_start: u8;
extern const g_ra8_ls_cpu1_stack_top: u8;

extern fn ra8_cgc_init() c_int;
extern fn ra8_board_uart_console_init(baud: u32) c_int;
extern fn ra8_board_uart_console_write(data: [*]const u8, len: usize) c_int;
extern fn ra8_board_uart_console_flush() c_int;
extern fn ra8_cpu1_release(mram: *const anyopaque, stack: *const anyopaque) c_int;
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
extern fn _txm_module_manager_initialize(ram: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_object_pool_create(pool: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_in_place_load(module: *anyopaque, name: [*:0]const u8, location: *const anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_start(module: *anyopaque) callconv(.c) u32;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

fn loadAndStart() bool {
    if (_txm_module_manager_initialize(&module_ram, module_ram_bytes) != tx_success) return false;
    if (_txm_module_manager_object_pool_create(&object_pool, object_pool_bytes) != tx_success) return false;
    if (_txm_module_manager_in_place_load(&instance, "txm_hello_m33", &g_ra8_ls_txm_module_start) != tx_success) return false;
    if (_txm_module_manager_start(&instance) != tx_success) return false;
    return shared.instanceWord(&instance, shared.m85_start_thread_offset) == shared.tx_thread_id;
}

const Verdict = enum { waiting, pass, cpu1_failed };

/// Both modules' run counts against `pass_runs`, CPU1's off the block.
fn verdict(block: *volatile shared.Block) Verdict {
    if (block.signature == shared.signature and block.failed_step != shared.Step.none) return .cpu1_failed;
    const m85_runs = shared.instanceWord(&instance, shared.m85_start_thread_offset + shared.run_count_offset);
    if (m85_runs < shared.pass_runs) return .waiting;
    if (block.signature != shared.signature or block.module_runs < shared.pass_runs) return .waiting;
    return .pass;
}

fn waitBoth() []const u8 {
    const block = shared.block();
    var ticks: u32 = 0;
    while (ticks < tick_limit) : (ticks += 1) {
        switch (verdict(block)) {
            .waiting => _ = _tx_thread_sleep(1),
            .pass => return pass_line,
            .cpu1_failed => return cpu1_fail_line,
        }
    }
    return fail_line;
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    say(if (loadAndStart()) waitBoth() else fail_line);
    while (true) _ = _tx_thread_sleep(tick_limit);
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) say(fail_line);
}

export fn main() callconv(.c) c_int {
    const block = shared.block();
    block.* = .{ .signature = 0, .failed_step = shared.Step.none, .result = 0, .module_runs = 0 };
    asm volatile ("dsb" ::: "memory");
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != ok) park();
    _ = ra8_board_uart_console_init(baud);
    if (ra8_cpu1_release(&g_ra8_ls_cpu1_mram_start, &g_ra8_ls_cpu1_stack_top) != ok) {
        say(cpu1_fail_line);
        park();
    }
    _tx_initialize_kernel_enter();
    say(fail_line);
    park();
}
