//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ESP32-C6 backend of the ra8_io netif facade (RAFW-318): thin
//! trampolines to an open ra8_c6link link (unmodified). The C6 delivers
//! received frames through the link's rx_cb, fixed at ::ra8_c6link_open,
//! so the backend exports ra8_io_netif_c6link_rx_sink for the app to name
//! in its link config with this backend context as cb_ctx. The sink stages
//! frames into a caller-supplied ring; poll pumps one ra8_c6link_poll
//! transaction (which runs the sink) and drains one staged frame.

const std = @import("std");
const front = @import("ra8_io_netif_abi.zig");

pub const Iface = front.Iface;
pub const Bus = front.Bus;

const tag = "ra8_io_netif_c6link";

/// k_ra8_c6link_max_payload (libs/ra8_c6link/inc/ra8_c6link.h). The C6
/// never forwards a longer frame; the sink refuses one that claims more.
const max_payload: u16 = 1588;

/// One ring slot: a 2-byte little-endian length prefix, then the frame.
pub const slot_bytes: usize = @as(usize, max_payload) + 2;

/// ra8_err_t values this unit adds to the front end's (ra8_err.h).
pub const err_no_data: c_int = 0x10A;
pub const err_invalid_size: c_int = 0x105;
pub const err_invalid_arg: c_int = 0x103;

/// The caller-allocated backend context (inc/ra8_io_netif_c6link.h).
pub const Ctx = extern struct {
    link: ?*anyopaque,
    ring: ?[*]u8,
    slots: u16,
    head: u16,
    count: u16,
    dropped: u16,
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_c6link_eth_send(link: ?*anyopaque, frame: ?[*]const u8, len: u16) callconv(.c) c_int;
extern fn ra8_c6link_poll(link: ?*anyopaque, max_transactions: u16, stats: ?*anyopaque) callconv(.c) c_int;

fn missing(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return front.err_null_ptr;
}

/// The link's rx sink: stage one frame, or count a drop on a full ring.
/// The co-processor send path has no back-pressure, so a full ring must
/// never block the pump.
pub export fn ra8_io_netif_c6link_rx_sink(cb_ctx: ?*anyopaque, frame: ?[*]const u8, len: u16) callconv(.c) void {
    const b: *Ctx = @ptrCast(@alignCast(cb_ctx orelse return));
    const f = frame orelse return;
    if (len == 0 or len > max_payload) return;
    if (b.count >= b.slots) {
        b.dropped += 1;
        return;
    }
    const tail = (@as(usize, b.head) + @as(usize, b.count)) % b.slots;
    const slot = b.ring.? + tail * slot_bytes;
    std.mem.writeInt(u16, slot[0..2], len, .little);
    @memcpy(slot[2..][0..len], f[0..len]);
    b.count += 1;
}

fn send(ctx: ?*anyopaque, frame: ?[*]const u8, len: u16) callconv(.c) c_int {
    const b: *Ctx = @ptrCast(@alignCast(ctx orelse return front.err_null_ptr));
    return ra8_c6link_eth_send(b.link, frame, len);
}

fn poll(ctx: ?*anyopaque, out_buf: ?[*]u8, inout_len: ?*u16) callconv(.c) c_int {
    const b: *Ctx = @ptrCast(@alignCast(ctx orelse return front.err_null_ptr));
    // Pump one transaction so the sink stages whatever arrived, then drain
    // the oldest staged frame. A NULL link (bound before open) skips the
    // pump; staged frames still drain.
    if (b.link != null) _ = ra8_c6link_poll(b.link, 1, null);
    if (b.count == 0) return err_no_data;
    const cap = inout_len orelse return missing("inout_len must not be nullptr");
    const slot = b.ring.? + @as(usize, b.head) * slot_bytes;
    const flen = std.mem.readInt(u16, slot[0..2], .little);
    if (cap.* < flen) return err_invalid_size;
    const dst = out_buf orelse return missing("out_buf must not be nullptr");
    @memcpy(dst[0..flen], slot[2..][0..flen]);
    cap.* = flen;
    b.head = @intCast((@as(usize, b.head) + 1) % b.slots);
    b.count -= 1;
    return front.ok;
}

const c6_iface = Iface{ .send = send, .poll = poll };

/// `ra8_io_netif_bind_c6link`: bind an open link into a handle.
export fn ra8_io_netif_bind_c6link(
    netif: ?*Bus,
    backend: ?*Ctx,
    link: ?*anyopaque,
    ring: ?[*]u8,
    ring_bytes: u16,
) c_int {
    const n = netif orelse return missing("netif must not be nullptr");
    const b = backend orelse return missing("backend must not be nullptr");
    const r = ring orelse return missing("ring must not be nullptr");
    const slots = ring_bytes / @as(u16, @intCast(slot_bytes));
    if (slots == 0) {
        ra8_log_emit_error(tag, "ring_bytes smaller than one slot");
        return err_invalid_arg;
    }
    b.link = link;
    b.ring = r;
    b.slots = slots;
    b.head = 0;
    b.count = 0;
    b.dropped = 0;
    n.iface = &c6_iface;
    n.ctx = b;
    return front.ok;
}
