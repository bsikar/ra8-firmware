//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI of the ra8_io netif front end (inc/ra8_io_netif.h, RA8FW-318):
//! validate the handle, then dispatch through the bound backend vtable.
//! The vtable shape is the one the backend units pin against
//! src/ra8_io_netif_internal.h.

const tag = "ra8_io_netif";

/// ra8_err_t values this unit returns (ra8_err.h).
pub const ok: c_int = 0;
pub const err_null_ptr: c_int = 0x504;
pub const err_not_initialized: c_int = 0x10F;

/// The bound backend vtable (ra8_io_netif_internal.h); both rows mandatory
/// by contract, NULL-checked only for a malformed backend.
pub const Iface = extern struct {
    send: ?*const fn (ctx: ?*anyopaque, frame: ?[*]const u8, len: u16) callconv(.c) c_int,
    poll: ?*const fn (ctx: ?*anyopaque, out_buf: ?[*]u8, inout_len: ?*u16) callconv(.c) c_int,
};

/// The caller-allocated handle: a bound vtable plus its backend context.
pub const Bus = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
};

comptime {
    if (@sizeOf(Bus) != 2 * @sizeOf(usize)) @compileError("Bus must be two pointers");
}

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// The bound vtable, or the error the C front end returned for this handle.
fn vtable(netif: ?*const Bus) union(enum) { iface: *const Iface, err: c_int } {
    const n = netif orelse return .{ .err = err_null_ptr };
    const i = n.iface orelse return .{ .err = err_not_initialized };
    return .{ .iface = i };
}

/// RA8_CHECK_NULL_PTR: log the missing piece once, report a null pointer.
fn missing(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err_null_ptr;
}

/// `ra8_io_netif_send`: hand one frame to the bound transport.
export fn ra8_io_netif_send(netif: ?*const Bus, frame: ?[*]const u8, len: u16) c_int {
    const i = switch (vtable(netif)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.send orelse return missing("backend send op missing");
    return op(netif.?.ctx, frame, len);
}

/// `ra8_io_netif_poll`: pump the transport and drain at most one frame.
export fn ra8_io_netif_poll(netif: ?*const Bus, out_buf: ?[*]u8, inout_len: ?*u16) c_int {
    const i = switch (vtable(netif)) {
        .iface => |i| i,
        .err => |e| return e,
    };
    const op = i.poll orelse return missing("backend poll op missing");
    return op(netif.?.ctx, out_buf, inout_len);
}
