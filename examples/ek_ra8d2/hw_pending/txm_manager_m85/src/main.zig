//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_manager_m85 (RA8FW-795): the ThreadX Module Manager on CPU0. The M85
//! links `threadx_m85_modules` (the RA8FW-481 scheduler and the RA8FW-484 MPU
//! budget) and carries txm_hello_m33 in `.txm_module` (CrossApp.txm_module,
//! RA8FW-796). The manager thread runs upstream's
//! sample_threadx_module_manager.c sequence (initialize, object pool,
//! in-place load, start), then watches the module start thread's run count
//! and prints the verdict on the SCI8 VCOM console. No CPU1 image.

const std = @import("std");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
/// Runs of the module's start thread that count as a pass.
pub const pass_runs: u32 = 10;
/// Ticks the manager waits for them before failing.
pub const tick_limit: u32 = 1000;
pub const pass_line = std.fmt.comptimePrint("txm_manager_m85: module ran {d} times PASS\r\n", .{pass_runs});
pub const fail_line = "txm_manager_m85: FAIL\r\n";

/// sizeof(TX_THREAD) is 232 with the Module Manager configuration on the
/// cortex_m33 module port, which the M85 archive builds unchanged; this
/// leaves headroom without a C import.
pub const thread_bytes = 256;
/// sizeof(TXM_MODULE_INSTANCE) is 1196 on the same port and defines.
pub const instance_bytes = 1280;
/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_start_stop_thread) is
/// 0xD0 and offsetof(TX_THREAD, tx_thread_run_count) is 4, measured on the
/// same port header and defines the M85 archive compiles.
pub const start_run_count_offset = 0xD0 + 4;
pub const stack_bytes = 2048;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;

var thread: [thread_bytes]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;

/// The start of `.txm_module`, set by this app's linker_append.ld.
extern const g_ra8_ls_txm_module_start: u8;

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
    return _txm_module_manager_start(&instance) == tx_success;
}

fn startRunCount() u32 {
    const count: *align(1) volatile u32 = @ptrCast(&instance[start_run_count_offset]);
    return count.*;
}

fn waitRuns() bool {
    var ticks: u32 = 0;
    while (ticks < tick_limit) : (ticks += 1) {
        if (startRunCount() >= pass_runs) return true;
        _ = _tx_thread_sleep(1);
    }
    return false;
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    say(if (loadAndStart() and waitRuns()) pass_line else fail_line);
    while (true) _ = _tx_thread_sleep(tick_limit);
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    const created = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
    if (created != tx_success) say(fail_line);
}

export fn main() callconv(.c) c_int {
    // CGC first: tx_initialize_low_level.S programs SysTick off the core clock.
    if (ra8_cgc_init() != 0) park();
    _ = ra8_board_uart_console_init(baud);
    _tx_initialize_kernel_enter();
    say(fail_line);
    park();
}
