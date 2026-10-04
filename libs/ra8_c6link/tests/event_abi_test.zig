//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_rpc_event` against scripted `priv_c6link_emit`,
//! `priv_c6link_copy_str` and `priv_c6link_copy_mac`: each modelled
//! announcement emits one record carrying its own fields, a missing body
//! leaves the kind only, and anything else emits nothing.

const std = @import("std");
const event_abi = @import("event_abi");

const c = event_abi.c;

const Seen = struct {
    emits: usize = 0,
    link: ?*c.ra8_c6link_t = null,
    ev: c.ra8_c6link_event_t = std.mem.zeroes(c.ra8_c6link_event_t),
    str_cap: u8 = 0,
    macs: usize = 0,
};

var seen: Seen = .{};

export fn priv_c6link_emit(link: ?*c.ra8_c6link_t, ev: ?*const c.ra8_c6link_event_t) callconv(.c) void {
    seen.emits += 1;
    seen.link = link;
    seen.ev = ev.?.*;
}

export fn priv_c6link_copy_str(dst: [*c]u8, cap: u8, src: [*c]const c.ProtobufCBinaryData) callconv(.c) u8 {
    seen.str_cap = cap;
    const n: u8 = @intCast(@min(src.*.len, cap - 1));
    @memcpy(dst[0..n], src.*.data[0..n]);
    dst[n] = 0;
    return n;
}

export fn priv_c6link_copy_mac(dst: [*c]c.ra8_c6link_mac_t, src: [*c]const c.ProtobufCBinaryData) callconv(.c) bool {
    _ = src;
    seen.macs += 1;
    dst.* = std.mem.zeroes(c.ra8_c6link_mac_t);
    return true;
}

var ssid_bytes = "lab-ap".*;
var mac_bytes = [_]u8{ 1, 2, 3, 4, 5, 6 };

fn bin(bytes: []u8) c.ProtobufCBinaryData {
    return .{ .len = bytes.len, .data = bytes.ptr };
}

fn run(msg: *const c.Rpc) !c.ra8_c6link_event_t {
    seen = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    event_abi.priv_c6link_rpc_event(&link, msg);
    try std.testing.expectEqual(@as(usize, 1), seen.emits);
    try std.testing.expectEqual(@as(?*c.ra8_c6link_t, &link), seen.link);
    return seen.ev;
}

fn event(id: c_uint) c.Rpc {
    var msg = std.mem.zeroes(c.Rpc);
    msg.msg_id = @intCast(id);
    return msg;
}

test "boot carries the co-processor reset reason" {
    var body = std.mem.zeroes(c.RpcEventESPInit);
    body.cp_reset_reason = 7;
    var msg = event(c.RPC_ID__Event_ESPInit);
    msg.unnamed_0.event_esp_init = &body;
    const ev = try run(&msg);
    try std.testing.expectEqual(c.k_ra8_c6link_event_boot, ev.kind);
    try std.testing.expectEqual(@as(u32, 7), ev.reset_reason);
}

test "sta_connected names the AP, its channel and its address" {
    var inner = std.mem.zeroes(c.WifiEventStaConnected);
    inner.ssid = bin(&ssid_bytes);
    inner.bssid = bin(&mac_bytes);
    inner.channel = 11;
    var outer = std.mem.zeroes(c.RpcEventStaConnected);
    outer.sta_connected = &inner;
    var msg = event(c.RPC_ID__Event_StaConnected);
    msg.unnamed_0.event_sta_connected = &outer;
    const ev = try run(&msg);
    try std.testing.expectEqual(c.k_ra8_c6link_event_sta_connected, ev.kind);
    try std.testing.expectEqual(@as(u8, 6), ev.ssid_len);
    try std.testing.expectEqualStrings("lab-ap", std.mem.sliceTo(&ev.ssid, 0));
    try std.testing.expectEqual(@as(u8, @sizeOf(@FieldType(c.ra8_c6link_event_t, "ssid"))), seen.str_cap);
    try std.testing.expectEqual(@as(u8, 11), ev.channel);
    try std.testing.expectEqual(@as(usize, 1), seen.macs);
}

test "sta_disconnected carries the reason code and signal level" {
    var inner = std.mem.zeroes(c.WifiEventStaDisconnected);
    inner.ssid = bin(&ssid_bytes);
    inner.bssid = bin(&mac_bytes);
    inner.reason = 15;
    inner.rssi = -71;
    var outer = std.mem.zeroes(c.RpcEventStaDisconnected);
    outer.sta_disconnected = &inner;
    var msg = event(c.RPC_ID__Event_StaDisconnected);
    msg.unnamed_0.event_sta_disconnected = &outer;
    const ev = try run(&msg);
    try std.testing.expectEqual(c.k_ra8_c6link_event_sta_disconnected, ev.kind);
    try std.testing.expectEqual(@as(u16, 15), ev.reason);
    try std.testing.expectEqual(@as(i8, -71), ev.rssi);
    try std.testing.expectEqual(@as(u8, 6), ev.ssid_len);
    try std.testing.expectEqual(@as(usize, 1), seen.macs);
}

test "wifi reports the raw event id" {
    var body = std.mem.zeroes(c.RpcEventWifiEventNoArgs);
    body.event_id = 42;
    var msg = event(c.RPC_ID__Event_WifiEventNoArgs);
    msg.unnamed_0.event_wifi_event_no_args = &body;
    const ev = try run(&msg);
    try std.testing.expectEqual(c.k_ra8_c6link_event_wifi, ev.kind);
    try std.testing.expectEqual(@as(i32, 42), ev.wifi_event_id);
}

test "a missing body still emits the kind and nothing else" {
    const ids = [_]c_uint{ c.RPC_ID__Event_ESPInit, c.RPC_ID__Event_StaConnected, c.RPC_ID__Event_StaDisconnected, c.RPC_ID__Event_WifiEventNoArgs };
    for (ids) |id| {
        const msg = event(id);
        const ev = try run(&msg);
        var bare = std.mem.zeroes(c.ra8_c6link_event_t);
        bare.kind = ev.kind;
        try std.testing.expectEqual(bare, ev);
        try std.testing.expectEqual(@as(usize, 0), seen.macs);
    }
    var outer = std.mem.zeroes(c.RpcEventStaConnected);
    var msg = event(c.RPC_ID__Event_StaConnected);
    msg.unnamed_0.event_sta_connected = &outer;
    const ev = try run(&msg);
    try std.testing.expectEqual(@as(u8, 0), ev.ssid_len);
}

test "an unmodelled event, a null link or a null message emits nothing" {
    seen = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    const msg = event(c.RPC_ID__Event_Heartbeat);
    event_abi.priv_c6link_rpc_event(&link, &msg);
    event_abi.priv_c6link_rpc_event(null, &msg);
    event_abi.priv_c6link_rpc_event(&link, null);
    try std.testing.expectEqual(@as(usize, 0), seen.emits);
}
