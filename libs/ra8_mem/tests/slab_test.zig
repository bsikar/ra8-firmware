//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The slab's contract is the freelist: that every cell starts free, that
//! alloc and free are inverses, and that a pointer which did not come from
//! this slab is rejected rather than threaded into the list.

const std = @import("std");
const slab = @import("slab");

var pool: [256]u8 align(8) = undefined;

test "init divides the buffer and frees every cell" {
    var self: slab.Slab = .{};
    try std.testing.expectEqual(slab.Err.ok, slab.init(&self, &pool, 256, 32));
    try std.testing.expectEqual(@as(u32, 8), self.cell_count);
    try std.testing.expectEqual(@as(u32, 8), self.free_count);
    try std.testing.expectEqual(@as(u32, 0), self.free_head);

    // A buffer that does not divide evenly loses the tail, it does not fail.
    try std.testing.expectEqual(slab.Err.ok, slab.init(&self, &pool, 100, 32));
    try std.testing.expectEqual(@as(u32, 3), self.cell_count);
}

test "init rejects a cell that cannot hold a freelist index" {
    var self: slab.Slab = .{};
    try std.testing.expectEqual(slab.Err.invalid_size, slab.init(&self, &pool, 256, 3));
    try std.testing.expectEqual(slab.Err.invalid_size, slab.init(&self, &pool, 256, 0));
    // 6 is big enough but not 4-aligned, so a cell's link would straddle.
    try std.testing.expectEqual(slab.Err.invalid_size, slab.init(&self, &pool, 256, 6));
    // A cell larger than the buffer yields zero cells.
    try std.testing.expectEqual(slab.Err.invalid_size, slab.init(&self, &pool, 16, 32));
}

test "alloc walks the freelist and exhausts cleanly" {
    var self: slab.Slab = .{};
    _ = slab.init(&self, &pool, 256, 64);

    var cells: [4]?*anyopaque = .{ null, null, null, null };
    for (&cells) |*cell| {
        try std.testing.expectEqual(slab.Err.ok, slab.alloc(&self, cell));
    }
    try std.testing.expectEqual(@as(u32, 0), self.free_count);

    var overflow: ?*anyopaque = null;
    try std.testing.expectEqual(slab.Err.no_mem, slab.alloc(&self, &overflow));
    try std.testing.expectEqual(@as(?*anyopaque, null), overflow);

    // Every cell handed out is distinct and on a cell boundary.
    for (cells, 0..) |cell, i| {
        const off = @intFromPtr(cell.?) - @intFromPtr(&pool);
        try std.testing.expectEqual(@as(usize, 0), off % 64);
        for (cells[i + 1 ..]) |other| {
            try std.testing.expect(cell.? != other.?);
        }
    }
}

test "free returns a cell and the next alloc reuses it" {
    var self: slab.Slab = .{};
    _ = slab.init(&self, &pool, 256, 64);

    var first: ?*anyopaque = null;
    var second: ?*anyopaque = null;
    _ = slab.alloc(&self, &first);
    _ = slab.alloc(&self, &second);

    try std.testing.expectEqual(slab.Err.ok, slab.free(&self, first.?));
    try std.testing.expectEqual(@as(u32, 3), self.free_count);

    // The freelist is LIFO, so the cell just returned comes back first.
    var again: ?*anyopaque = null;
    _ = slab.alloc(&self, &again);
    try std.testing.expectEqual(first.?, again.?);
}

test "free rejects a pointer that is not one of this slab's cells" {
    var self: slab.Slab = .{};
    _ = slab.init(&self, &pool, 256, 64);

    var outside: [8]u8 align(8) = undefined;
    try std.testing.expectEqual(slab.Err.invalid_arg, slab.free(&self, &outside));

    // Past the last cell, and inside a cell but off its boundary.
    try std.testing.expectEqual(slab.Err.invalid_arg, slab.free(&self, &pool[256 - 1]));
    try std.testing.expectEqual(slab.Err.invalid_arg, slab.free(&self, &pool[8]));

    try std.testing.expectEqual(@as(u32, 4), self.free_count);
}

test "stats reports both counters and tolerates either being omitted" {
    var self: slab.Slab = .{};
    var free_cells: u32 = 0;
    var total: u32 = 0;

    // An unbound slab is a state error, not a zeroed report.
    try std.testing.expectEqual(slab.Err.invalid_state, slab.stats(&self, &free_cells, &total));

    _ = slab.init(&self, &pool, 256, 64);
    var one: ?*anyopaque = null;
    _ = slab.alloc(&self, &one);

    try std.testing.expectEqual(slab.Err.ok, slab.stats(&self, &free_cells, &total));
    try std.testing.expectEqual(@as(u32, 3), free_cells);
    try std.testing.expectEqual(@as(u32, 4), total);

    try std.testing.expectEqual(slab.Err.ok, slab.stats(&self, null, null));
}

test "a cell survives being written, freed and handed out again" {
    var self: slab.Slab = .{};
    _ = slab.init(&self, &pool, 256, 64);

    var cell: ?*anyopaque = null;
    _ = slab.alloc(&self, &cell);
    const bytes: [*]u8 = @ptrCast(cell.?);
    @memset(bytes[0..64], 0xA5);

    // Freeing overwrites the first four bytes with the link; the rest of the
    // payload is untouched, which is what makes the freelist free of cost.
    _ = slab.free(&self, cell.?);
    try std.testing.expectEqual(@as(u8, 0xA5), bytes[4]);
    try std.testing.expectEqual(@as(u8, 0xA5), bytes[63]);
}
