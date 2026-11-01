//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ESWM Ethernet backend of the ra8_io netif facade (RA8FW-318): thin
//! trampolines to the ra8_net_pal singleton frame mover (unmodified).
//! There is exactly one ESWM MAC, so the backend binds no context: ctx is
//! NULL and every call goes straight to the PAL, whose own validation and
//! error codes pass through.

const front = @import("ra8_io_netif_abi.zig");

pub const Iface = front.Iface;
pub const Bus = front.Bus;

const tag = "ra8_io_netif_net_pal";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
// The PAL's exports return ra8_err_t's u16 backing type (ra8_net_pal_abi.zig),
// not a widened c_int: declare them exactly as the PAL defines them.
extern fn ra8_net_pal_send_frame(frame: ?[*]const u8, len: u16) callconv(.c) u16;
extern fn ra8_net_pal_recv_frame(out_buf: ?[*]u8, inout_len: ?*u16) callconv(.c) u16;

fn missing(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return front.err_null_ptr;
}

fn send(ctx: ?*anyopaque, frame: ?[*]const u8, len: u16) callconv(.c) c_int {
    _ = ctx;
    return @as(c_int, ra8_net_pal_send_frame(frame, len));
}

fn poll(ctx: ?*anyopaque, out_buf: ?[*]u8, inout_len: ?*u16) callconv(.c) c_int {
    _ = ctx;
    return @as(c_int, ra8_net_pal_recv_frame(out_buf, inout_len));
}

const net_pal_iface = Iface{ .send = send, .poll = poll };

/// `ra8_io_netif_bind_net_pal`: bind the ESWM singleton into a handle.
export fn ra8_io_netif_bind_net_pal(netif: ?*Bus) c_int {
    const n = netif orelse return missing("netif must not be nullptr");
    n.iface = &net_pal_iface;
    n.ctx = null;
    return front.ok;
}
