//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The two things a shared-memory transport needs from the machine, injected
//! as a context and a vtable.
//!
//! Neither is implemented here. On hardware the barrier is a memory barrier
//! instruction and the notify is a write to whatever wakes the other core;
//! in a test they are whatever the test wants to observe.

pub const Signal = struct {
    ctx: *anyopaque,
    vtable: *const VTable,

    pub const VTable = struct {
        /// Order memory: every store made before this call is visible to the
        /// other core before any store made after it.
        barrier: *const fn (ctx: *anyopaque) void,
        /// Tell the other core there is something new to read. A hint: the
        /// other core must still find the data by reading the ring.
        notify: *const fn (ctx: *anyopaque) void,
    };

    pub fn barrier(self: Signal) void {
        self.vtable.barrier(self.ctx);
    }

    pub fn notify(self: Signal) void {
        self.vtable.notify(self.ctx);
    }
};
