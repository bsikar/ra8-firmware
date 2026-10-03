//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_wifi_ap_info` against a scripted RPC layer: the request it
//! builds, the record it copies back, the failure paths, and the guards.

const std = @import("std");
const ap = @import("ap_info_abi");

const c = ap.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
    pub const protocol_error: u16 = 0x406;
};

const Script = struct {
    calls: usize = 0,
    req_msg_id: u32 = 0,
    req_payload: u32 = 0,
    resp_id: u32 = 0,
    fault_id: u32 = 0,
    fault_resp: i32 = 0,
    verdict: u16 = 0,
    no_body: bool = false,
    reply: c.RpcRespWifiStaGetApInfo = std.mem.zeroes(c.RpcRespWifiStaGetApInfo),
};

var script: Script = .{};

export fn rpc__init(message: ?*c.Rpc) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.Rpc);
}

export fn rpc__req__wifi_sta_get_ap_info__init(message: ?*c.RpcReqWifiStaGetApInfo) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqWifiStaGetApInfo);
}

export fn priv_c6link_rpc_call(link: ?*c.ra8_c6link_t, req: ?*c.Rpc, resp_id: u32, take: c.ra8_c6link_take_fn_t, ctx: ?*anyopaque) callconv(.c) c.ra8_err_t {
    _ = link;
    script.calls += 1;
    script.req_msg_id = @intCast(req.?.msg_id);
    script.req_payload = @intCast(req.?.payload_case);
    script.resp_id = resp_id;
    var resp = std.mem.zeroes(c.Rpc);
    if (!script.no_body) resp.unnamed_0.resp_wifi_sta_get_ap_info = &script.reply;
    return take.?(ctx, &resp);
}

export fn priv_c6link_resp(link: ?*c.ra8_c6link_t, rpc_id: u32, resp: i32) callconv(.c) c.ra8_err_t {
    _ = link;
    script.fault_id = rpc_id;
    script.fault_resp = resp;
    return script.verdict;
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

test "ap_info rejects a null handle or out pointer" {
    script = .{};
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_ap_info_t);
    try std.testing.expectEqual(Code.null_ptr, ap.ra8_c6link_wifi_ap_info(null, &out));
    try std.testing.expectEqual(Code.null_ptr, ap.ra8_c6link_wifi_ap_info(&link, null));
    try std.testing.expectEqual(@as(usize, 0), script.calls);
}

test "ap_info refuses a closed link" {
    script = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var out = std.mem.zeroes(c.ra8_c6link_ap_info_t);
    try std.testing.expectEqual(Code.not_initialized, ap.ra8_c6link_wifi_ap_info(&link, &out));
    try std.testing.expectEqual(@as(usize, 0), script.calls);
}

test "ap_info issues the request and copies the record out" {
    script = .{};
    var ssid = "lab-ap".*;
    var bssid = [_]u8{ 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };
    var rec = std.mem.zeroes(c.WifiApRecord);
    rec.ssid = .{ .len = ssid.len, .data = &ssid };
    rec.bssid = .{ .len = bssid.len, .data = &bssid };
    rec.primary = 11;
    rec.rssi = -47;
    rec.authmode = 3;
    script.reply.resp = 0;
    script.reply.ap_record = &rec;
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_ap_info_t);
    try std.testing.expectEqual(Code.ok, ap.ra8_c6link_wifi_ap_info(&link, &out));
    try std.testing.expectEqual(ap.Id.request, script.req_msg_id);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_WIFI_STA_GET_AP_INFO), script.req_payload);
    try std.testing.expectEqual(ap.Id.response, script.resp_id);
    try std.testing.expectEqual(ap.Id.request, script.fault_id);
    try std.testing.expectEqualStrings("lab-ap", out.ssid[0..out.ssid_len]);
    try std.testing.expectEqual(@as(u8, 0), out.ssid[out.ssid_len]);
    try std.testing.expectEqual(@as(u8, 11), out.channel);
    try std.testing.expectEqual(@as(i8, -47), out.rssi);
    try std.testing.expectEqual(@as(i32, 3), out.authmode);
    try std.testing.expectEqualSlices(u8, &bssid, out.bssid.octet[0..]);
}

test "ap_info returns the verdict and leaves the record cleared on failure" {
    script = .{ .verdict = Code.protocol_error };
    script.reply.resp = -1;
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_ap_info_t);
    out.channel = 9;
    try std.testing.expectEqual(Code.protocol_error, ap.ra8_c6link_wifi_ap_info(&link, &out));
    try std.testing.expectEqual(@as(i32, -1), script.fault_resp);
    try std.testing.expectEqual(@as(u8, 0), out.channel);
    try std.testing.expectEqual(@as(u8, 0), out.ssid_len);
}

test "ap_info reports protocol_error for a missing body or record" {
    script = .{ .no_body = true };
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_ap_info_t);
    try std.testing.expectEqual(Code.protocol_error, ap.ra8_c6link_wifi_ap_info(&link, &out));
    script = .{};
    try std.testing.expectEqual(Code.protocol_error, ap.ra8_c6link_wifi_ap_info(&link, &out));
    try std.testing.expectEqual(@as(usize, 1), script.calls);
}
