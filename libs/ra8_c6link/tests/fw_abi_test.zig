//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_fw_version` against a scripted RPC layer: the request it
//! builds, the fields it copies back, and the guards in front of both.

const std = @import("std");
const fw = @import("fw_abi");

const c = fw.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
};

const Script = struct {
    calls: usize = 0,
    req_msg_id: u32 = 0,
    req_payload: u32 = 0,
    resp_id: u32 = 0,
    fault_id: u32 = 0,
    fault_resp: i32 = 0,
    reply: c.RpcRespGetCoprocessorFwVersion = std.mem.zeroes(c.RpcRespGetCoprocessorFwVersion),
};

var script: Script = .{};

export fn rpc__init(message: ?*c.Rpc) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.Rpc);
}

export fn rpc__req__get_coprocessor_fw_version__init(message: ?*c.RpcReqGetCoprocessorFwVersion) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqGetCoprocessorFwVersion);
}

export fn priv_c6link_rpc_call(link: ?*c.ra8_c6link_t, req: ?*c.Rpc, resp_id: u32, take: c.ra8_c6link_take_fn_t, ctx: ?*anyopaque) callconv(.c) c.ra8_err_t {
    _ = link;
    script.calls += 1;
    script.req_msg_id = @intCast(req.?.msg_id);
    script.req_payload = @intCast(req.?.payload_case);
    script.resp_id = resp_id;
    var resp = std.mem.zeroes(c.Rpc);
    resp.unnamed_0.resp_get_coprocessor_fwversion = &script.reply;
    return take.?(ctx, &resp);
}

export fn priv_c6link_resp(link: ?*c.ra8_c6link_t, rpc_id: u32, resp: i32) callconv(.c) c.ra8_err_t {
    _ = link;
    script.fault_id = rpc_id;
    script.fault_resp = resp;
    return Code.ok;
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

test "fw_version rejects a null handle or out pointer" {
    script = .{};
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_fw_version_t);
    try std.testing.expectEqual(Code.null_ptr, fw.ra8_c6link_fw_version(null, &out));
    try std.testing.expectEqual(Code.null_ptr, fw.ra8_c6link_fw_version(&link, null));
    try std.testing.expectEqual(@as(usize, 0), script.calls);
}

test "fw_version refuses a closed link" {
    script = .{};
    var link = std.mem.zeroes(c.ra8_c6link_t);
    var out = std.mem.zeroes(c.ra8_c6link_fw_version_t);
    try std.testing.expectEqual(Code.not_initialized, fw.ra8_c6link_fw_version(&link, &out));
    try std.testing.expectEqual(@as(usize, 0), script.calls);
}

test "fw_version issues the request and copies the reply out" {
    script = .{};
    var target = "esp32c6".*;
    script.reply.resp = 0;
    script.reply.major1 = 2;
    script.reply.minor1 = 5;
    script.reply.patch1 = 11;
    script.reply.chip_id = 13;
    script.reply.idf_target = .{ .len = target.len, .data = &target };
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_fw_version_t);
    out.major = 99;
    try std.testing.expectEqual(Code.ok, fw.ra8_c6link_fw_version(&link, &out));
    try std.testing.expectEqual(fw.Id.request, script.req_msg_id);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_GET_COPROCESSOR_FWVERSION), script.req_payload);
    try std.testing.expectEqual(fw.Id.response, script.resp_id);
    try std.testing.expectEqual(fw.Id.request, script.fault_id);
    try std.testing.expectEqual(@as(u32, 2), out.major);
    try std.testing.expectEqual(@as(u32, 5), out.minor);
    try std.testing.expectEqual(@as(u32, 11), out.patch);
    try std.testing.expectEqual(@as(u32, 13), out.chip_id);
    try std.testing.expectEqual(@as(u8, 7), out.target_len);
    try std.testing.expectEqualStrings("esp32c6", out.target[0..out.target_len]);
    try std.testing.expectEqual(@as(u8, 0), out.target[out.target_len]);
}

test "fw_version passes the co-processor's verdict to the fault slot" {
    script = .{};
    script.reply.resp = -1;
    var link = openLink();
    var out = std.mem.zeroes(c.ra8_c6link_fw_version_t);
    _ = fw.ra8_c6link_fw_version(&link, &out);
    try std.testing.expectEqual(@as(i32, -1), script.fault_resp);
}
