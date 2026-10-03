//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `table.zig` without the table: the same writable global and the same
//! kernel call through ra8_rpc_tx's module Api, and the two functions
//! chosen by a branch instead of looked up. Nothing in its data holds an
//! address, so tools/check_txm_module_relocs must pass the linked module.

const rpc_tx = @import("ra8_rpc_tx");
const module = rpc_tx.module;
const tx = rpc_tx.api;

/// Set by the module's thread shell entry before this thread runs.
extern var _txm_module_kernel_call_dispatcher: module.Dispatcher;
extern fn _tx_thread_sleep(timer_ticks: u32) u32;

/// Starts at five, so it is initialised data and not `.bss`.
var counter: u32 = 5;

fn double(x: u32) u32 {
    return x *% 2;
}

fn square(x: u32) u32 {
    return x *% x;
}

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    var which = id;
    while (true) : (which +%= 1) {
        counter = if (which & 1 == 0) double(counter) else square(counter);
        var ref: module.QueueRef = .{
            .dispatcher = _txm_module_kernel_call_dispatcher,
            .queue = &counter,
        };
        var waiting: tx.Ulong = 0;
        var free: tx.Ulong = 0;
        _ = module.api.info_get(ref.handle(), null, &waiting, &free, null, null, null);
        _ = _tx_thread_sleep(1);
    }
}
