//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_fault_m85 (RA8FW-805): txm_fault_cpu1's negative case with the Module
//! Manager on CPU0. The M85 links `threadx_m85_modules` and carries
//! txm_fault_m33 in `.txm_module`; that module's start thread stores outside
//! its MPU regions. The manager registers a memory-fault callback before the
//! load. The port's MemManage handler terminates the module thread and calls
//! it, and the manager then counts its own ticks: proof the kernel carried on.
//! The verdict goes to the SCI8 VCOM console. No CPU1 image.

const std = @import("std");

pub const panic = std.debug.no_panic;

pub const baud: u32 = 115_200;
/// Manager ticks after the fault that count as a pass.
pub const pass_ticks: u32 = 10;
/// Ticks the manager waits for the fault before failing.
pub const tick_limit: u32 = 1000;
pub const pass_line = std.fmt.comptimePrint("txm_fault_m85: module faulted, manager ran {d} more ticks PASS\r\n", .{pass_ticks});
pub const fail_line = "txm_fault_m85: FAIL\r\n";

/// The txm_manager_m85 sizes (same port, same defines).
pub const thread_bytes = 256;
pub const instance_bytes = 1280;
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

/// Written by the fault callback, read by the manager thread.
var faults: u32 = 0;
var own_module: bool = false;

/// The start of `.txm_module`, set by this app's linker_append.ld.
extern const g_ra8_ls_txm_module_start: u8;

const Notify = *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void;

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
extern fn _txm_module_manager_memory_fault_notify(notify: Notify) callconv(.c) u32;
extern fn _txm_module_manager_in_place_load(module: *anyopaque, name: [*:0]const u8, location: *const anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_start(module: *anyopaque) callconv(.c) u32;

fn say(line: []const u8) void {
    _ = ra8_board_uart_console_write(line.ptr, line.len);
    _ = ra8_board_uart_console_flush();
}

fn park() noreturn {
    while (true) asm volatile ("wfi");
}

/// Called from the MemManage handler after the faulting thread is terminated.
fn faulted(thread_ptr: ?*anyopaque, module: ?*anyopaque) callconv(.c) void {
    _ = thread_ptr;
    @atomicStore(bool, &own_module, module == @as(?*anyopaque, &instance), .seq_cst);
    _ = @atomicRmw(u32, &faults, .Add, 1, .seq_cst);
}

fn loadAndStart() bool {
    if (_txm_module_manager_initialize(&module_ram, module_ram_bytes) != tx_success) return false;
    if (_txm_module_manager_object_pool_create(&object_pool, object_pool_bytes) != tx_success) return false;
    if (_txm_module_manager_memory_fault_notify(&faulted) != tx_success) return false;
    if (_txm_module_manager_in_place_load(&instance, "txm_fault_m33", &g_ra8_ls_txm_module_start) != tx_success) return false;
    return _txm_module_manager_start(&instance) == tx_success;
}

/// True once the fault named this module and the manager ticked on after it.
fn outlivedFault() bool {
    var ticks: u32 = 0;
    while (ticks < tick_limit) : (ticks += 1) {
        if (@atomicLoad(u32, &faults, .seq_cst) != 0) break;
        _ = _tx_thread_sleep(1);
    } else return false;
    var after: u32 = 0;
    while (after < pass_ticks) : (after += 1) _ = _tx_thread_sleep(1);
    return @atomicLoad(bool, &own_module, .seq_cst);
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    say(if (loadAndStart() and outlivedFault()) pass_line else fail_line);
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
