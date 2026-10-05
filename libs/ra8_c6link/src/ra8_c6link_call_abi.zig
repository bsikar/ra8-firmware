//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for `priv_c6link_rpc_call`: stamp a request with the next UID, pack
//! it behind its envelope in the transmit transaction, arm the single wait
//! slot, and pump until the answer lands or the transfer budget runs out.
//! One request is outstanding at a time; a second is refused as busy rather
//! than queued. The transmit buffer is wiped on every exit.
//! The frame-geometry assertions that used to sit beside this in C now live
//! in `ra8_c6link_internal.h`, so the `@cImport` above still checks them.

const std = @import("std");
const Err = @import("abi_err.zig");
const header = @import("c6link_rpc_c.zig");

pub const c = header.c;

const header_bytes: usize = @intCast(c.k_ra8_c6link_header_bytes);
const max_payload: u16 = @intCast(c.k_ra8_c6link_max_payload);

/// Pack `req` straight into the transmit transaction behind its envelope, so
/// the encoder writes where the transport reads. On failure `tx_len` is zero.
fn stage(link: *c.ra8_c6link_t, req: *c.Rpc) c.ra8_err_t {
    link.tx_len = 0;
    const packed_len = c.rpc__get_packed_size(req);
    if (packed_len > max_payload) return Err.invalid_size;

    const payload: [*]u8 = link.tx[header_bytes..].ptr;
    var body_at: u16 = 0;
    const opened = c.priv_c6link_tlv_open(payload, max_payload, @intCast(packed_len), &body_at);
    if (opened != Err.ok) return opened;
    if (c.rpc__pack(req, payload + body_at) != packed_len) return Err.validation_failed;

    link.tx_len = @intCast(@as(usize, body_at) + packed_len);
    link.tx_if = @intCast(c.ESP_SERIAL_IF);
    return Err.ok;
}

/// Drop the wait slot and wipe whatever was staged for transmit.
fn clear(link: *c.ra8_c6link_t) void {
    link.wait = std.mem.zeroes(c.ra8_c6link_wait_t);
    link.tx_len = 0;
    std.crypto.secureZero(u8, &link.tx);
}

/// `priv_c6link_rpc_call`: issue one request and wait for its answer.
pub export fn priv_c6link_rpc_call(
    link: ?*c.ra8_c6link_t,
    req: ?*c.Rpc,
    resp_id: u32,
    take: c.ra8_c6link_take_fn_t,
    take_ctx: ?*anyopaque,
) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const request = req orelse return Err.null_ptr;
    if (take == null) return Err.null_ptr;
    const issuable = c.priv_c6link_rpc_issuable(handle.open, handle.wait.armed, handle.tx_len);
    if (issuable != Err.ok) return issuable;

    handle.next_uid +%= 1;
    request.uid = handle.next_uid;
    const staged = stage(handle, request);
    if (staged != Err.ok) {
        std.crypto.secureZero(u8, &handle.tx);
        return staged;
    }

    handle.wait = std.mem.zeroes(c.ra8_c6link_wait_t);
    handle.wait.uid = request.uid;
    handle.wait.resp_id = resp_id;
    handle.wait.take = take;
    handle.wait.take_ctx = take_ctx;
    handle.wait.result = Err.ok;
    handle.wait.armed = true;

    var stats = std.mem.zeroes(c.ra8_c6link_stats_t);
    const pumped = c.priv_c6link_pump(handle, @intCast(c.k_ra8_c6link_rpc_transfers), &stats);
    const got = handle.wait.satisfied;
    const result = handle.wait.result;
    clear(handle);

    if (pumped != Err.ok) return pumped;
    if (!got) {
        handle.fault.rpc_id = @intCast(request.msg_id);
        handle.fault.resp = 0;
        return Err.timeout;
    }
    return result;
}
