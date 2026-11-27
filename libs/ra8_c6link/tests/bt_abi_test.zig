//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_c6link_bt_start` and `ra8_c6link_bt_stop` against a scripted RPC
//! layer: command sequence (`BT_Init` -> `BT_Enable`, `BT_Disable` ->
//! `BT_Deinit`), stale controller recovery on `invalid_state`, failure
//! precedence, and handle guards.

const std = @import("std");
const bt = @import("bt_abi");

const c = bt.c;

const Code = struct {
    pub const ok: u16 = 0;
    pub const invalid_state: u16 = 0x104;
    pub const not_initialized: u16 = 0x10F;
    pub const null_ptr: u16 = 0x504;
    pub const hw_timeout: u16 = 0x203;
    pub const protocol_error: u16 = 0x406;
};

const Script = struct {
    cmds: [8]u32 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    features: [8]u32 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    options: [8]u32 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    payloads: [8]u32 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    resp_ids: [8]u32 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    take_ids: [8]u32 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    take_fns: [8]usize = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    replies: [8]u16 = .{ 0, 0, 0, 0, 0, 0, 0, 0 },
    count: usize = 0,
};

var script: Script = .{};

export fn rpc__init(message: ?*c.Rpc) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.Rpc);
}

export fn rpc__req__feature_control__init(message: ?*c.RpcReqFeatureControl) callconv(.c) void {
    message.?.* = std.mem.zeroes(c.RpcReqFeatureControl);
}

export fn priv_c6link_take_resp(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    _ = ctx;
    _ = msg_v;
    return Code.ok;
}

export fn priv_c6link_rpc_call(
    link: ?*c.ra8_c6link_t,
    req: ?*c.Rpc,
    resp_id: u32,
    take: c.ra8_c6link_take_fn_t,
    ctx: ?*anyopaque,
) callconv(.c) c.ra8_err_t {
    _ = link;
    const msg = req.?;
    const at = script.count;
    script.count += 1;
    script.payloads[at] = @intCast(msg.payload_case);
    script.resp_ids[at] = resp_id;
    script.take_ids[at] = @as(*c.ra8_c6link_take_ctx_t, @ptrCast(@alignCast(ctx.?))).rpc_id;
    script.take_fns[at] = @intFromPtr(take.?);
    if (msg.unnamed_0.req_feature_control != null) {
        const body = msg.unnamed_0.req_feature_control;
        script.features[at] = @intCast(body.*.feature);
        script.cmds[at] = @intCast(body.*.command);
        script.options[at] = @intCast(body.*.option);
    }
    return script.replies[at];
}

fn openLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    return link;
}

test "bt_start sends Feature_Bluetooth BT_Init then BT_Enable" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.ok, bt.ra8_c6link_bt_start(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_FEATURE_CONTROL), script.payloads[0]);
    try std.testing.expectEqual(@as(u32, c.RPC__PAYLOAD_REQ_FEATURE_CONTROL), script.payloads[1]);
    try std.testing.expectEqual(bt.Id.resp, script.resp_ids[0]);
    try std.testing.expectEqual(bt.Id.resp, script.resp_ids[1]);
    try std.testing.expectEqual(bt.Id.req, script.take_ids[0]);
    try std.testing.expectEqual(@intFromPtr(&priv_c6link_take_resp), script.take_fns[0]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE__Feature_Bluetooth), script.features[0]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE__Feature_Bluetooth), script.features[1]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Init), script.cmds[0]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Enable), script.cmds[1]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_OPTION__Feature_Option_None), script.options[0]);
}

test "bt_start recovers from stale invalid_state by running BT_Disable and BT_Deinit" {
    script = .{ .replies = .{ Code.invalid_state, Code.protocol_error, Code.ok, Code.ok, Code.ok, 0, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.ok, bt.ra8_c6link_bt_start(&link));
    try std.testing.expectEqual(@as(usize, 5), script.count);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Init), script.cmds[0]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Disable), script.cmds[1]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Deinit), script.cmds[2]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Init), script.cmds[3]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Enable), script.cmds[4]);
}

test "bt_start stops on transport fault during stale recovery" {
    script = .{ .replies = .{ Code.invalid_state, Code.hw_timeout, 0, 0, 0, 0, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, bt.ra8_c6link_bt_start(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
}

test "bt_start returns failure from init or enable without recovery" {
    script = .{ .replies = .{ Code.hw_timeout, 0, 0, 0, 0, 0, 0, 0 } };
    var link = openLink();
    try std.testing.expectEqual(Code.hw_timeout, bt.ra8_c6link_bt_start(&link));
    try std.testing.expectEqual(@as(usize, 1), script.count);

    script = .{ .replies = .{ Code.ok, Code.protocol_error, 0, 0, 0, 0, 0, 0 } };
    try std.testing.expectEqual(Code.protocol_error, bt.ra8_c6link_bt_start(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
}

test "bt_stop sends BT_Disable then BT_Deinit and preserves first failure" {
    script = .{};
    var link = openLink();
    try std.testing.expectEqual(Code.ok, bt.ra8_c6link_bt_stop(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Disable), script.cmds[0]);
    try std.testing.expectEqual(@as(u32, c.RPC_FEATURE_COMMAND__Feature_Command_BT_Deinit), script.cmds[1]);

    script = .{ .replies = .{ Code.hw_timeout, Code.protocol_error, 0, 0, 0, 0, 0, 0 } };
    try std.testing.expectEqual(Code.hw_timeout, bt.ra8_c6link_bt_stop(&link));
    try std.testing.expectEqual(@as(usize, 2), script.count);

    script = .{ .replies = .{ Code.ok, Code.protocol_error, 0, 0, 0, 0, 0, 0 } };
    try std.testing.expectEqual(Code.protocol_error, bt.ra8_c6link_bt_stop(&link));
}

test "null or closed link sends nothing" {
    script = .{};
    var closed = std.mem.zeroes(c.ra8_c6link_t);
    try std.testing.expectEqual(Code.null_ptr, bt.ra8_c6link_bt_start(null));
    try std.testing.expectEqual(Code.null_ptr, bt.ra8_c6link_bt_stop(null));
    try std.testing.expectEqual(Code.not_initialized, bt.ra8_c6link_bt_start(&closed));
    try std.testing.expectEqual(Code.not_initialized, bt.ra8_c6link_bt_stop(&closed));
    try std.testing.expectEqual(@as(usize, 0), script.count);
}
