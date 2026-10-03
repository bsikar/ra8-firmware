//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_fault_cpu1, CPU1 half (RA8FW-459). The same Module Manager bring-up
//! as txm_manager_cpu1 (initialize, object pool, in-place load, start), on
//! txm_fault_m33, whose start thread stores outside its MPU regions. Before the
//! load the manager registers a memory-fault callback. The port's MemManage
//! handler saves the fault registers, terminates the module thread and calls
//! it; the callback records the fault in the shared block. The manager thread
//! then counts its own ticks, which is the proof the kernel carried on.

const glue = @import("threadx_cpu1");
const shared = @import("shared.zig");

/// CPU1's clock (the RA8D2's M33 maximum).
pub const cpu1_hz: u32 = 250_000_000;
/// The txm_manager_cpu1 sizes (measured there on the same configuration).
pub const thread_bytes = 256;
pub const instance_bytes = 1280;
pub const stack_bytes = 2048;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;
const idle_ticks: u32 = 100;

/// The head of upstream's TXM_MODULE_MANAGER_MEMORY_FAULT_INFO, at the offsets
/// the port's MemManage handler stores to.
const FaultInfo = extern struct {
    thread: u32,
    code_location: u32,
    shcsr: u32,
    cfsr: u32,
    mmfar: u32,
    bfar: u32,
    control: u32,
};

var thread: [thread_bytes]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;

/// The start of `.txm_module`, set by this app's CPU1 linker script.
extern const g_ra8_ls_cpu1_txm_module_start: u8;
extern const _txm_module_manager_memory_fault_info: FaultInfo;

const Notify = *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void;

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

/// Called from the MemManage handler after the faulting thread is terminated.
fn faulted(thread_ptr: ?*anyopaque, module: ?*anyopaque) callconv(.c) void {
    _ = thread_ptr;
    const block = shared.block();
    const info: *const volatile FaultInfo = &_txm_module_manager_memory_fault_info;
    block.cfsr = info.cfsr;
    block.mmfar = info.mmfar;
    block.own_module = @intFromBool(module == @as(?*anyopaque, &instance));
    block.faults += 1;
}

fn passed(block: *volatile shared.Block, step: u32, result: u32) bool {
    if (result == tx_success) return true;
    block.failed_step = step;
    block.result = result;
    return false;
}

fn loadAndStart(block: *volatile shared.Block) bool {
    const Step = shared.Step;
    if (!passed(block, Step.initialize, _txm_module_manager_initialize(&module_ram, module_ram_bytes))) return false;
    if (!passed(block, Step.object_pool, _txm_module_manager_object_pool_create(&object_pool, object_pool_bytes))) return false;
    if (!passed(block, Step.notify, _txm_module_manager_memory_fault_notify(&faulted))) return false;
    if (!passed(block, Step.load, _txm_module_manager_in_place_load(&instance, "txm_fault_m33", &g_ra8_ls_cpu1_txm_module_start))) return false;
    return passed(block, Step.start, _txm_module_manager_start(&instance));
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    const block = shared.block();
    block.signature = shared.signature;
    if (!loadAndStart(block)) {
        while (true) _ = _tx_thread_sleep(idle_ticks);
    }
    while (true) {
        if (block.faults != 0) block.ticks_after_fault += 1;
        _ = _tx_thread_sleep(1);
    }
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    glue.startTicks(cpu1_hz);
    _ = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
}
