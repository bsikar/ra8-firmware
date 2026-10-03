//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The pending table: a fixed number of calls in flight, and no more.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("service.zig");
}

test "each call gets its own id and gets its waiter back" {
    var table: rpc.Pending(3) = .{};
    const first = try table.add(100);
    const second = try table.add(200);
    try testing.expect(first != second);
    try testing.expectEqual(@as(usize, 2), table.count());

    try testing.expectEqual(@as(usize, 200), try table.take(second));
    try testing.expectEqual(@as(usize, 100), try table.take(first));
    try testing.expectEqual(@as(usize, 0), table.count());
}

test "a full table refuses the next call and keeps the ones it has" {
    var table: rpc.Pending(2) = .{};
    const first = try table.add(1);
    _ = try table.add(2);
    try testing.expectError(error.TableFull, table.add(3));
    try testing.expectEqual(@as(usize, 2), table.count());

    _ = try table.take(first);
    _ = try table.add(3);
}

test "an id that was never issued, or was already answered, is unknown" {
    var table: rpc.Pending(2) = .{};
    try testing.expectError(error.UnknownId, table.take(1));
    const id = try table.add(7);
    _ = try table.take(id);
    try testing.expectError(error.UnknownId, table.take(id));
}

test "when the counter wraps it steps over ids still in flight" {
    var table: rpc.Pending(3) = .{};
    table.next_id = std.math.maxInt(u32);
    const last = try table.add(1);
    const zero = try table.add(2);
    try testing.expectEqual(@as(u32, std.math.maxInt(u32)), last);
    try testing.expectEqual(@as(u32, 0), zero);

    table.next_id = last;
    const fresh = try table.add(3);
    try testing.expectEqual(@as(u32, 1), fresh);
    try testing.expectEqual(@as(usize, 3), try table.take(fresh));
}
