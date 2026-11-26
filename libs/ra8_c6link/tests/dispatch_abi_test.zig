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
    hci_len: u16 = 0,
    hci_first: u8 = 0,
    hci_ctx: ?*anyopaque = null,
    emitted_count: usize = 0,
    emitted_kind: u8 = 0xFF,
};

var seen: Seen = .{};

export fn priv_c6link_rpc_consume(link: *c.ra8_c6link_t, payload: [*]const u8, len: u16) callconv(.c) bool {
    _ = link;
    seen.rpc_len = len;
    seen.rpc_first = payload[0];
    return true;
}

export fn priv_c6link_emit(link: *c.ra8_c6link_t, ev: *const c.ra8_c6link_event_t) callconv(.c) void {
    _ = link;
    seen.emitted_count += 1;
    seen.emitted_kind = ev.kind;
}

fn receive(ctx: ?*anyopaque, payload: [*c]const u8, len: u16) callconv(.c) void {
    seen.eth_ctx = ctx;
    seen.eth_len = len;
    seen.eth_first = payload[0];
}

fn receiveHci(ctx: ?*anyopaque, payload: [*c]const u8, len: u16) callconv(.c) void {
    seen.hci_ctx = ctx;
    seen.hci_len = len;
    seen.hci_first = payload[0];
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

test "an HCI frame reaches the HCI callback with its H4 type intact" {
    seen = .{};
    var link = newLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    var marker: u8 = 0;
    link.rx[12] = 0x04;
    link.hci_rx_cb = receiveHci;
    link.hci_ctx = &marker;
    link.stats = &stats;
    const view: RxView = .{ .offset = 12, .len = 8, .if_type = 4, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(u16, 8), seen.hci_len);
    try std.testing.expectEqual(@as(u8, 0x04), seen.hci_first);
    try std.testing.expectEqual(@as(?*anyopaque, &marker), seen.hci_ctx);
    try std.testing.expectEqual(@as(u16, 0), stats.unrouted);
    try std.testing.expectEqual(@as(u16, 0), stats.eth_in);
}

test "an HCI frame with no HCI callback is dropped without counting unrouted" {
    seen = .{};
    var link = newLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    link.rx_cb = receive;
    link.stats = &stats;
    const view: RxView = .{ .offset = 12, .len = 6, .if_type = 4, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(u16, 0), stats.unrouted);
    try std.testing.expectEqual(@as(u16, 0), seen.eth_len);
}

test "an unrouted interface counts unrouted and reaches nobody" {
    seen = .{};
    var link = newLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    link.rx_cb = receive;
    link.hci_rx_cb = receiveHci;
    link.stats = &stats;
    const view: RxView = .{ .offset = 12, .len = 4, .if_type = 6, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(u16, 1), stats.unrouted);
    try std.testing.expectEqual(@as(u16, 0), seen.eth_len);
    try std.testing.expectEqual(@as(u16, 0), seen.hci_len);
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

test "a valid ESP_PRIV_EVENT_INIT frame emits a boot event" {
    seen = .{};
    var link = newLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    link.stats = &stats;
    const init_payload = [_]u8{ 0x22, 3, 0x12, 1, 0x0D };
    @memcpy(link.rx[12..][0..init_payload.len], &init_payload);
    const view: RxView = .{ .offset = 12, .len = init_payload.len, .if_type = 5, .if_num = 1 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(usize, 1), seen.emitted_count);
    try std.testing.expectEqual(c.k_ra8_c6link_event_boot, seen.emitted_kind);
    try std.testing.expectEqual(@as(u16, 0), stats.unrouted);
}

test "a non-INIT privileged frame is counted as unrouted and emits nothing" {
    seen = .{};
    var link = newLink();
    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    link.stats = &stats;
    link.rx[12] = 0x23;
    link.rx[13] = 0x00;
    const view: RxView = .{ .offset = 12, .len = 2, .if_type = 5, .if_num = 0 };
    try std.testing.expect(!dispatch_abi.priv_c6link_dispatch(&link, &view));
    try std.testing.expectEqual(@as(usize, 0), seen.emitted_count);
    try std.testing.expectEqual(@as(u16, 1), stats.unrouted);
}
