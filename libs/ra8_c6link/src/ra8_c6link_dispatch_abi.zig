//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the frame dispatcher: `priv_c6link_dispatch`, which hands one
//! well-formed received frame to the consumer its interface number names.
//!
//! Control-plane frames go to the RPC decoder (`priv_c6link_rpc_consume`,
//! still C), station and access-point frames to the Ethernet receive callback,
//! and everything else is counted and dropped.

const rx_route = @import("internal/rx_route.zig");
const frame = @import("internal/frame.zig");
const header = @import("c6link_c.zig");

/// The public `ra8_c6link.h` view, re-exported for the host test.
pub const c = header.c;
/// Where a classified frame's payload is.
pub const RxView = header.RxView;

extern fn priv_c6link_rpc_consume(link: *c.ra8_c6link_t, payload: [*]const u8, len: u16) callconv(.c) bool;

/// Count one frame on the running pump's counters, when a pump is running.
fn bump(link: *c.ra8_c6link_t, comptime field: []const u8) void {
    if (link.stats) |stats| @field(stats.*, field) +%= 1;
}

/// Route one data frame. Returns true when it answered the outstanding wait.
pub fn dispatch(link: *c.ra8_c6link_t, view: header.RxView) bool {
    if (@as(usize, view.offset) + view.len > frame.Frame.bytes) return false;
    const payload = link.rx[view.offset..][0..view.len];

    switch (rx_route.routeFor(view.if_type)) {
        .rpc => return priv_c6link_rpc_consume(link, payload.ptr, view.len),
        .ethernet => {
            bump(link, "eth_in");
            if (link.rx_cb) |deliver| deliver(link.cb_ctx, payload.ptr, view.len);
        },
        .counted => bump(link, "unrouted"),
    }
    return false;
}

/// `priv_c6link_dispatch`: route the frame `view` describes in `link.rx`.
pub export fn priv_c6link_dispatch(link: ?*c.ra8_c6link_t, view: ?*const header.RxView) callconv(.c) bool {
    const handle = link orelse return false;
    const where = view orelse return false;
    return dispatch(handle, where.*);
}
