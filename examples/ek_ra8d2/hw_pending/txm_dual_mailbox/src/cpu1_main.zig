//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_dual_mailbox, CPU1 half (RA8FW-843). The `threadx_cpu1` glue owns the
//! vector table and reset path and enters the kernel; `tx_application_define`
//! creates the Module Manager thread. That thread initializes the manager,
//! makes the object pool, loads txm_hello_m33 in place from `.txm_module` and
//! starts it, checks the start thread sits at its measured offset, then
//! copies the start thread's run count into the mailbox block once per tick.

const glue = @import("threadx_cpu1");
const shared = @import("shared.zig");

/// CPU1's clock (the RA8D2's M33 maximum). A slower real clock only makes the
/// ticks slower, never wrong in count.
pub const cpu1_hz: u32 = 250_000_000;
/// sizeof(TX_THREAD) is 232 with the Module Manager configuration and the M33
/// flags (measured); this leaves headroom without a C import.
pub const thread_bytes = 256;
/// sizeof(TXM_MODULE_INSTANCE) is 1196 on the same configuration (measured).
pub const instance_bytes = 1280;
pub const stack_bytes = 2048;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;
pub const priority: u32 = 1;

const tx_success: u32 = 0;
const no_time_slice: u32 = 0;
const auto_start: u32 = 1;
const idle_ticks: u32 = 100;

var thread: [thread_bytes]u8 align(8) = undefined;
var stack: [stack_bytes]u8 align(8) = undefined;
var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;

/// The start of `.txm_module`, set by this app's CPU1 linker script.
extern const g_ra8_ls_cpu1_txm_module_start: u8;

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
    if (!passed(block, Step.load, _txm_module_manager_in_place_load(&instance, "txm_hello_m33", &g_ra8_ls_cpu1_txm_module_start))) return false;
    if (!passed(block, Step.start, _txm_module_manager_start(&instance))) return false;
    const id = shared.instanceWord(&instance, shared.cpu1_start_thread_offset);
    return passed(block, Step.thread, if (id == shared.tx_thread_id) tx_success else id);
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    const block = shared.block();
    block.signature = shared.signature;
    if (!loadAndStart(block)) {
        while (true) _ = _tx_thread_sleep(idle_ticks);
    }
    while (true) {
        // Rewritten every tick, so the M85 sees CPU1 alive whatever order
        // the two cores reached the block in.
        block.signature = shared.signature;
        block.module_runs = shared.instanceWord(&instance, shared.cpu1_start_thread_offset + shared.run_count_offset);
        _ = _tx_thread_sleep(1);
    }
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    glue.startTicks(cpu1_hz);
    _ = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
}
