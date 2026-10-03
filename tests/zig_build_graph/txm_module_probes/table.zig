//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A module start thread that only works if the addresses in its data are
//! right once it is loaded (RA8FW-534, RA8FW-539).
//!
//! It has the three things the spike asked about. A writable global. A
//! constant table of function pointers. And kernel calls through
//! ra8_rpc_tx's module Api, with the dispatcher read from the module's own
//! global: right here and only here, because this source reaches the
//! module through gcc.
//!
//! The table's two words are absolute addresses in initialised data, which
//! upstream's start-up copies and does not rebase. The module's rebase
//! table names both, and its start-up puts them right.
//!
//! Every tick it runs one function chosen from the table, sends the result
//! through a ThreadX queue it created and receives it back, both through a
//! `TxQueue` bound to the module Api, and reports what came back to the
//! resident image as an application request. txm_table_cpu1 checks the
//! values.

const std = @import("std");
const rpc_tx = @import("ra8_rpc_tx");
const module = rpc_tx.module;
const ModuleQueue = rpc_tx.TxQueue(module.api);

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
const message_words = 2;
const message_bytes = message_words * 4;
const queue_depth = 4;
const tx_success = 0;

/// What the module tells the resident image. A request at or above
/// `TXM_APPLICATION_REQUEST_ID_BASE` goes to the application, less the base.
const Report = struct {
    const base = 0x10000;
    /// A value went round: the value, the step it belongs to, and how many
    /// messages the queue then had free.
    const value: u32 = base + 1;
    /// Something failed: the stage, and the status or step.
    const failed: u32 = base + 2;
};

const Stage = struct {
    const allocate: u32 = 1;
    const create: u32 = 2;
    const bind: u32 = 3;
    const send: u32 = 4;
    const receive: u32 = 5;
};

/// Starts at five, so it is initialised data and not `.bss`.
var counter: u32 = 5;
var storage: [queue_depth * message_words]u32 = undefined;

const Op = struct { apply: *const fn (u32) callconv(.c) u32 };

fn double(x: u32) callconv(.c) u32 {
    return x *% 2;
}

fn square(x: u32) callconv(.c) u32 {
    return x *% x;
}

/// The shape of every vtable: constant, and made of function addresses.
const ops = [_]Op{ .{ .apply = double }, .{ .apply = square } };

fn fail(stage: u32, detail: u32) noreturn {
    _ = _txm_module_kernel_call_dispatcher(Report.failed, stage, detail, 0);
    while (true) _ = _tx_thread_sleep(100);
}

/// A ThreadX queue of this module's own, in its own memory.
fn createQueue() *anyopaque {
    var object: ?*anyopaque = null;
    const allocated = _txm_module_object_allocate(&object, queue_block_bytes);
    if (allocated != tx_success) fail(Stage.allocate, allocated);
    const created = _txe_queue_create(
        object.?,
        "table",
        message_words,
        &storage,
        @sizeOf(@TypeOf(storage)),
        queue_block_bytes,
    );
    if (created != tx_success) fail(Stage.create, created);
    return object.?;
}

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    _ = id;
    var ref: module.QueueRef = .{
        .dispatcher = _txm_module_kernel_call_dispatcher,
        .queue = createQueue(),
    };
    var bound = ModuleQueue.init(ref.handle(), message_bytes) catch fail(Stage.bind, 0);
    const queue = bound.queue();

    var step: u32 = 0;
    while (true) : (step +%= 1) {
        counter = ops[step & 1].apply(counter);
        var out: [message_bytes]u8 = undefined;
        std.mem.writeInt(u32, out[0..4], counter, .little);
        std.mem.writeInt(u32, out[4..8], step, .little);
        queue.send(&out) catch fail(Stage.send, step);

        var back: [message_bytes]u8 = undefined;
        const got = queue.receive(&back) catch fail(Stage.receive, step);
        if (!got) fail(Stage.receive, step);
        _ = _txm_module_kernel_call_dispatcher(
            Report.value,
            std.mem.readInt(u32, back[0..4], .little),
            std.mem.readInt(u32, back[4..8], .little),
            @intCast(queue.free()),
        );
        _ = _tx_thread_sleep(1);
    }
}
