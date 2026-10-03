//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the event fan-out: `priv_c6link_emit`, which every decoded
//! announcement from the co-processor passes through on its way to the
//! application's event callback.

const header = @import("c6link_c.zig");

/// The public `ra8_c6link.h` view, re-exported for the host test.
pub const c = header.c;

/// `priv_c6link_emit`: record and deliver one announcement.
///
/// A boot announcement marks the handle as having seen the co-processor
/// start, the running pump (if any) counts it, then the callback (if any)
/// receives it.
pub export fn priv_c6link_emit(link: ?*c.ra8_c6link_t, ev: ?*const c.ra8_c6link_event_t) callconv(.c) void {
    const handle = link orelse return;
    const event = ev orelse return;

    if (event.kind == c.k_ra8_c6link_event_boot) handle.boot_seen = true;
    if (handle.stats) |stats| stats.*.events +%= 1;
    if (handle.event_cb) |deliver| deliver(handle.cb_ctx, event);
}
