//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `priv_c6link_rpc_call` against scripted codec, envelope, issuable and pump
//! hooks: the request gets the next UID, lands behind its envelope, arms the
//! wait while the pump runs, and leaves the wait cleared and the transmit
//! buffer wiped on every exit.

const std = @import("std");
const call_abi = @import("call_abi");

const c = call_abi.c;

const ok: c.ra8_err_t = 0;
const busy: c.ra8_err_t = 0x109;
const timeout: c.ra8_err_t = 0x108;
const invalid_size: c.ra8_err_t = 0x105;
const validation_failed: c.ra8_err_t = 0x501;
const spi_error: c.ra8_err_t = 0x402;
const null_ptr: c.ra8_err_t = 0x504;
const header_bytes: usize = 12;

const Script = struct {
    packed_len: usize = 5,
    pack_short: bool = false,
    issuable: c.ra8_err_t = ok,
    body_at: u16 = 3,
    pump: c.ra8_err_t = ok,
    satisfy: bool = true,
    answer: c.ra8_err_t = ok,
};

const Seen = struct {
    pumps: usize = 0,
    armed: bool = false,
    uid: u32 = 0,
    resp_id: u32 = 0,
    tx_len: u16 = 0,
    tx_if: u8 = 0,
    body_byte: u8 = 0,
};

var script: Script = .{};
var seen: Seen = .{};

export fn rpc__get_packed_size(msg: [*c]const c.Rpc) callconv(.c) usize {
    _ = msg;
    return script.packed_len;
}

export fn rpc__pack(msg: [*c]const c.Rpc, out: [*c]u8) callconv(.c) usize {
    _ = msg;
    @memset(out[0..script.packed_len], 0x5A);
    return if (script.pack_short) script.packed_len - 1 else script.packed_len;
}

export fn priv_c6link_tlv_open(out: ?[*]u8, cap: u16, proto_len: u16, body_at: ?*u16) callconv(.c) u16 {
    _ = cap;
    _ = proto_len;
    @memset(out.?[0..script.body_at], 0xE0);
    body_at.?.* = script.body_at;
    return ok;
}

export fn priv_c6link_rpc_issuable(open: bool, armed: bool, tx_len: u16) callconv(.c) u16 {
    _ = open;
    _ = armed;
    _ = tx_len;
    return script.issuable;
}

export fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max: u16, stats: ?*c.ra8_c6link_stats_t) callconv(.c) u16 {
    _ = max;
    _ = stats;
    const l = link.?;
    seen.pumps += 1;
    seen.armed = l.wait.armed;
    seen.uid = l.wait.uid;
    seen.resp_id = l.wait.resp_id;
    seen.tx_len = l.tx_len;
    seen.tx_if = l.tx_if;
    seen.body_byte = l.tx[header_bytes + script.body_at];
    if (script.satisfy) {
        l.wait.satisfied = true;
        l.wait.result = script.answer;
    }
    return script.pump;
}

fn take(ctx: ?*anyopaque, msg: ?*const anyopaque) callconv(.c) c.ra8_err_t {
    _ = ctx;
    _ = msg;
    return ok;
}

fn reset() void {
    script = .{};
    seen = .{};
}

fn freshLink() c.ra8_c6link_t {
    var link = std.mem.zeroes(c.ra8_c6link_t);
    link.open = true;
    link.next_uid = 41;
    return link;
}

fn expectWiped(link: *const c.ra8_c6link_t) !void {
    try std.testing.expectEqual(@as(u16, 0), link.tx_len);
    try std.testing.expect(!link.wait.armed);
    for (link.tx) |b| try std.testing.expectEqual(@as(u8, 0), b);
}

test "a null link, request or extractor is refused before anything moves" {
    reset();
    var link = freshLink();
    var req = std.mem.zeroes(c.Rpc);
    try std.testing.expectEqual(null_ptr, call_abi.priv_c6link_rpc_call(null, &req, 1, take, null));
    try std.testing.expectEqual(null_ptr, call_abi.priv_c6link_rpc_call(&link, null, 1, take, null));
    try std.testing.expectEqual(null_ptr, call_abi.priv_c6link_rpc_call(&link, &req, 1, null, null));
    try std.testing.expectEqual(@as(u32, 41), link.next_uid);
}

test "a link that cannot issue returns its verdict and spends no UID" {
    reset();
    script.issuable = busy;
    var link = freshLink();
    var req = std.mem.zeroes(c.Rpc);
    try std.testing.expectEqual(busy, call_abi.priv_c6link_rpc_call(&link, &req, 7, take, null));
    try std.testing.expectEqual(@as(u32, 41), link.next_uid);
    try std.testing.expectEqual(@as(usize, 0), seen.pumps);
}

test "an answered call stages behind the envelope, arms the wait, then wipes" {
    reset();
    script.answer = 0x106;
    var link = freshLink();
    var req = std.mem.zeroes(c.Rpc);
    try std.testing.expectEqual(@as(c.ra8_err_t, 0x106), call_abi.priv_c6link_rpc_call(&link, &req, 7, take, null));
    try std.testing.expectEqual(@as(u32, 42), req.uid);
    try std.testing.expect(seen.armed);
    try std.testing.expectEqual(@as(u32, 42), seen.uid);
    try std.testing.expectEqual(@as(u32, 7), seen.resp_id);
    try std.testing.expectEqual(@as(u16, 3 + 5), seen.tx_len);
    try std.testing.expectEqual(@as(u8, @intCast(c.ESP_SERIAL_IF)), seen.tx_if);
    try std.testing.expectEqual(@as(u8, 0x5A), seen.body_byte);
    try expectWiped(&link);
}

test "no answer within the pump budget is a timeout recorded in the fault slot" {
    reset();
    script.satisfy = false;
    var link = freshLink();
    link.fault.resp = 9;
    var req = std.mem.zeroes(c.Rpc);
    req.msg_id = c.RPC_ID__Req_WifiStart;
    try std.testing.expectEqual(timeout, call_abi.priv_c6link_rpc_call(&link, &req, 7, take, null));
    try std.testing.expectEqual(@as(u32, @intCast(c.RPC_ID__Req_WifiStart)), link.fault.rpc_id);
    try std.testing.expectEqual(@as(i32, 0), link.fault.resp);
    try expectWiped(&link);
}

test "a transport fault from the pump wins over any answer" {
    reset();
    script.pump = spi_error;
    var link = freshLink();
    var req = std.mem.zeroes(c.Rpc);
    try std.testing.expectEqual(spi_error, call_abi.priv_c6link_rpc_call(&link, &req, 7, take, null));
    try expectWiped(&link);
}

test "a request larger than one frame is refused without pumping" {
    reset();
    script.packed_len = @intCast(c.k_ra8_c6link_max_payload + 1);
    var link = freshLink();
    var req = std.mem.zeroes(c.Rpc);
    try std.testing.expectEqual(invalid_size, call_abi.priv_c6link_rpc_call(&link, &req, 7, take, null));
    try std.testing.expectEqual(@as(usize, 0), seen.pumps);
    try std.testing.expectEqual(@as(u32, 42), link.next_uid);
    try expectWiped(&link);
}

test "a codec that packs a different length than it predicted is refused" {
    reset();
    script.pack_short = true;
    var link = freshLink();
    var req = std.mem.zeroes(c.Rpc);
    try std.testing.expectEqual(validation_failed, call_abi.priv_c6link_rpc_call(&link, &req, 7, take, null));
    try std.testing.expectEqual(@as(usize, 0), seen.pumps);
    try expectWiped(&link);
}
