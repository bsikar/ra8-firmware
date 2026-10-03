//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_emit` on a real `ra8_c6link_t`: the boot flag, the pump's
//! event counter and the application callback.

const std = @import("std");
const emit_abi = @import("emit_abi");

const c = emit_abi.c;

const Seen = struct {
    calls: usize = 0,
    ctx: ?*anyopaque = null,
    kind: u32 = 0xFFFF,
};

var seen: Seen = .{};

fn onEvent(ctx: ?*anyopaque, ev: [*c]const c.ra8_c6link_event_t) callconv(.c) void {
    seen.calls += 1;
    seen.ctx = ctx;
    seen.kind = @intCast(ev.*.kind);
}

fn event(kind: anytype) c.ra8_c6link_event_t {
    var ev = std.mem.zeroes(c.ra8_c6link_event_t);
    ev.kind = @intCast(kind);
    return ev;
}

test "a boot announcement marks the handle, counts and reaches the callback" {
    seen = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    var marker: u8 = 0;
    link.event_cb = onEvent;
    link.cb_ctx = &marker;
    link.stats = &stats;
    const ev = event(c.k_ra8_c6link_event_boot);
    emit_abi.priv_c6link_emit(&link, &ev);
    try std.testing.expect(link.boot_seen);
    try std.testing.expectEqual(@as(u16, 1), stats.events);
    try std.testing.expectEqual(@as(usize, 1), seen.calls);
    try std.testing.expectEqual(@as(?*anyopaque, &marker), seen.ctx);
    try std.testing.expectEqual(@as(u32, c.k_ra8_c6link_event_boot), seen.kind);
}

test "another announcement leaves the boot flag alone" {
    seen = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.event_cb = onEvent;
    const ev = event(c.k_ra8_c6link_event_boot + 1);
    emit_abi.priv_c6link_emit(&link, &ev);
    try std.testing.expect(!link.boot_seen);
    try std.testing.expectEqual(@as(usize, 1), seen.calls);
}

test "no callback, no pump, or a null argument is quiet" {
    seen = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    const ev = event(c.k_ra8_c6link_event_boot);
    emit_abi.priv_c6link_emit(&link, &ev);
    try std.testing.expect(link.boot_seen);

    link.boot_seen = false;
    link.event_cb = onEvent;
    emit_abi.priv_c6link_emit(null, &ev);
    emit_abi.priv_c6link_emit(&link, null);
    try std.testing.expect(!link.boot_seen);
    try std.testing.expectEqual(@as(usize, 0), seen.calls);
}
