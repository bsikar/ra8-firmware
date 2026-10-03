//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_rpc_cpu1, CPU1 half (RA8FW-544). The `threadx_cpu1` glue owns the
//! vector table and reset path and enters the kernel; `tx_application_define`
//! creates the Module Manager thread. That thread runs upstream's
//! sample_threadx_module_manager.c sequence (initialize, object pool, in-place
//! load, start) on txm_rpc_m33, which the image carries in `.txm_module`.
//!
//! The module creates two ThreadX queues and hands them over as an
//! application request. The manager thread then serves `ra8_rpc` on them:
//! a `Server` with one method, `add`, on a `QueueTransport` whose queues are
//! bound to the kernel's own entry points through `ra8_rpc_tx`. The module
//! reports each sum it gets back, and `_txm_module_manager_application_request`
//! records them, and whether each was the sum expected, in the shared block.

const glue = @import("threadx_cpu1");
const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const shared = @import("shared.zig");
const service = @import("service.zig");

/// CPU1's clock (the RA8D2's M33 maximum). A slower real clock only makes the
/// ticks slower, never wrong in count.
pub const cpu1_hz: u32 = 250_000_000;
/// sizeof(TX_THREAD) is 232 with the Module Manager configuration and the M33
/// flags (measured); this leaves headroom without a C import.
pub const thread_bytes = 256;
/// sizeof(TXM_MODULE_INSTANCE) is 1196 on the same configuration (measured).
pub const instance_bytes = 1280;
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
extern fn _txm_module_manager_in_place_load(
    module: *anyopaque,
    name: [*:0]const u8,
    location: *const anyopaque,
) callconv(.c) u32;
extern fn _txm_module_manager_start(module: *anyopaque) callconv(.c) u32;

fn passed(block: *volatile shared.Block, step: u32, result: u32) bool {
    if (result == tx_success) return true;
    block.failed_step = step;
    block.result = result;
    return false;
}

fn loadAndStart(block: *volatile shared.Block) bool {
    const Step = shared.Step;
    const initialized = _txm_module_manager_initialize(&module_ram, module_ram_bytes);
    if (!passed(block, Step.initialize, initialized)) return false;
    const pooled = _txm_module_manager_object_pool_create(&object_pool, object_pool_bytes);
    if (!passed(block, Step.object_pool, pooled)) return false;
    const image = &g_ra8_ls_cpu1_txm_module_start;
    const loaded = _txm_module_manager_in_place_load(&instance, "txm_rpc_m33", image);
    if (!passed(block, Step.load, loaded)) return false;
    return passed(block, Step.start, _txm_module_manager_start(&instance));
}

/// The two queues the module created, once it has attached them: the one
/// to this image, then the one back. Written on the module's thread.
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
        service.Report.value => {
            // param_1 is the sum and param_2 the step it belongs to.
            const right = block.reports == param_2 and param_1 == service.sumFor(param_2);
            if (!right) block.mismatches += 1;
            if (block.reports < shared.kept) block.values[block.reports] = param_1;
            block.reports += 1;
        },
        service.Report.failed => {
            block.result = param_1;
            block.failed_step = shared.Step.module;
        },
        else => block.mismatches += 1,
    }
    return tx_success;
}

const KernelQueue = rpc_tx.TxQueue(rpc_tx.kernel.api);

const Answers = struct {
    block: *volatile shared.Block,
};

fn add(context: *Answers, args: service.Add) rpc.Outcome(service.Sum) {
    context.block.answered += 1;
    return .{ .ok = .{ .value = args.a +% args.b } };
}

const Server = rpc.Server(Answers, service.max_body, .{.{ service.Method.add, add }});

/// Everything below must not move once bound, so it lives here.
var up: KernelQueue = undefined;
var down: KernelQueue = undefined;
var tx_message: [service.message_bytes]u8 = undefined;
var rx_message: [service.message_bytes]u8 = undefined;
var wire: rpc.QueueTransport = undefined;
var answers: Answers = undefined;
var server: Server = undefined;
var rx: [Server.Env.max_frame]u8 = undefined;
var tx: [Server.Env.max_frame]u8 = undefined;

/// Wait for the module's queues, then bind a server to them.
fn bind(block: *volatile shared.Block) bool {
    var ticks: u32 = 0;
    while (@atomicLoad(usize, &attached[0], .acquire) == 0) : (ticks += 1) {
        if (ticks == service.patience_ticks) {
            block.failed_step = shared.Step.attach;
            return false;
        }
        _ = _tx_thread_sleep(1);
    }
    const message_bytes = service.message_bytes;
    up = KernelQueue.init(@ptrFromInt(attached[0]), message_bytes) catch unreachable;
    down = KernelQueue.init(@ptrFromInt(attached[1]), message_bytes) catch unreachable;
    wire = rpc.QueueTransport.init(down.queue(), up.queue(), &tx_message, &rx_message) catch
        unreachable;
    answers = .{ .block = block };
    server = Server.init(wire.transport(), &rx, &answers, service.caps);
    return true;
}

/// Answer whatever has arrived, then come back next tick. Returns only if
/// the server fails, with the failure in the block.
fn serve(block: *volatile shared.Block) void {
    while (true) {
        while (true) {
            const step = server.poll(&tx) catch |err| {
                block.result = @intFromError(err);
                block.failed_step = shared.Step.server;
                return;
            };
            if (step == .idle) break;
        }
        _ = _tx_thread_sleep(1);
    }
}

fn manager(input: u32) callconv(.c) void {
    _ = input;
    const block = shared.block();
    block.signature = shared.signature;
    if (loadAndStart(block) and bind(block)) serve(block);
    while (true) _ = _tx_thread_sleep(idle_ticks);
}

export fn tx_application_define(first_unused: ?*anyopaque) callconv(.c) void {
    _ = first_unused;
    glue.startTicks(cpu1_hz);
    _ = _tx_thread_create(
        &thread,
        "module manager",
        &manager,
        0,
        &stack,
        stack_bytes,
        priority,
        priority,
        no_time_slice,
        auto_start,
    );
}
