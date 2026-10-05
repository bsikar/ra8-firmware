//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for `priv_c6link_rpc_consume`: unwrap one control-plane payload,
//! decode it into the link's arena, and either satisfy the outstanding wait,
//! deliver an announcement, or count it as undecodable and drop it. The
//! arena is reset before returning however that goes. Request staging and
//! the pump-driven call are in `ra8_c6link_call_abi.zig`.

const header = @import("c6link_rpc_c.zig");

pub const c = header.c;

/// Count one payload that could not be acted on, when the pump is counting.
fn countUndecodable(link: *c.ra8_c6link_t) void {
    if (link.stats) |stats| stats.*.undecodable +%= 1;
}

/// Offer a decoded answer to the outstanding wait. It matches only when the
/// wait is armed, the UID is the one sent, and the id is the expected answer;
/// then the extractor runs exactly once. An armed wait without an extractor
/// cannot happen (`priv_c6link_rpc_call` rejects one) and is treated as no
/// match rather than called through.
fn answer(link: *c.ra8_c6link_t, msg: *const c.Rpc) bool {
    const wait = &link.wait;
    if (!c.priv_c6link_rpc_answers(wait.armed, wait.uid, wait.resp_id, msg.uid, @intCast(msg.msg_id))) return false;
    const take = wait.take orelse return false;
    wait.result = take(wait.take_ctx, msg);
    wait.satisfied = true;
    return true;
}

/// Route one decoded message; true when it satisfied the outstanding wait. A
/// request arriving at a host is a co-processor defect: counted, never acted on.
fn route(link: *c.ra8_c6link_t, msg: *const c.Rpc) bool {
    switch (msg.msg_type) {
        c.RPC_TYPE__Event => c.priv_c6link_rpc_event(link, msg),
        c.RPC_TYPE__Resp => return answer(link, msg),
        else => countUndecodable(link),
    }
    return false;
}

/// `priv_c6link_rpc_consume`: decode one payload and act on it; true when the
/// outstanding wait was satisfied and the pump should stop.
pub export fn priv_c6link_rpc_consume(link: ?*c.ra8_c6link_t, payload: ?[*]const u8, len: u16) callconv(.c) bool {
    const handle = link orelse return false;
    const bytes = payload orelse return false;

    var proto_len: u16 = 0;
    const proto = c.priv_c6link_tlv_body(bytes, len, &proto_len);
    if (proto == null) {
        countUndecodable(handle);
        return false;
    }

    var alloc = std.mem.zeroes(c.ProtobufCAllocator);
    c.priv_c6link_arena_bind(&alloc, handle);
    c.priv_c6link_arena_reset(handle);
    defer c.priv_c6link_arena_reset(handle);

    const msg = c.rpc__unpack(&alloc, proto_len, proto) orelse {
        countUndecodable(handle);
        return false;
    };
    defer c.rpc__free_unpacked(msg, &alloc);
    if (handle.stats) |stats| stats.*.rpc_in +%= 1;
    return route(handle, msg);
}

const std = @import("std");
