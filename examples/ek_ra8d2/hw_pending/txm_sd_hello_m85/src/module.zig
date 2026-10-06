//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Module Manager half of txm_sd_hello_m85 (RA8FW-830): copy a verified
//! module out of RAM into module memory, start it unprivileged, watch its
//! start thread run, then stop and unload it. Offsets as txm_manager_m85
//! reads them off a run (RA8FW-825).

/// Runs of the module's start thread that count as a pass.
pub const pass_runs: u32 = 10;
/// Ticks to wait for them.
pub const tick_limit: u32 = 1000;
/// sizeof(TXM_MODULE_INSTANCE) is 1196 on the cortex_m33 module port.
pub const instance_bytes = 1280;
/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_start_stop_thread).
pub const start_thread_offset = 0xC0;
/// TX_THREAD_ID, the first word of a created TX_THREAD.
pub const tx_thread_id: u32 = 0x5448_5244;
/// offsetof(TX_THREAD, tx_thread_run_count).
pub const start_run_count_offset = start_thread_offset + 4;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;

const tx_success: u32 = 0;

pub const Fail = error{ manager, load, start, run, stop, unload };

var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;

extern fn _tx_thread_sleep(ticks: u32) callconv(.c) u32;
extern fn _txm_module_manager_initialize(ram: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_object_pool_create(pool: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_memory_load(module: *anyopaque, name: [*:0]const u8, location: *const anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_start(module: *anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_stop(module: *anyopaque) callconv(.c) u32;
extern fn _txm_module_manager_unload(module: *anyopaque) callconv(.c) u32;

fn word(offset: usize) u32 {
    const w: *align(1) volatile u32 = @ptrCast(&instance[offset]);
    return w.*;
}

fn waitRuns() bool {
    var ticks: u32 = 0;
    while (ticks < tick_limit) : (ticks += 1) {
        if (word(start_run_count_offset) >= pass_runs) return true;
        _ = _tx_thread_sleep(1);
    }
    return false;
}

/// Loads the module image at `payload` (its preamble first), runs it until
/// its start thread has run `pass_runs` times, then stops and unloads it.
pub fn run(payload: []const u8) Fail!void {
    if (_txm_module_manager_initialize(&module_ram, module_ram_bytes) != tx_success) return error.manager;
    if (_txm_module_manager_object_pool_create(&object_pool, object_pool_bytes) != tx_success) return error.manager;
    if (_txm_module_manager_memory_load(&instance, "txm_hello_m33", payload.ptr) != tx_success) return error.load;
    if (_txm_module_manager_start(&instance) != tx_success) return error.start;
    if (word(start_thread_offset) != tx_thread_id or !waitRuns()) return error.run;
    if (_txm_module_manager_stop(&instance) != tx_success) return error.stop;
    if (_txm_module_manager_unload(&instance) != tx_success) return error.unload;
}
