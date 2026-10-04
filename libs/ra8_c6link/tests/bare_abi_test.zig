//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_bare_req` against a scripted `priv_c6link_rpc_call`: for each
//! of the five empty-body requests, the message id, payload case, answer id,
//! extractor and its id; then the verdict pass-through and the guards.

const std = @import("std");
const bare = @import("bare_abi");

const c = bare.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const not_supported: u16 = 0x107;
    pub const null_ptr: u16 = 0x504;
    pub const hw_timeout: u16 = 0x203;
};

const Seen = struct {
    calls: usize = 0,
    msg_type: u32 = 0,
    msg_id: u32 = 0,
    payload: u32 = 0,
    body_set: bool = false,
    resp_id: u32 = 0,
    take_id: u32 = 0,
    take_fn: usize = 0,
    reply: u16 = 0,
};

var seen: Seen = .{};

export fn rpc__init(message: ?*c.Rpc) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.Rpc);
}

export fn rpc__req__wifi_start__init(message: ?*c.RpcReqWifiStart) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqWifiStart);
}

export fn rpc__req__wifi_stop__init(message: ?*c.RpcReqWifiStop) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqWifiStop);
}

export fn rpc__req__wifi_deinit__init(message: ?*c.RpcReqWifiDeinit) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqWifiDeinit);
}

export fn rpc__req__wifi_connect__init(message: ?*c.RpcReqWifiConnect) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqWifiConnect);
}

export fn rpc__req__wifi_disconnect__init(message: ?*c.RpcReqWifiDisconnect) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqWifiDisconnect);
}

export fn priv_c6link_take_resp(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    _ = ctx;
    _ = msg_v;
    return Code.ok;
}

/// Is the union arm for this payload case set? Each case reads its own arm.
fn bodySet(msg: *const c.Rpc) bool {
    return switch (msg.payload_case) {
        c.RPC__PAYLOAD_REQ_WIFI_START => msg.unnamed_0.req_wifi_start != null,
        c.RPC__PAYLOAD_REQ_WIFI_STOP => msg.unnamed_0.req_wifi_stop != null,
        c.RPC__PAYLOAD_REQ_WIFI_DEINIT => msg.unnamed_0.req_wifi_deinit != null,
        c.RPC__PAYLOAD_REQ_WIFI_CONNECT => msg.unnamed_0.req_wifi_connect != null,
        c.RPC__PAYLOAD_REQ_WIFI_DISCONNECT => msg.unnamed_0.req_wifi_disconnect != null,
        else => false,
    };
}

export fn priv_c6link_rpc_call(link: ?*c.ra8_c6link_t, req: ?*c.Rpc, resp_id: u32, take: c.ra8_c6link_take_fn_t, ctx: ?*anyopaque) callconv(.c) c.ra8_err_t {
    _ = link;
    const msg = req.?;
    seen.calls += 1;
    seen.msg_type = @intCast(msg.msg_type);
    seen.msg_id = @intCast(msg.msg_id);
    seen.payload = @intCast(msg.payload_case);
    seen.body_set = bodySet(msg);
    seen.resp_id = resp_id;
    seen.take_id = @as(*c.ra8_c6link_take_ctx_t, @ptrCast(@alignCast(ctx.?))).rpc_id;
    seen.take_fn = @intFromPtr(take.?);
    return seen.reply;
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

const Case = struct { req: u32, resp: u32, payload: u32 };

const cases = [_]Case{
    .{ .req = c.RPC_ID__Req_WifiStart, .resp = c.RPC_ID__Resp_WifiStart, .payload = c.RPC__PAYLOAD_REQ_WIFI_START },
    .{ .req = c.RPC_ID__Req_WifiStop, .resp = c.RPC_ID__Resp_WifiStop, .payload = c.RPC__PAYLOAD_REQ_WIFI_STOP },
    .{ .req = c.RPC_ID__Req_WifiDeinit, .resp = c.RPC_ID__Resp_WifiDeinit, .payload = c.RPC__PAYLOAD_REQ_WIFI_DEINIT },
    .{ .req = c.RPC_ID__Req_WifiConnect, .resp = c.RPC_ID__Resp_WifiConnect, .payload = c.RPC__PAYLOAD_REQ_WIFI_CONNECT },
    .{ .req = c.RPC_ID__Req_WifiDisconnect, .resp = c.RPC_ID__Resp_WifiDisconnect, .payload = c.RPC__PAYLOAD_REQ_WIFI_DISCONNECT },
};

test "each bare request goes out with its id, payload, answer id and extractor" {
    var link = openLink();
    for (cases) |case| {
        seen = .{};
        try std.testing.expectEqual(Code.ok, bare.priv_c6link_bare_req(&link, case.req));
        try std.testing.expectEqual(@as(usize, 1), seen.calls);
        try std.testing.expectEqual(@as(u32, c.RPC_TYPE__Req), seen.msg_type);
        try std.testing.expectEqual(case.req, seen.msg_id);
        try std.testing.expectEqual(case.payload, seen.payload);
        try std.testing.expect(seen.body_set);
        try std.testing.expectEqual(case.resp, seen.resp_id);
        try std.testing.expectEqual(case.req, seen.take_id);
        try std.testing.expectEqual(@intFromPtr(&priv_c6link_take_resp), seen.take_fn);
    }
}

test "the RPC layer's verdict is passed through" {
    var link = openLink();
    seen = .{ .reply = Code.hw_timeout };
    try std.testing.expectEqual(Code.hw_timeout, bare.priv_c6link_bare_req(&link, c.RPC_ID__Req_WifiStop));
}

test "an id that is not a bare request is not supported and sends nothing" {
    var link = openLink();
    seen = .{};
    try std.testing.expectEqual(Code.not_supported, bare.priv_c6link_bare_req(&link, c.RPC_ID__Req_WifiInit));
    try std.testing.expectEqual(Code.not_supported, bare.priv_c6link_bare_req(&link, 0));
    try std.testing.expectEqual(@as(usize, 0), seen.calls);
}

test "a null link is null_ptr and sends nothing" {
    seen = .{};
    try std.testing.expectEqual(Code.null_ptr, bare.priv_c6link_bare_req(null, c.RPC_ID__Req_WifiStart));
    try std.testing.expectEqual(@as(usize, 0), seen.calls);
}
