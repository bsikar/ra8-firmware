//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/spi_b_events.zig (RA8FW-894).

const std = @import("std");
const events = @import("spi_b_events");

var calls: u32 = 0;
var last_mask: u8 = 0;
var last_ctx: ?*anyopaque = null;

fn record(ctx: ?*anyopaque, mask: u8) callconv(.c) void {
    calls += 1;
    last_mask = mask;
    last_ctx = ctx;
}

fn reset() void {
    calls = 0;
    last_mask = 0;
    last_ctx = null;
}

test "state mirrors ra8_spi_state_t" {
    try std.testing.expectEqual(@as(usize, 2 * @sizeOf(usize)), @offsetOf(events.State, "initialized"));
}

test "a non-zero mask reaches the attached handler with its ctx" {
    reset();
    var token: u8 = 0;
    events.report(.{ .cb = &record, .ctx = &token, .initialized = true }, 0x05);
    try std.testing.expectEqual(@as(u32, 1), calls);
    try std.testing.expectEqual(@as(u8, 0x05), last_mask);
    try std.testing.expectEqual(@as(?*anyopaque, &token), last_ctx);
}

test "an empty mask or a missing handler reports nothing" {
    reset();
    events.report(.{ .cb = &record, .ctx = null, .initialized = true }, 0);
    events.report(.{ .cb = null, .ctx = null, .initialized = true }, 0x08);
    try std.testing.expectEqual(@as(u32, 0), calls);
}
