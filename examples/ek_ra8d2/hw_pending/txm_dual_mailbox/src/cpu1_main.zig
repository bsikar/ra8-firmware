//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_dual_mailbox, CPU1 half (RA8FW-843, RA8FW-849). The `threadx_cpu1`
//! glue owns the vector table and reset path and enters the kernel;
//! `tx_application_define` creates the Module Manager thread. That thread
//! initializes the manager, makes the object pool, loads txm_dual_server_m33
//! in place from `.txm_module` and starts it, checks the start thread sits at
//! its measured offset, and waits for the module to attach its two queues.
//! The module is the `ra8_rpc` server; this image only moves messages. Once
//! per tick it copies the start thread's run count into the mailbox block,
//! moves the M85 module's calls from the request slot into the module's
//! request queue, and the module's answers into the reply slot (pump.zig).
//! Once the manager reports a memory fault in the module (RA8FW-842), it
//! stops feeding the module's queues and refuses every call itself with an
//! `ra8_rpc` fault frame carrying `service.module_gone`.

const glue = @import("threadx_cpu1");
const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const shared = @import("shared.zig");
const service = @import("service.zig");
const pump = @import("pump.zig");

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
extern fn _txm_module_manager_memory_fault_notify(notify: *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void) callconv(.c) u32;
extern fn _txe_queue_create(
    queue: *anyopaque,
    name: [*:0]const u8,
    message_words: c_uint,
    storage: *anyopaque,
    storage_bytes: c_ulong,
    control_block_bytes: c_uint,
) callconv(.c) c_uint;

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
    if (!passed(block, Step.load, _txm_module_manager_in_place_load(&instance, "txm_dual_server_m33", &g_ra8_ls_cpu1_txm_module_start))) return false;
    if (!passed(block, Step.start, _txm_module_manager_start(&instance))) return false;
    const id = shared.instanceWord(&instance, shared.cpu1_start_thread_offset);
    return passed(block, Step.thread, if (id == shared.tx_thread_id) tx_success else id);
}

/// Faults the manager reported in the module (RA8FW-842).
var faults: u32 = 0;

/// Called from the MemManage handler once the manager has terminated the
/// faulting module thread.
fn faulted(thread_ptr: ?*anyopaque, module: ?*anyopaque) callconv(.c) void {
    _ = thread_ptr;
    _ = module;
    _ = @atomicRmw(u32, &faults, .Add, 1, .seq_cst);
    shared.block().cpu1_faults += 1;
}

/// The two queues the module created, once it has attached them: requests
/// in, then replies out. Written on the module's thread.
var attached: [2]usize = .{ 0, 0 };

/// The manager's hook for a module's application requests, less
/// `service.Report.base`. It runs on the module's thread, in the resident
/// image.
export fn _txm_module_manager_application_request(
    request: u32,
    param_1: u32,
    param_2: u32,
    param_3: u32,
) callconv(.c) u32 {
    _ = param_3;
    const block = shared.block();
    switch (request) {
        service.Report.attach => {
            attached[1] = param_2;
            @atomicStore(usize, &attached[0], param_1, .release);
        },
        // param_1 is the sum the module answered, param_2 its count.
        service.Report.value => block.answered += 1,
        service.Report.failed => {
            block.result = param_1;
            block.failed_step = shared.Step.module;
        },
        else => {},
    }
    return tx_success;
}

/// Wait for the module's queues. False, with the step in the block, if
/// they never come.
fn awaitQueues(block: *volatile shared.Block) bool {
    var ticks: u32 = 0;
    while (@atomicLoad(usize, &attached[0], .acquire) == 0) : (ticks += 1) {
        if (ticks == service.patience_ticks) return passed(block, shared.Step.attach, 1);
        _ = _tx_thread_sleep(1);
    }
    return true;
}

/// Move the M85's waiting call into the module's request queue, and the
/// module's answers into the reply slot.
fn pumpOnce(block: *volatile shared.Block) void {
    while (pump.take(&block.request, @ptrFromInt(attached[0]))) {}
    while (pump.send(@ptrFromInt(attached[1]), &block.reply)) {}
}

/// sizeof(TX_QUEUE) with the Module Manager configuration and the M33 flags
/// (measured, 0x44); `_txe_queue_create` checks it.
const queue_block_bytes = 68;
const Storage = [service.queue_depth * service.message_words]u32;
const KernelQueue = rpc_tx.TxQueue(rpc_tx.kernel.api);

/// The queue CPU1 answers on once its module is gone. Everything below must
/// not move once bound, so it lives here.
var gone_queue: [queue_block_bytes]u8 align(8) = undefined;
var gone_storage: Storage = undefined;
var gone: KernelQueue = undefined;
var gone_tx_message: [service.message_bytes]u8 = undefined;
var gone_rx_message: [service.message_bytes]u8 = undefined;
var gone_wire: rpc.QueueTransport = undefined;
var gone_frame: [rpc.frame.maxSize(rpc.Fault)]u8 = undefined;
/// Whether the call the module died on has been refused.
var refused_first = false;

/// Make the queue and the transport the refusals go out on. Only `out` is
/// ever used; nothing comes in on it.
fn bindGone(block: *volatile shared.Block) bool {
    const created = _txe_queue_create(&gone_queue, "module gone", service.message_words, &gone_storage, @sizeOf(Storage), queue_block_bytes);
    if (!passed(block, shared.Step.queue, created)) return false;
    gone = KernelQueue.init(&gone_queue, service.message_bytes) catch return passed(block, shared.Step.queue, 1);
    gone_wire = rpc.QueueTransport.init(gone.queue(), gone.queue(), &gone_tx_message, &gone_rx_message) catch
        return passed(block, shared.Step.queue, 2);
    return true;
}

/// Queue one fault frame saying the module is gone.
fn refuse(block: *volatile shared.Block) bool {
    const fault: rpc.Fault = .{ .code = @enumFromInt(service.module_gone) };
    rpc.link.post(gone_wire.transport(), rpc.Fault, rpc.Kind.fault, fault, &gone_frame) catch |err| {
        block.result = @intFromError(err);
        block.failed_step = shared.Step.refuse;
        return false;
    };
    return true;
}

/// With the module gone: refuse the call it died on, then every call that
/// arrives after it, and send the refusals.
fn answerGone(block: *volatile shared.Block) bool {
    if (!refused_first) {
        if (!refuse(block)) return false;
        refused_first = true;
    }
    while (pump.discard(&block.request)) {
        if (!refuse(block)) return false;
    }
    while (pump.send(&gone_queue, &block.reply)) {}
    return true;
}

/// One tick's traffic: through the module while it lives, else refused here.
fn tick(block: *volatile shared.Block) bool {
    if (@atomicLoad(u32, &faults, .acquire) == 0) {
        pumpOnce(block);
        return true;
    }
    return answerGone(block);
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    const block = shared.block();
    block.signature = shared.signature;
    if (loadAndStart(block) and awaitQueues(block) and bindGone(block)) {
        while (true) {
            // Rewritten every tick, so the M85 sees CPU1 alive whatever order
            // the two cores reached the block in.
            block.signature = shared.signature;
            block.module_runs = shared.instanceWord(&instance, shared.cpu1_start_thread_offset + shared.run_count_offset);
            if (!tick(block)) break;
            _ = _tx_thread_sleep(1);
        }
    }
    while (true) _ = _tx_thread_sleep(idle_ticks);
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    glue.startTicks(cpu1_hz);
    _ = _tx_thread_create(&thread, "module manager", &manager, 0, &stack, stack_bytes, priority, priority, no_time_slice, auto_start);
}
