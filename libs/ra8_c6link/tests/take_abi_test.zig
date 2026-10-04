//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_take_resp` against a scripted `priv_c6link_resp`: each of the
//! eight Wi-Fi answers reports its own body's `resp` under the request id; a
//! missing body or a foreign answer reports -1; the result is passed through.

const std = @import("std");
const take_abi = @import("take_abi");

const c = take_abi.c;

const Seen = struct {
    calls: usize = 0,
    link: ?*c.ra8_c6link_t = null,
    rpc_id: u32 = 0,
    resp: i32 = 0,
    reply: u16 = 0,
};

var seen: Seen = .{};

export fn priv_c6link_resp(link: ?*c.ra8_c6link_t, rpc_id: u32, resp: i32) callconv(.c) c.ra8_err_t {
    seen.calls += 1;
    seen.link = link;
    seen.rpc_id = rpc_id;
    seen.resp = resp;
    return seen.reply;
}

/// Every Wi-Fi answer body has the same first field, so one generic body
/// stands in for all eight in turn.
fn expectReported(comptime T: type, comptime field: []const u8, resp_id: u32) !void {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var body = std.mem.zeroes(T);
    body.resp = 0x3009;
    var msg = std.mem.zeroes(c.Rpc);
    msg.msg_id = @intCast(resp_id);
    @field(msg.unnamed_0, field) = &body;
    var take = c.ra8_c6link_take_ctx_t{ .link = &link, .out = null, .rpc_id = 4242 };

    seen = .{};
    try std.testing.expectEqual(@as(u16, 0), take_abi.priv_c6link_take_resp(&take, &msg));
    try std.testing.expectEqual(@as(usize, 1), seen.calls);
    try std.testing.expectEqual(@as(?*c.ra8_c6link_t, &link), seen.link);
    try std.testing.expectEqual(@as(u32, 4242), seen.rpc_id);
    try std.testing.expectEqual(@as(i32, 0x3009), seen.resp);

    @field(msg.unnamed_0, field) = null;
    seen = .{};
    _ = take_abi.priv_c6link_take_resp(&take, &msg);
    try std.testing.expectEqual(@as(i32, -1), seen.resp);
}

test "each Wi-Fi answer reports its own body's verdict, -1 without a body" {
    try expectReported(c.RpcRespWifiInit, "resp_wifi_init", c.RPC_ID__Resp_WifiInit);
    try expectReported(c.RpcRespSetMode, "resp_set_wifi_mode", c.RPC_ID__Resp_SetWifiMode);
    try expectReported(c.RpcRespWifiSetConfig, "resp_wifi_set_config", c.RPC_ID__Resp_WifiSetConfig);
    try expectReported(c.RpcRespWifiStart, "resp_wifi_start", c.RPC_ID__Resp_WifiStart);
    try expectReported(c.RpcRespWifiStop, "resp_wifi_stop", c.RPC_ID__Resp_WifiStop);
    try expectReported(c.RpcRespWifiDeinit, "resp_wifi_deinit", c.RPC_ID__Resp_WifiDeinit);
    try expectReported(c.RpcRespWifiConnect, "resp_wifi_connect", c.RPC_ID__Resp_WifiConnect);
    try expectReported(c.RpcRespWifiDisconnect, "resp_wifi_disconnect", c.RPC_ID__Resp_WifiDisconnect);
}

test "an answer that is not a Wi-Fi answer reports -1" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var msg = std.mem.zeroes(c.Rpc);
    msg.msg_id = c.RPC_ID__Resp_GetMACAddress;
    var take = c.ra8_c6link_take_ctx_t{ .link = &link, .out = null, .rpc_id = 7 };
    seen = .{};
    _ = take_abi.priv_c6link_take_resp(&take, &msg);
    try std.testing.expectEqual(@as(i32, -1), seen.resp);
    try std.testing.expectEqual(@as(u32, 7), seen.rpc_id);
}

test "priv_c6link_resp's result is passed through" {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var body = std.mem.zeroes(c.RpcRespWifiStop);
    var msg = std.mem.zeroes(c.Rpc);
    msg.msg_id = c.RPC_ID__Resp_WifiStop;
    msg.unnamed_0.resp_wifi_stop = &body;
    var take = c.ra8_c6link_take_ctx_t{ .link = &link, .out = null, .rpc_id = 1 };
    seen = .{ .reply = 0x406 };
    try std.testing.expectEqual(@as(u16, 0x406), take_abi.priv_c6link_take_resp(&take, &msg));
}
