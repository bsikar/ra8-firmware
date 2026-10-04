//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_wifi_start`, `ra8_c6link_wifi_stop` and `ra8_c6link_wifi_leave`
//! against a scripted RPC layer: the ids they send, the order, what the
//! WifiInit and SetWifiMode bodies carry, which error wins, and the guards in
//! front of them.

const std = @import("std");
const wifi = @import("wifi_abi");

const c = wifi.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
    pub const hw_timeout: u16 = 0x203;
    pub const protocol_error: u16 = 0x406;
};

const Script = struct {
    sent: [4]u32 = .{ 0, 0, 0, 0 },
    count: usize = 0,
    replies: [4]u16 = .{ 0, 0, 0, 0 },
};

/// What the coded requests carried, copied while they were still alive.
const Seen = struct {
    payload: [4]u32 = .{ 0, 0, 0, 0 },
    resp_id: [4]u32 = .{ 0, 0, 0, 0 },
    take_id: [4]u32 = .{ 0, 0, 0, 0 },
    take_fn: [4]usize = .{ 0, 0, 0, 0 },
    cfg: c.WifiInitConfig = std.mem.zeroes(c.WifiInitConfig),
    mode: i32 = -1,
};

var script: Script = .{};
var seen: Seen = .{};

fn record(req_id: u32) c.ra8_err_t {
    const at = script.count;
    script.sent[at] = req_id;
    script.count += 1;
    return script.replies[at];
}

export fn priv_c6link_bare_req(link: ?*c.ra8_c6link_t, req_id: u32) callconv(.c) c.ra8_err_t {
    _ = link;
    return record(req_id);
}

export fn rpc__init(message: ?*c.Rpc) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.Rpc);
}

export fn rpc__req__wifi_init__init(message: ?*c.RpcReqWifiInit) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqWifiInit);
}

export fn wifi_init_config__init(message: ?*c.WifiInitConfig) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.WifiInitConfig);
}

export fn rpc__req__set_mode__init(message: ?*c.RpcReqSetMode) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqSetMode);
}

export fn priv_c6link_take_resp(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    _ = ctx;
    _ = msg_v;
    return Code.ok;
}

export fn priv_c6link_rpc_call(link: ?*c.ra8_c6link_t, req: ?*c.Rpc, resp_id: u32, take: c.ra8_c6link_take_fn_t, ctx: ?*anyopaque) callconv(.c) c.ra8_err_t {
    _ = link;
    const msg = req.?;
    const at = script.count;
    seen.payload[at] = @intCast(msg.payload_case);
    seen.resp_id[at] = resp_id;
    seen.take_id[at] = @as(*c.ra8_c6link_take_ctx_t, @ptrCast(@alignCast(ctx.?))).rpc_id;
    seen.take_fn[at] = @intFromPtr(take.?);
    if (msg.payload_case == c.RPC__PAYLOAD_REQ_WIFI_INIT) seen.cfg = msg.unnamed_0.req_wifi_init.*.cfg.*;
    if (msg.payload_case == c.RPC__PAYLOAD_REQ_SET_WIFI_MODE) seen.mode = msg.unnamed_0.req_set_wifi_mode.*.mode;
    return record(@intCast(msg.msg_id));
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

test "stop sends WifiStop then WifiDeinit and returns ok" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.ok, wifi.ra8_c6link_wifi_stop(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
    try std.testing.expectEqual(wifi.Id.stop, script.sent[0]);
    try std.testing.expectEqual(wifi.Id.deinit, script.sent[1]);
}

test "stop still deinits when stop fails, and stop's error wins" {
    script = .{ .replies = .{ Code.hw_timeout, Code.protocol_error, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, wifi.ra8_c6link_wifi_stop(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
}

test "stop reports deinit's error when stop succeeded" {
    script = .{ .replies = .{ Code.ok, Code.protocol_error, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.protocol_error, wifi.ra8_c6link_wifi_stop(&link));
}

test "leave sends WifiDisconnect and passes its result on" {
    script = .{ .replies = .{ Code.hw_timeout, 0, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, wifi.ra8_c6link_wifi_leave(&link));
    try std.testing.expectEqual(@as(usize, 1), script.count);
    try std.testing.expectEqual(wifi.Id.disconnect, script.sent[0]);
}

test "a null or closed link sends nothing" {
    script = .{};
    var closed = std.mem.zeroes(c.ra8_c6link_t);
    try std.testing.expectEqual(Code.null_ptr, wifi.ra8_c6link_wifi_stop(null));
    try std.testing.expectEqual(Code.null_ptr, wifi.ra8_c6link_wifi_leave(null));
    try std.testing.expectEqual(Code.not_initialized, wifi.ra8_c6link_wifi_stop(&closed));
    try std.testing.expectEqual(Code.not_initialized, wifi.ra8_c6link_wifi_leave(&closed));
    try std.testing.expectEqual(@as(usize, 0), script.count);
}

test "start sends WifiInit, SetWifiMode, then WifiStart" {
    script = .{};
    seen = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.ok, wifi.ra8_c6link_wifi_start(&link));
    try std.testing.expectEqual(@as(usize, 3), script.count);
    try std.testing.expectEqual(wifi.Id.init, script.sent[0]);
    try std.testing.expectEqual(wifi.Id.set_mode, script.sent[1]);
    try std.testing.expectEqual(wifi.Id.start, script.sent[2]);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_WIFI_INIT), seen.payload[0]);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_SET_WIFI_MODE), seen.payload[1]);
    try std.testing.expectEqual(wifi.Id.init_resp, seen.resp_id[0]);
    try std.testing.expectEqual(wifi.Id.set_mode_resp, seen.resp_id[1]);
}

test "start hands the shared extractor the request id it is waiting on" {
    script = .{};
    seen = .{};
    var link = openLink();
    _ = wifi.ra8_c6link_wifi_start(&link);
    try std.testing.expectEqual(wifi.Id.init, seen.take_id[0]);
    try std.testing.expectEqual(wifi.Id.set_mode, seen.take_id[1]);
    try std.testing.expectEqual(@intFromPtr(&priv_c6link_take_resp), seen.take_fn[0]);
    try std.testing.expectEqual(@intFromPtr(&priv_c6link_take_resp), seen.take_fn[1]);
}

test "WifiInit carries every field of the wifi_init configuration" {
    script = .{};
    seen = .{};
    var link = openLink();
    _ = wifi.ra8_c6link_wifi_start(&link);
    const want = wifi.wifi_init.cfg();
    const got = seen.cfg;
    try std.testing.expectEqual(want.static_rx_buf_num, got.static_rx_buf_num);
    try std.testing.expectEqual(want.dynamic_rx_buf_num, got.dynamic_rx_buf_num);
    try std.testing.expectEqual(want.tx_buf_type, got.tx_buf_type);
    try std.testing.expectEqual(want.static_tx_buf_num, got.static_tx_buf_num);
    try std.testing.expectEqual(want.dynamic_tx_buf_num, got.dynamic_tx_buf_num);
    try std.testing.expectEqual(want.rx_mgmt_buf_type, got.rx_mgmt_buf_type);
    try std.testing.expectEqual(want.rx_mgmt_buf_num, got.rx_mgmt_buf_num);
    try std.testing.expectEqual(want.ampdu_rx_enable, got.ampdu_rx_enable);
    try std.testing.expectEqual(want.ampdu_tx_enable, got.ampdu_tx_enable);
    try std.testing.expectEqual(want.nvs_enable, got.nvs_enable);
    try std.testing.expectEqual(want.rx_ba_win, got.rx_ba_win);
    try std.testing.expectEqual(want.beacon_max_len, got.beacon_max_len);
    try std.testing.expectEqual(want.mgmt_sbuf_num, got.mgmt_sbuf_num);
    try std.testing.expectEqual(want.feature_caps, got.feature_caps);
    try std.testing.expectEqual(@intFromBool(want.sta_disconnected_pm != 0), got.sta_disconnected_pm);
    try std.testing.expectEqual(want.espnow_max_encrypt_num, got.espnow_max_encrypt_num);
    try std.testing.expectEqual(want.tx_hetb_queue_num, got.tx_hetb_queue_num);
    try std.testing.expectEqual(want.magic, got.magic);
}

test "SetWifiMode asks for station, the co-processor's mode 1" {
    script = .{};
    seen = .{};
    var link = openLink();
    _ = wifi.ra8_c6link_wifi_start(&link);
    try std.testing.expectEqual(@as(i32, 1), wifi.mode_sta);
    try std.testing.expectEqual(wifi.mode_sta, seen.mode);
}

test "start stops at the first failure" {
    script = .{ .replies = .{ Code.hw_timeout, 0, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, wifi.ra8_c6link_wifi_start(&link));
    try std.testing.expectEqual(@as(usize, 1), script.count);

    script = .{ .replies = .{ Code.ok, Code.protocol_error, 0, 0 } };
    try std.testing.expectEqual(Code.protocol_error, wifi.ra8_c6link_wifi_start(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);

    script = .{ .replies = .{ Code.ok, Code.ok, Code.hw_timeout, 0 } };
    try std.testing.expectEqual(Code.hw_timeout, wifi.ra8_c6link_wifi_start(&link));
    try std.testing.expectEqual(@as(usize, 3), script.count);
}

test "start on a null or closed link sends nothing" {
    script = .{};
    var closed = std.mem.zeroes(c.ra8_c6link_t);
    try std.testing.expectEqual(Code.null_ptr, wifi.ra8_c6link_wifi_start(null));
    try std.testing.expectEqual(Code.not_initialized, wifi.ra8_c6link_wifi_start(&closed));
    try std.testing.expectEqual(@as(usize, 0), script.count);
}
