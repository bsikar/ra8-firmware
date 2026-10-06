//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Module Manager half of txm_sd_hello_m85. hello (RA8FW-830) copies a
//! verified module out of RAM into module memory, starts it unprivileged,
//! watches its start thread run, then stops and unloads it. fault
//! (RA8FW-837) loads the faulting module the same way and checks the MPU
//! kills it while the manager ticks on. Offsets as txm_manager_m85 reads them
//! off a run (RA8FW-825).

/// Runs of the module's start thread that count as a pass.
pub const pass_runs: u32 = 10;
/// Manager ticks after the fault that count as a pass.
pub const pass_ticks: u32 = 10;
/// Ticks to wait for the runs or the fault.
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

pub const Fail = error{ manager, load, start, run, stop, unload, notify, fault };

var instance: [instance_bytes]u8 align(8) = undefined;
var module_ram: [module_ram_bytes]u8 align(32) = undefined;
var object_pool: [object_pool_bytes]u8 align(8) = undefined;
var faults: u32 = 0;
var own_module: bool = false;

const Notify = *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void;

extern fn _tx_thread_sleep(ticks: u32) callconv(.c) u32;
extern fn _txm_module_manager_initialize(ram: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_object_pool_create(pool: *anyopaque, size: u32) callconv(.c) u32;
extern fn _txm_module_manager_memory_fault_notify(notify: Notify) callconv(.c) u32;
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

/// Called from the MemManage handler after the faulting thread is terminated.
fn faulted(thread_ptr: ?*anyopaque, module: ?*anyopaque) callconv(.c) void {
    _ = thread_ptr;
    @atomicStore(bool, &own_module, module == @as(?*anyopaque, &instance), .seq_cst);
    _ = @atomicRmw(u32, &faults, .Add, 1, .seq_cst);
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
    return @atomicLoad(bool, &own_module, .seq_cst) and @atomicLoad(u32, &faults, .seq_cst) == 1;
}

/// Brings up the Module Manager and registers the fault callback, once.
pub fn init() Fail!void {
    if (_txm_module_manager_initialize(&module_ram, module_ram_bytes) != tx_success) return error.manager;
    if (_txm_module_manager_object_pool_create(&object_pool, object_pool_bytes) != tx_success) return error.manager;
    if (_txm_module_manager_memory_fault_notify(&faulted) != tx_success) return error.notify;
}

/// Loads the module image at `payload` (its preamble first), runs it until
/// its start thread has run `pass_runs` times, then stops and unloads it.
pub fn hello(payload: []const u8) Fail!void {
    if (_txm_module_manager_memory_load(&instance, "txm_hello_m33", payload.ptr) != tx_success) return error.load;
    if (_txm_module_manager_start(&instance) != tx_success) return error.start;
    if (word(start_thread_offset) != tx_thread_id or !waitRuns()) return error.run;
    if (_txm_module_manager_stop(&instance) != tx_success) return error.stop;
    if (_txm_module_manager_unload(&instance) != tx_success) return error.unload;
}

/// Loads and starts the faulting module at `payload`, then waits for the MPU
/// to kill it and for `pass_ticks` more manager ticks. No faults may land
/// before it starts: the hello module has to come and go cleanly.
pub fn fault(payload: []const u8) Fail!void {
    if (@atomicLoad(u32, &faults, .seq_cst) != 0) return error.fault;
    if (_txm_module_manager_memory_load(&instance, "txm_fault_m33", payload.ptr) != tx_success) return error.load;
    if (_txm_module_manager_start(&instance) != tx_success) return error.start;
    if (!outlivedFault()) return error.fault;
}
