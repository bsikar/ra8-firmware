//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A module start thread with the three things RA8FW-534 asked about: a
//! writable global, a constant table of function pointers, and a kernel
//! call through ra8_rpc_tx's module Api, with the dispatcher read from the
//! module's own global. Reading that global from Zig is right here and only
//! here: this source reaches the module through gcc.
//!
//! The table is the part the module model cannot carry yet. Its two words
//! are absolute addresses in initialised data, which nothing rebases at
//! load, so tools/check_txm_module_relocs must refuse the linked module and
//! name both.

const rpc_tx = @import("ra8_rpc_tx");
const module = rpc_tx.module;
const tx = rpc_tx.api;

/// Set by the module's thread shell entry before this thread runs.
extern var _txm_module_kernel_call_dispatcher: module.Dispatcher;
extern fn _tx_thread_sleep(timer_ticks: u32) u32;

/// Starts at five, so it is initialised data and not `.bss`.
var counter: u32 = 5;

const Op = struct { apply: *const fn (u32) callconv(.c) u32 };

fn double(x: u32) callconv(.c) u32 {
    return x *% 2;
}

fn square(x: u32) callconv(.c) u32 {
    return x *% x;
}

/// The shape of every vtable: constant, and made of function addresses.
const ops = [_]Op{ .{ .apply = double }, .{ .apply = square } };

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    var which = id;
    while (true) : (which +%= 1) {
        counter = ops[which & 1].apply(counter);
        var ref: module.QueueRef = .{
            .dispatcher = _txm_module_kernel_call_dispatcher,
            // No queue exists yet; the manager answers with an error, and
            // the call is still made through the dispatcher.
            .queue = &counter,
        };
        var waiting: tx.Ulong = 0;
        var free: tx.Ulong = 0;
        _ = module.api.info_get(ref.handle(), null, &waiting, &free, null, null, null);
        _ = _tx_thread_sleep(1);
    }
}
