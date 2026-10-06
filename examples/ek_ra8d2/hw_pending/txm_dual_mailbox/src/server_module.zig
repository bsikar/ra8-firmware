//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_dual_server_m33's start thread (RA8FW-849): the `ra8_rpc` server
//! txm_dual_mailbox's M85 module calls, as a ThreadX module on CPU1.
//!
//! It creates two ThreadX queues in its own memory, requests in and replies
//! out, and hands them to CPU1's resident image as an application request.
//! The resident image only moves queue messages between them and the mailbox
//! block (pump.zig). The module answers `add` on them once a tick, through
//! `ra8_rpc_tx`'s module Api, and reports each sum so the resident image can
//! count it in the block.
//!
//! The server reaches its transport, and the transport its queues, through
//! constant vtables of function pointers in the module's data, so the module
//! is built through C and its start-up rebases them (RA8FW-539).

const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const service = @import("service.zig");
const module = rpc_tx.module;
const ModuleQueue = rpc_tx.TxQueue(module.api);
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

/// sizeof(TX_QUEUE) with the Module Manager configuration and the M33 flags
/// (measured, 0x44, as txm_rpc_m33 uses).
const queue_block_bytes = 68;
const tx_success = 0;

const Storage = [service.queue_depth * service.message_words]u32;

fn report(request: u32, param_1: u32, param_2: u32) void {
    _ = _txm_module_kernel_call_dispatcher(service.Report.base + request, param_1, param_2, 0);
}

fn fail(stage: u32, detail: u32) noreturn {
    report(service.Report.failed, stage, detail);
    while (true) _ = _tx_thread_sleep(100);
}

/// The server's context: how many calls it has answered.
const Answers = struct {
    count: u32 = 0,
};

fn add(context: *Answers, args: service.Add) rpc.Outcome(service.Sum) {
    const sum = args.a +% args.b;
    report(service.Report.value, sum, context.count);
    context.count +%= 1;
    return .{ .ok = .{ .value = sum } };
}

const Server = rpc.Server(Answers, service.max_body, .{.{ service.Method.add, add }});

/// Everything below must not move once bound, so it lives here.
var request_storage: Storage = undefined;
var reply_storage: Storage = undefined;
var request_ref: module.QueueRef = undefined;
var reply_ref: module.QueueRef = undefined;
var requests: ModuleQueue = undefined;
var replies: ModuleQueue = undefined;
var tx_message: [service.message_bytes]u8 = undefined;
var rx_message: [service.message_bytes]u8 = undefined;
var wire: rpc.QueueTransport = undefined;
var answers: Answers = .{};
var server: Server = undefined;
var rx: [Server.Env.max_frame]u8 = undefined;
var tx: [Server.Env.max_frame]u8 = undefined;

/// A ThreadX queue of this module's own, in its own memory.
fn createQueue(name: [*:0]const u8, storage: *Storage) *anyopaque {
    var object: ?*anyopaque = null;
    const allocated = _txm_module_object_allocate(&object, queue_block_bytes);
    if (allocated != tx_success) fail(Stage.allocate, allocated);
    const words = service.message_words;
    const created = _txe_queue_create(object.?, name, words, storage, @sizeOf(Storage), queue_block_bytes);
    if (created != tx_success) fail(Stage.create, created);
    return object.?;
}

/// Create the queues, hand them over, and bind the server to them.
fn connect() void {
    const dispatcher = _txm_module_kernel_call_dispatcher;
    request_ref = .{ .dispatcher = dispatcher, .queue = createQueue("server requests", &request_storage) };
    reply_ref = .{ .dispatcher = dispatcher, .queue = createQueue("server replies", &reply_storage) };
    report(service.Report.attach, @intFromPtr(request_ref.queue), @intFromPtr(reply_ref.queue));

    requests = ModuleQueue.init(request_ref.handle(), service.message_bytes) catch fail(Stage.bind, 0);
    replies = ModuleQueue.init(reply_ref.handle(), service.message_bytes) catch fail(Stage.bind, 1);
    wire = rpc.QueueTransport.init(replies.queue(), requests.queue(), &tx_message, &rx_message) catch
        fail(Stage.bind, 2);
    server = Server.init(wire.transport(), &rx, &answers, service.caps);
}

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    _ = id;
    connect();
    while (true) {
        while (true) {
            const step = server.poll(&tx) catch |err| fail(Stage.poll, @intFromError(err));
            if (step == .idle) break;
        }
        _ = _tx_thread_sleep(1);
    }
}
