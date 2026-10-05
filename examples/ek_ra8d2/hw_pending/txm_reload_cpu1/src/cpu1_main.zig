//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_reload_cpu1, CPU1 half (RA8FW-774). The `threadx_cpu1` glue owns the
//! vector table and reset path and enters the kernel; `tx_application_define`
//! creates the Module Manager thread. That thread runs upstream's whole
//! sample_threadx_module_manager.c round trip on txm_hello_m33, which the
//! image carries in `.txm_module`: initialize, object pool, in-place load and
//! start, then, once the start thread has run `shared.pass_runs` times, stop,
//! unload, in-place load and start again from the same blob. It copies the
//! current round's run count into the shared block once per tick.

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
/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_start_stop_thread) is
/// 0xD0 and offsetof(TX_THREAD, tx_thread_run_count) is 4 (measured).
pub const start_run_count_offset = 0xD0 + 4;
pub const stack_bytes = 2048;
/// Where the manager places module data and the module threads' stacks.
pub const module_ram_bytes = 16 * 1024;
/// Where the manager allocates kernel objects a module asks for.
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
extern fn _txm_module_manager_stop(module: *anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_unload(module: *anyopaque) callconv(.c) u32;

fn passed(block: *volatile shared.Block, step: u32, result: u32) bool {
    if (result == tx_success) return true;
    block.failed_step = step;
    block.result = result;
    return false;
}

fn setUp(block: *volatile shared.Block) bool {
    const Step = shared.Step;
    if (!passed(block, Step.initialize, _txm_module_manager_initialize(&module_ram, module_ram_bytes))) return false;
    return passed(block, Step.object_pool, _txm_module_manager_object_pool_create(&object_pool, object_pool_bytes));
}

fn loadAndStart(block: *volatile shared.Block) bool {
    const Step = shared.Step;
    if (!passed(block, Step.load, _txm_module_manager_in_place_load(&instance, "txm_hello_m33", &g_ra8_ls_cpu1_txm_module_start))) return false;
    return passed(block, Step.start, _txm_module_manager_start(&instance));
}

fn stopAndUnload(block: *volatile shared.Block) bool {
    const Step = shared.Step;
    if (!passed(block, Step.stop, _txm_module_manager_stop(&instance))) return false;
    return passed(block, Step.unload, _txm_module_manager_unload(&instance));
}

fn startRunCount() u32 {
    const count: *align(1) volatile u32 = @ptrCast(&instance[start_run_count_offset]);
    return count.*;
}

/// Publishes the run count once per tick until it reaches the pass mark.
fn waitRuns(block: *volatile shared.Block) void {
    while (true) {
        const runs = startRunCount();
        block.module_runs = runs;
        if (runs >= shared.pass_runs) return;
        _ = _tx_thread_sleep(1);
    }
}

/// Opens the next round: its run count reads 0 before its number shows.
fn beginRound(block: *volatile shared.Block, round: u32) void {
    block.module_runs = 0;
    asm volatile ("dmb" ::: "memory");
    block.round = round;
}

fn roundTrip(block: *volatile shared.Block) bool {
    if (!setUp(block)) return false;
    beginRound(block, 1);
    if (!loadAndStart(block)) return false;
    waitRuns(block);
    if (!stopAndUnload(block)) return false;
    beginRound(block, 2);
    if (!loadAndStart(block)) return false;
    waitRuns(block);
    return true;
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    const block = shared.block();
    block.signature = shared.signature;
    _ = roundTrip(block);
    while (true) _ = _tx_thread_sleep(idle_ticks);
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    glue.startTicks(cpu1_hz);
    _ = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
}
