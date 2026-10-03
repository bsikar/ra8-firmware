//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A barrier and a doorbell that do nothing but remember being used, and
//! what the ring they are watching looked like each time.

const std = @import("std");
const rpc = @import("ra8_rpc");
const Layout = rpc.ring.Layout;

pub const MockSignal = struct {
    /// The ring whose indices are noted on every call, if any.
    watch: ?[]const u8 = null,
    log: [capacity]Entry = undefined,
    count: usize = 0,
    notifies: usize = 0,
    barriers: usize = 0,

    const capacity = 16;

    pub const Kind = enum { barrier, notify };
    /// One call, and the watched ring's indices as they stood during it.
    pub const Entry = struct { kind: Kind, head: u32, tail: u32 };

    /// The signal points at this `MockSignal`, so it must not move.
    pub fn signal(self: *MockSignal) rpc.Signal {
        return .{ .ctx = self, .vtable = &vtable };
    }

    /// The calls since the last `clear`, oldest first.
    pub fn entries(self: *const MockSignal) []const Entry {
        return self.log[0..@min(self.count, capacity)];
    }

    pub fn clear(self: *MockSignal) void {
        self.count = 0;
    }

    const vtable: rpc.Signal.VTable = .{ .barrier = barrier, .notify = notify };

    fn note(ctx: *anyopaque, kind: Kind) *MockSignal {
        const self: *MockSignal = @ptrCast(@alignCast(ctx));
        if (self.count < capacity) self.log[self.count] = .{
            .kind = kind,
            .head = if (self.watch) |mem| word(mem, Layout.At.head) else 0,
            .tail = if (self.watch) |mem| word(mem, Layout.At.tail) else 0,
        };
        self.count += 1;
        return self;
    }

    fn word(mem: []const u8, offset: usize) u32 {
        return std.mem.readInt(u32, mem[offset..][0..4], .little);
    }

    fn barrier(ctx: *anyopaque) void {
        note(ctx, .barrier).barriers += 1;
    }

    fn notify(ctx: *anyopaque) void {
        note(ctx, .notify).notifies += 1;
    }
};
