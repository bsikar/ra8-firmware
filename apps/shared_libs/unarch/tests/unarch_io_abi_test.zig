//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `unarch_io_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("unarch_io_abi");

const unarch_mem_read = sut.unarch_mem_read;
const unarch_mem_t = sut.unarch_mem_t;

const null_mem = unarch_mem_t{ .base = null, .len = 4 };
var live_mem = unarch_mem_t{ .base = null, .len = 0 };

fn stub_view(base: [*]const u8, len: usize) *const anyopaque {
    live_mem = .{ .base = base, .len = len };
    return @ptrCast(&live_mem);
}

test "null context, base, or destination read zero" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = undefined;
    const view = stub_view(&payload, payload.len);
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(null, 0, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(@ptrCast(&null_mem), 0, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, 0, null, 4));
}

test "reads at or past the end read zero (EOF)" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = undefined;
    const view = stub_view(&payload, payload.len);
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, payload.len, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, payload.len + 1, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, std.math.maxInt(u64), &out, 4));
}

test "reads clamp to the view's bounds" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = undefined;
    const view = stub_view(&payload, payload.len);
    // Full read, then an over-long tail read clamped to what is left.
    try std.testing.expectEqual(@as(usize, 8), unarch_mem_read(view, 0, &out, 8));
    try std.testing.expectEqualSlices(u8, &payload, out[0..8]);
    try std.testing.expectEqual(@as(usize, 5), unarch_mem_read(view, 3, &out, 16));
    try std.testing.expectEqualSlices(u8, payload[3..], out[0..5]);
}

test "zero-length request reads zero without touching anything" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = .{ 0, 0, 0, 0, 0, 0, 0, 0 };
    const view = stub_view(&payload, payload.len);
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, 0, &out, 0));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, payload.len, &out, 0));
    const zeros: [8]u8 = @splat(0);
    try std.testing.expectEqualSlices(u8, &zeros, &out);
}
