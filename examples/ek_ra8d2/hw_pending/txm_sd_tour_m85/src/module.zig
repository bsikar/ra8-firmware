//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Module Manager half of txm_sd_tour_m85 (RA8FW-1023): the load,
//! start, stop and unload of the tour module, the same cycle sd_hello runs
//! (RA8FW-830) but waiting for the module's exit request instead of runs of
//! its start thread. The backend (backend.zig) answers the module's calls
//! and records the exit code; the unload must give the manager's byte pool
//! back whole, so the next app has its memory. Offsets as txm_manager_m85
//! reads them off a run (RA8FW-825).

const backend = @import("backend.zig");

/// Ticks to wait for the module to exit.
pub const tick_limit: u32 = 1000;
/// sizeof(TXM_MODULE_INSTANCE) is 1196 on the cortex_m33 module port.
pub const instance_bytes = 1280;
/// offsetof(TXM_MODULE_INSTANCE, txm_module_instance_start_stop_thread).
pub const start_thread_offset = 0xC0;
/// TX_THREAD_ID, the first word of a created TX_THREAD.
pub const tx_thread_id: u32 = 0x5448_5244;
/// offsetof(TX_THREAD, tx_thread_run_count), from the start-stop thread.
pub const start_run_count_offset = start_thread_offset + 4;
pub const module_ram_bytes = 16 * 1024;
pub const object_pool_bytes = 4 * 1024;

const tx_success: u32 = 0;
pub const Fail = error{ manager, load, start, run, stop, unload, pool, leak, exit };

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
extern var _txm_module_manager_byte_pool: u8;
extern fn _tx_byte_pool_info_get(
    pool: *anyopaque,
    name: ?*?[*:0]u8,
    available: ?*u32,
    fragments: ?*u32,
    first_suspended: ?*?*anyopaque,
    suspended_count: ?*u32,
    next_pool: ?*?*anyopaque,
) callconv(.c) u32;

fn word(offset: usize) u32 {
    const w: *align(1) volatile u32 = @ptrCast(&instance[offset]);
    return w.*;
}

/// Brings up the Module Manager, once.
pub fn init() Fail!void {
    if (_txm_module_manager_initialize(&module_ram, module_ram_bytes) != tx_success) return error.manager;
    if (_txm_module_manager_object_pool_create(&object_pool, object_pool_bytes) != tx_success) return error.manager;
}

/// Bytes free in the manager's module-memory pool.
pub fn poolFree() Fail!u32 {
    var available: u32 = 0;
    if (_tx_byte_pool_info_get(&_txm_module_manager_byte_pool, null, &available, null, null, null, null) != tx_success) return error.pool;
    return available;
}

/// Loads the tour module, starts it, waits for its exit request, then stops
/// and unloads it. The code the module exited with comes back: zero is the
/// pass, anything else is the failing tour step. The pool must come back
/// whole, or the unload leaked.
pub fn tour(name: [*:0]const u8, payload: []const u8) Fail!u32 {
    const before = try poolFree();
    if (_txm_module_manager_memory_load(&instance, name, payload.ptr) != tx_success) return error.load;
    const held = try poolFree();
    backend.registered(&instance);
    backend.markStart();
    if (_txm_module_manager_start(&instance) != tx_success) return error.start;
    if (word(start_thread_offset) != tx_thread_id) return error.run;
    const code = backend.waitExit(tick_limit) orelse return error.exit;
    if (word(start_run_count_offset) == 0) return error.run;
    if (_txm_module_manager_stop(&instance) != tx_success) return error.stop;
    if (_txm_module_manager_unload(&instance) != tx_success) return error.unload;
    backend.deregistered();
    const after = try poolFree();
    if (held >= before or after != before) return error.leak;
    return code;
}
