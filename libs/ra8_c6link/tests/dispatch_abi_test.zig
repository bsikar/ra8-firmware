//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_dispatch` on a real `ra8_c6link_t`: the RPC, Ethernet and
//! counted routes, a view that runs off the buffer, and null arguments. The C
//! RPC decoder is stood in for by an export below.

const std = @import("std");
const dispatch_abi = @import("dispatch_abi");

const c = dispatch_abi.c;
const RxView = dispatch_abi.RxView;

/// What the stand-in decoder and receive callback saw.
const Seen = struct {
    rpc_len: u16 = 0,
    rpc_first: u8 = 0,
    eth_len: u16 = 0,
    eth_first: u8 = 0,
    eth_ctx: ?*anyopaque = null,
};

var seen: Seen = .{};

export fn priv_c6link_rpc_consume(link: *c.ra8_c6link_t, payload: [*]const u8, len: u16) callconv(.c) bool {
    _ = link;
    seen.rpc_len = len;
    seen.rpc_first = payload[0];
    return true;
}

fn receive(ctx: ?*anyopaque, payload: [*c]const u8, len: u16) callconv(.c) void {
    seen.eth_ctx = ctx;
    seen.eth_len = len;
    seen.eth_first = payload[0];
}

fn newLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.rx[12] = 0x7E;
    return link;
}

test "a serial frame goes to the RPC decoder and its verdict comes back" {
    seen = .{};
    var link = newLink();
    const view: RxView = .{ .offset = 12, .len = 30, .if_type = 3, .if_num = 0 };
    try std.testing.expect(dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(u16, 30), seen.rpc_len);
    try std.testing.expectEqual(@as(u8, 0x7E), seen.rpc_first);
}

test "a station frame reaches the receive callback and counts eth_in" {
    seen = .{};
    var link = newLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    var marker: u8 = 0;
    link.rx_cb = receive;
    link.cb_ctx = &marker;
    link.stats = &stats;
    const view: RxView = .{ .offset = 12, .len = 64, .if_type = 1, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(u16, 64), seen.eth_len);
    try std.testing.expectEqual(@as(u8, 0x7E), seen.eth_first);
    try std.testing.expectEqual(@as(?*anyopaque, &marker), seen.eth_ctx);
    try std.testing.expectEqual(@as(u16, 1), stats.eth_in);
}

test "an Ethernet frame with no callback or counters is dropped quietly" {
    var link = newLink();
    const view: RxView = .{ .offset = 12, .len = 64, .if_type = 2, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
}

test "an unrouted interface counts unrouted and reaches nobody" {
    seen = .{};
    var link = newLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    link.rx_cb = receive;
    link.stats = &stats;
    const view: RxView = .{ .offset = 12, .len = 4, .if_type = 4, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(u16, 1), stats.unrouted);
    try std.testing.expectEqual(@as(u16, 0), seen.eth_len);
    try std.testing.expectEqual(@as(u16, 0), seen.rpc_len);
}

test "a view past the end of the buffer, or a null argument, routes nothing" {
    seen = .{};
    var link = newLink();
    const view: RxView = .{ .offset = 1590, .len = 20, .if_type = 3, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(u16, 0), seen.rpc_len);
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(null, &view));
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, null));
}
