//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A stand-in for the module manager's side of the kernel-call dispatcher.
//!
//! It writes down every request it is given, then does what the manager
//! does: unpacks the words and calls the service, here the fake one. A real
//! dispatcher takes no context, so neither does this, and what it records is
//! file-level state that a test clears before it starts.

const std = @import("std");
const rpc_tx = @import("ra8_rpc_tx");
const tx = rpc_tx.api;
const module = rpc_tx.module;
const fake = @import("fake_threadx.zig");

/// What the manager answers for a request it does not serve:
/// `TX_NOT_AVAILABLE`.
pub const not_available: tx.Uint = 0x1D;

/// One request as it arrived.
pub const Call = struct { request: tx.Ulong, params: [3]tx.Ulong };

const capacity = 64;
var log: [capacity]Call = undefined;
var count: usize = 0;

/// The `extra` words of the last `info_get`, copied while they were live.
pub var last_extra: [module.Extra.words]tx.Ulong = undefined;

pub fn clear() void {
    count = 0;
}

/// Every request since the last `clear`, oldest first.
pub fn calls() []const Call {
    return log[0..@min(count, capacity)];
}

pub fn last() Call {
    return log[count - 1];
}

fn pointer(comptime T: type, word: tx.Ulong) T {
    return @ptrFromInt(@as(usize, @intCast(word)));
}

pub fn dispatch(
    request: tx.Ulong,
    param_1: tx.Ulong,
    param_2: tx.Ulong,
    param_3: tx.Ulong,
) callconv(.c) tx.Ulong {
    if (count < capacity) log[count] = .{
        .request = request,
        .params = .{ param_1, param_2, param_3 },
    };
    count += 1;

    const queue = pointer(tx.Handle, param_1);
    switch (request) {
        module.Request.queue_send => {
            return fake.api.send(queue, pointer(*anyopaque, param_2), param_3);
        },
        module.Request.queue_receive => {
            return fake.api.receive(queue, pointer(*anyopaque, param_2), param_3);
        },
        module.Request.queue_info_get => {
            const extra = pointer(*const [module.Extra.words]tx.Ulong, param_3);
            last_extra = extra.*;
            return fake.api.info_get(
                queue,
                pointer(?*?[*:0]u8, param_2),
                pointer(?*tx.Ulong, extra[module.Extra.enqueued]),
                pointer(?*tx.Ulong, extra[module.Extra.available_storage]),
                pointer(?*?*anyopaque, extra[module.Extra.first_suspended]),
                pointer(?*tx.Ulong, extra[module.Extra.suspended_count]),
                pointer(?*?*anyopaque, extra[module.Extra.next_queue]),
            );
        },
        else => return not_available,
    }
}
