//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_events.zig (RA8FW-847).

const std = @import("std");
const ev = @import("eth_gwca_events");

var seen_mask: u32 = 0;
var seen_ctx: ?*anyopaque = null;
var calls: u32 = 0;

fn record(ctx: ?*anyopaque, mask: u32) callconv(.c) void {
    seen_ctx = ctx;
    seen_mask = mask;
    calls += 1;
}

test "clearStatus writes ICLR and drops only the masked STS bits" {
    var r = ev.Regs{ .ctrl = 0, .sts = 0b1011, .ie = 0, .iclr = 0 };
    ev.clearStatus(&r, 0b0011);
    try std.testing.expectEqual(@as(u32, 0b0011), r.iclr);
    try std.testing.expectEqual(@as(u32, 0b1000), r.sts);
}

test "dispatch acknowledges the snapshot and hands it to the handler" {
    var r = ev.Regs{ .ctrl = 0, .sts = 0x55, .ie = 0, .iclr = 0 };
    var token: u8 = 7;
    calls = 0;
    ev.dispatch(&r, record, &token);
    try std.testing.expectEqual(@as(u32, 1), calls);
    try std.testing.expectEqual(@as(u32, 0x55), seen_mask);
    try std.testing.expectEqual(@as(?*anyopaque, &token), seen_ctx);
    try std.testing.expectEqual(@as(u32, 0x55), r.iclr);
    try std.testing.expectEqual(@as(u32, 0), r.sts);
}

test "dispatch with no handler still acknowledges" {
    var r = ev.Regs{ .ctrl = 0, .sts = 0x3, .ie = 0, .iclr = 0 };
    calls = 0;
    ev.dispatch(&r, null, null);
    try std.testing.expectEqual(@as(u32, 0), calls);
    try std.testing.expectEqual(@as(u32, 0x3), r.iclr);
    try std.testing.expectEqual(@as(u32, 0), r.sts);
}
