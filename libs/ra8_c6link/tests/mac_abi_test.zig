//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_wifi_mac` against a scripted RPC layer: the request it builds,
//! the address it copies back, the failure paths, and the guards.

const std = @import("std");
const mac = @import("mac_abi");

const c = mac.c;

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
    req_mode: i32 = -1,
    resp_id: u32 = 0,
    fault_id: u32 = 0,
    fault_resp: i32 = 0,
    verdict: u16 = 0,
    no_body: bool = false,
    reply: c.RpcRespGetMacAddress = std.mem.zeroes(c.RpcRespGetMacAddress),
};

var script: Script = .{};

export fn rpc__init(message: ?*c.Rpc) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.Rpc);
}

export fn rpc__req__get_mac_address__init(message: ?*c.RpcReqGetMacAddress) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqGetMacAddress);
    message.?.mode = 99;
}

export fn priv_c6link_rpc_call(link: ?*c.ra8_c6link_t, req: ?*c.Rpc, resp_id: u32, take: c.ra8_c6link_take_fn_t, ctx: ?*anyopaque) callconv(.c) c.ra8_err_t {
    _ = link;
    script.calls += 1;
    script.req_msg_id = @intCast(req.?.msg_id);
    script.req_payload = @intCast(req.?.payload_case);
    script.req_mode = req.?.unnamed_0.req_get_mac_address.*.mode;
    script.resp_id = resp_id;
    var resp = std.mem.zeroes(c.Rpc);
    if (!script.no_body) resp.unnamed_0.resp_get_mac_address = &script.reply;
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

test "wifi_mac rejects a null handle or out pointer" {
    script = .{};
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_mac_t);
    try std.testing.expectEqual(Code.null_ptr, mac.ra8_c6link_wifi_mac(null, &out));
    try std.testing.expectEqual(Code.null_ptr, mac.ra8_c6link_wifi_mac(&link, null));
    try std.testing.expectEqual(@as(usize, 0), script.calls);
}

test "wifi_mac refuses a closed link" {
    script = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var out = std.mem.zeroes(c.ra8_c6link_mac_t);
    try std.testing.expectEqual(Code.not_initialized, mac.ra8_c6link_wifi_mac(&link, &out));
    try std.testing.expectEqual(@as(usize, 0), script.calls);
}

test "wifi_mac asks the station interface and copies the address out" {
    script = .{};
    var octets = [_]u8{ 0x24, 0x0A, 0xC4, 0x01, 0x02, 0x03 };
    script.reply.mac = .{ .len = octets.len, .data = &octets };
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_mac_t);
    try std.testing.expectEqual(Code.ok, mac.ra8_c6link_wifi_mac(&link, &out));
    try std.testing.expectEqual(mac.Id.request, script.req_msg_id);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_GET_MAC_ADDRESS), script.req_payload);
    try std.testing.expectEqual(@as(i32, 0), script.req_mode);
    try std.testing.expectEqual(mac.Id.response, script.resp_id);
    try std.testing.expectEqual(mac.Id.request, script.fault_id);
    try std.testing.expectEqualSlices(u8, &octets, out.octet[0..]);
}

test "wifi_mac returns the verdict and leaves the address cleared on failure" {
    script = .{ .verdict = Code.protocol_error };
    script.reply.resp = -1;
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_mac_t);
    out.octet[0] = 0xEE;
    try std.testing.expectEqual(Code.protocol_error, mac.ra8_c6link_wifi_mac(&link, &out));
    try std.testing.expectEqual(@as(i32, -1), script.fault_resp);
    try std.testing.expectEqual(@as(u8, 0), out.octet[0]);
}

test "wifi_mac reports protocol_error for a missing body or a short address" {
    script = .{ .no_body = true };
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_mac_t);
    try std.testing.expectEqual(Code.protocol_error, mac.ra8_c6link_wifi_mac(&link, &out));
    script = .{};
    var short = [_]u8{ 1, 2, 3 };
    script.reply.mac = .{ .len = short.len, .data = &short };
    try std.testing.expectEqual(Code.protocol_error, mac.ra8_c6link_wifi_mac(&link, &out));
    try std.testing.expectEqual(@as(u8, 0), out.octet[0]);
}
