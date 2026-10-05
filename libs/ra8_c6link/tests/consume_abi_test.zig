//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_rpc_consume` against scripted envelope, arena, codec,
//! correlation and event hooks: every path counts what it should, routes
//! the message once, and leaves the arena reset and the message freed.

const std = @import("std");
const consume_abi = @import("consume_abi");

const c = consume_abi.c;

const Script = struct {
    envelope_ok: bool = true,
    decodes: bool = true,
    answers: bool = false,
    msg: c.Rpc = std.mem.zeroes(c.Rpc),
};

const Seen = struct {
    binds: usize = 0,
    resets: usize = 0,
    frees: usize = 0,
    events: usize = 0,
    takes: usize = 0,
    proto_len: usize = 0,
    answers_args: [5]u32 = .{ 0, 0, 0, 0, 0 },
};

var script: Script = .{};
var seen: Seen = .{};
var body = [_]u8{ 0xAA, 0xBB, 0xCC };

export fn priv_c6link_tlv_body(payload: [*c]const u8, len: u16, proto_len: [*c]u16) callconv(.c) [*c]const u8 {
    _ = payload;
    _ = len;
    if (!script.envelope_ok) return null;
    proto_len.* = body.len;
    return &body;
}

export fn priv_c6link_arena_bind(out: [*c]c.ProtobufCAllocator, link: ?*c.ra8_c6link_t) callconv(.c) void {
    _ = out;
    _ = link;
    seen.binds += 1;
}

export fn priv_c6link_arena_reset(link: ?*c.ra8_c6link_t) callconv(.c) void {
    _ = link;
    seen.resets += 1;
}

export fn rpc__unpack(alloc: [*c]c.ProtobufCAllocator, len: usize, data: [*c]const u8) callconv(.c) [*c]c.Rpc {
    _ = alloc;
    _ = data;
    seen.proto_len = len;
    return if (script.decodes) &script.msg else null;
}

export fn rpc__free_unpacked(msg: [*c]c.Rpc, alloc: [*c]c.ProtobufCAllocator) callconv(.c) void {
    _ = msg;
    _ = alloc;
    seen.frees += 1;
}

export fn priv_c6link_rpc_answers(armed: bool, wait_uid: u32, wait_resp_id: u32, msg_uid: u32, msg_id: u32) callconv(.c) bool {
    seen.answers_args = .{ @intFromBool(armed), wait_uid, wait_resp_id, msg_uid, msg_id };
    return script.answers;
}

export fn priv_c6link_rpc_event(link: ?*c.ra8_c6link_t, msg_v: ?*const anyopaque) callconv(.c) void {
    _ = link;
    _ = msg_v;
    seen.events += 1;
}

fn take(ctx: ?*anyopaque, msg_v: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    _ = ctx;
    _ = msg_v;
    seen.takes += 1;
    return 0x1234;
}

const Rig = struct {
    link: c.ra8_c6link_t = std.mem.zeroes(c.ra8_c6link_t),
    stats: c.ra8_c6link_stats_t = std.mem.zeroes(c.ra8_c6link_stats_t),
};

fn consume(rig: *Rig) bool {
    seen = .{};
    rig.link.stats = &rig.stats;
    const payload = [_]u8{0} ** 8;
    return consume_abi.priv_c6link_rpc_consume(&rig.link, &payload, payload.len);
}

fn reset(msg_type: c_uint) void {
    script = .{};
    script.msg.msg_type = @intCast(msg_type);
}

fn expectCleanedUp() !void {
    try std.testing.expectEqual(@as(usize, 1), seen.binds);
    try std.testing.expectEqual(@as(usize, 2), seen.resets);
    try std.testing.expectEqual(@as(usize, 1), seen.frees);
}

test "an event is decoded, counted and delivered, and never stops the pump" {
    reset(c.RPC_TYPE__Event);
    var rig: Rig = .{};
    try std.testing.expect(!consume(&rig));
    try std.testing.expectEqual(@as(usize, 1), seen.events);
    try std.testing.expectEqual(@as(usize, body.len), seen.proto_len);
    try std.testing.expectEqual(@as(u32, 1), rig.stats.rpc_in);
    try std.testing.expectEqual(@as(u32, 0), rig.stats.undecodable);
    try expectCleanedUp();
}

test "a matching answer runs the extractor once and satisfies the wait" {
    reset(c.RPC_TYPE__Resp);
    script.answers = true;
    script.msg.uid = 9;
    script.msg.msg_id = @intCast(c.RPC_ID__Resp_WifiStart);
    var rig: Rig = .{};
    rig.link.wait = .{ .uid = 9, .resp_id = c.RPC_ID__Resp_WifiStart, .take = take, .armed = true };
    try std.testing.expect(consume(&rig));
    try std.testing.expectEqual(@as(usize, 1), seen.takes);
    try std.testing.expect(rig.link.wait.satisfied);
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x1234), rig.link.wait.result);
    const want = [5]u32{ 1, 9, c.RPC_ID__Resp_WifiStart, 9, c.RPC_ID__Resp_WifiStart };
    try std.testing.expectEqual(want, seen.answers_args);
    try expectCleanedUp();
}

test "a foreign answer leaves the wait untouched" {
    reset(c.RPC_TYPE__Resp);
    var rig: Rig = .{};
    rig.link.wait = .{ .uid = 9, .take = take, .armed = true };
    try std.testing.expect(!consume(&rig));
    try std.testing.expectEqual(@as(usize, 0), seen.takes);
    try std.testing.expect(!rig.link.wait.satisfied);
    try std.testing.expectEqual(@as(u32, 1), rig.stats.rpc_in);
    try expectCleanedUp();
}

test "a request arriving at the host is counted and dropped" {
    reset(c.RPC_TYPE__Req);
    var rig: Rig = .{};
    try std.testing.expect(!consume(&rig));
    try std.testing.expectEqual(@as(usize, 0), seen.events);
    try std.testing.expectEqual(@as(u32, 1), rig.stats.rpc_in);
    try std.testing.expectEqual(@as(u32, 1), rig.stats.undecodable);
    try expectCleanedUp();
}

test "an undecodable message is counted and the arena is still reset" {
    reset(c.RPC_TYPE__Event);
    script.decodes = false;
    var rig: Rig = .{};
    try std.testing.expect(!consume(&rig));
    try std.testing.expectEqual(@as(u32, 1), rig.stats.undecodable);
    try std.testing.expectEqual(@as(u32, 0), rig.stats.rpc_in);
    try std.testing.expectEqual(@as(usize, 2), seen.resets);
    try std.testing.expectEqual(@as(usize, 0), seen.frees);
}

test "a bad envelope is counted before the arena is touched" {
    reset(c.RPC_TYPE__Event);
    script.envelope_ok = false;
    var rig: Rig = .{};
    try std.testing.expect(!consume(&rig));
    try std.testing.expectEqual(@as(u32, 1), rig.stats.undecodable);
    try std.testing.expectEqual(@as(usize, 0), seen.binds);
    try std.testing.expectEqual(@as(usize, 0), seen.resets);
}

test "null arguments and a link without stats are safe" {
    reset(c.RPC_TYPE__Req);
    var link = std.mem.zeroes(c.ra8_c6link_t);
    const payload = [_]u8{0} ** 4;
    try std.testing.expect(!consume_abi.priv_c6link_rpc_consume(null, &payload, payload.len));
    try std.testing.expect(!consume_abi.priv_c6link_rpc_consume(&link, null, 0));
    seen = .{};
    try std.testing.expect(!consume_abi.priv_c6link_rpc_consume(&link, &payload, payload.len));
    try std.testing.expectEqual(@as(usize, 1), seen.frees);
}
