//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/rtc_events.zig (RA8FW-852).

const std = @import("std");
const ev = @import("rtc_events");

var seen_mask: u8 = 0xFF;
var seen_ctx: ?*anyopaque = null;

fn record(ctx: ?*anyopaque, mask: u8) callconv(.c) void {
    seen_ctx = ctx;
    seen_mask = mask;
}

test "irq enable ors the low three bits; clear drops only those" {
    var r: u8 = 0x80;
    ev.setIrqEnable(&r, 0xFD);
    try std.testing.expectEqual(@as(u8, 0x85), r);
    try std.testing.expectEqual(@as(u8, 0x05), ev.status(&r));
    ev.clearStatus(&r, 0xF1);
    try std.testing.expectEqual(@as(u8, 0x84), r);
}

test "dispatch passes the masked status and context to the handler" {
    var r: u8 = 0xF6;
    var token: u32 = 1;
    const h = ev.Handler{ .func = record, .ctx = &token };
    ev.dispatch(&r, &h);
    try std.testing.expectEqual(@as(u8, 0x06), seen_mask);
    try std.testing.expectEqual(@as(?*anyopaque, &token), seen_ctx);
    seen_mask = 0xFF;
    ev.dispatch(&r, &ev.Handler{});
    try std.testing.expectEqual(@as(u8, 0xFF), seen_mask);
}

test "deinit zeroes RCR1 and RCR2 and drops the handler" {
    var r1: u8 = 0x07;
    var r2: u8 = 0x41;
    var token: u32 = 0;
    var h = ev.Handler{ .func = record, .ctx = &token };
    ev.deinit(&r1, &r2, &h);
    try std.testing.expectEqual(@as(u8, 0), r1 | r2);
    try std.testing.expect(h.func == null and h.ctx == null);
}
