//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_dual_client_m33's start thread (RA8FW-842): txm_dual_mailbox's M85
//! module, the `ra8_rpc` client. It began as txm_rpc_m33's (RA8FW-544).
//!
//! It creates two ThreadX queues in its own memory, one each way, and hands
//! them to the resident image as an application request; the resident image
//! carries their messages across the mailbox block to CPU1's server module.
//! Over them it runs an `ra8_rpc` client on a `QueueTransport`, each queue
//! bound through `ra8_rpc_tx`'s module Api. It greets the server, calls `add`
//! `shared.pass_calls` times, a tick apart, and reports each sum. Then it
//! calls `fault`, which kills CPU1's module, and waits for CPU1's resident
//! image to refuse that call in its place; it reports that as `gone`.
//!
//! The client reaches its transport, and the transport its queues, through
//! constant vtables of function pointers. Those words are in the module's
//! data, so the module is built through C and its start-up rebases them
//! (RA8FW-539).

const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const service = @import("service.zig");
const shared = @import("shared.zig");
const module = rpc_tx.module;
const ModuleQueue = rpc_tx.TxQueue(module.api);
const Client = rpc.Client(service.in_flight, service.max_body);
const Stage = service.Stage;

/// Set by the module's thread shell entry before this thread runs.
extern var _txm_module_kernel_call_dispatcher: module.Dispatcher;
extern fn _tx_thread_sleep(timer_ticks: u32) u32;
extern fn _txm_module_object_allocate(object: *?*anyopaque, bytes: u32) u32;
extern fn _txe_queue_create(
    queue: *anyopaque,
    name: [*:0]const u8,
    message_words: u32,
    storage: *anyopaque,
    storage_bytes: u32,
    control_block_bytes: u32,
) u32;

/// sizeof(TX_QUEUE) with the Module Manager configuration and the M33 or
/// M85 flags (measured, 0x44); this keeps the module free of a C import.
const queue_block_bytes = 68;
const tx_success = 0;

const Storage = [service.queue_depth * service.message_words]u32;

/// Everything below must not move once bound, so it lives here.
var up_storage: Storage = undefined;
var down_storage: Storage = undefined;
var up_ref: module.QueueRef = undefined;
var down_ref: module.QueueRef = undefined;
var up: ModuleQueue = undefined;
var down: ModuleQueue = undefined;
var tx_message: [service.message_bytes]u8 = undefined;
var rx_message: [service.message_bytes]u8 = undefined;
var wire: rpc.QueueTransport = undefined;
var client: Client = undefined;
var rx: [Client.Env.max_frame]u8 = undefined;
var tx: [Client.Env.max_frame]u8 = undefined;

fn report(request: u32, param_1: u32, param_2: u32) void {
    _ = _txm_module_kernel_call_dispatcher(service.Report.base + request, param_1, param_2, 0);
}

fn fail(stage: u32, detail: u32) noreturn {
    report(service.Report.failed, stage, detail);
    while (true) _ = _tx_thread_sleep(100);
}

fn errorCode(err: anyerror) u32 {
    return @intFromError(err);
}

/// A ThreadX queue of this module's own, in its own memory.
fn createQueue(name: [*:0]const u8, storage: *Storage) *anyopaque {
    var object: ?*anyopaque = null;
    const allocated = _txm_module_object_allocate(&object, queue_block_bytes);
    if (allocated != tx_success) fail(Stage.allocate, allocated);
    const words = service.message_words;
    const bytes = @sizeOf(Storage);
    const created = _txe_queue_create(object.?, name, words, storage, bytes, queue_block_bytes);
    if (created != tx_success) fail(Stage.create, created);
    return object.?;
}

/// Create the queues, hand them over, and bind the client to them.
fn connect() void {
    const dispatcher = _txm_module_kernel_call_dispatcher;
    up_ref = .{ .dispatcher = dispatcher, .queue = createQueue("rpc up", &up_storage) };
    down_ref = .{ .dispatcher = dispatcher, .queue = createQueue("rpc down", &down_storage) };
    report(service.Report.attach, @intFromPtr(up_ref.queue), @intFromPtr(down_ref.queue));

    up = ModuleQueue.init(up_ref.handle(), service.message_bytes) catch fail(Stage.bind, 0);
    down = ModuleQueue.init(down_ref.handle(), service.message_bytes) catch fail(Stage.bind, 1);
    wire = rpc.QueueTransport.init(up.queue(), down.queue(), &tx_message, &rx_message) catch
        fail(Stage.bind, 2);
    client = Client.init(wire.transport(), &rx, service.caps);
}

/// The next thing the server sends, waiting a tick at a time.
fn next() Client.Incoming {
    var ticks: u32 = 0;
    while (ticks < service.patience_ticks) : (ticks += 1) {
        const got = client.poll(&tx) catch |err| fail(Stage.poll, errorCode(err));
        if (got) |incoming| return incoming;
        _ = _tx_thread_sleep(1);
    }
    fail(Stage.timeout, 0);
}

fn greet() void {
    client.greet(&tx) catch |err| fail(Stage.greet, errorCode(err));
    if (next() != .ready) fail(Stage.greet, 0);
}

/// Call `add` for `step` and return the sum the server answered.
fn add(step: u32) u32 {
    const args = service.argsFor(step);
    const id = client.call(service.Add, service.Method.add, args, step, &tx) catch |err|
        fail(Stage.call, errorCode(err));
    const response = switch (next()) {
        .response => |response| response,
        else => fail(Stage.reply, step),
    };
    if (response.id != id or response.waiter != step) fail(Stage.reply, step);
    const bytes = switch (response.result) {
        .ok => |bytes| bytes,
        .err => |code| fail(Stage.refused, @intFromEnum(code)),
    };
    const sum = rpc.codec.decode(service.Sum, bytes) catch |err| fail(Stage.reply, errorCode(err));
    return sum.value;
}

/// Ask CPU1's server module to store outside its MPU regions, and wait for
/// the refusal CPU1's resident image sends once the module is gone. An
/// answer from the module itself means isolation did not hold.
fn fault() void {
    const args: service.Poke = .{ .address = shared.poke_address };
    _ = client.call(service.Poke, service.Method.fault, args, shared.pass_calls, &tx) catch |err|
        fail(Stage.call, errorCode(err));
    var ticks: u32 = 0;
    while (ticks < service.patience_ticks) : (ticks += 1) {
        const got = client.poll(&tx) catch |err| {
            if (err == error.Rejected) return report(service.Report.gone, 0, 0);
            fail(Stage.poll, errorCode(err));
        };
        if (got != null) fail(Stage.reply, shared.pass_calls);
        _ = _tx_thread_sleep(1);
    }
    fail(Stage.timeout, shared.pass_calls);
}

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    _ = id;
    connect();
    greet();
    var step: u32 = 0;
    while (step < shared.pass_calls) : (step += 1) {
        const sum = add(step);
        if (sum != service.sumFor(step)) fail(Stage.wrong, step);
        report(service.Report.value, sum, step);
        _ = _tx_thread_sleep(1);
    }
    fault();
    while (true) _ = _tx_thread_sleep(100);
}
