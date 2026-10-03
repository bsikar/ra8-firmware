//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the Wi-Fi teardown exports: `ra8_c6link_wifi_stop` and
//! `ra8_c6link_wifi_leave`, as `ra8_c6link.h` declares them. Each is a guard
//! plus bare requests through `priv_c6link_bare_req`, which stays in C with
//! the rest of the RPC layer for now.

const Err = @import("abi_err.zig");
const header = @import("c6link_rpc_c.zig");

/// The private `ra8_c6link_internal.h` view, codec ids included.
pub const c = header.c;

/// The requests these exports issue.
pub const Id = struct {
    pub const stop: u32 = c.RPC_ID__Req_WifiStop;
    pub const deinit: u32 = c.RPC_ID__Req_WifiDeinit;
    pub const disconnect: u32 = c.RPC_ID__Req_WifiDisconnect;
};

/// The open link behind `link`, or the code a closed or null one returns.
fn opened(link: ?*c.ra8_c6link_t) error{ Null, Closed }!*c.ra8_c6link_t {
    const handle = link orelse return error.Null;
    if (!handle.open) return error.Closed;
    return handle;
}

fn guardCode(err: error{ Null, Closed }) c.ra8_err_t {
    return switch (err) {
        error.Null => Err.null_ptr,
        error.Closed => Err.not_initialized,
    };
}

/// `ra8_c6link_wifi_stop`: stop the station, then deinit the Wi-Fi driver.
/// Deinit is sent even when stop fails; stop's error wins when both fail.
pub export fn ra8_c6link_wifi_stop(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = opened(link) catch |err| return guardCode(err);
    const stopped = c.priv_c6link_bare_req(handle, Id.stop);
    const deinit = c.priv_c6link_bare_req(handle, Id.deinit);
    return if (stopped != Err.ok) stopped else deinit;
}

/// `ra8_c6link_wifi_leave`: disconnect from the current access point.
pub export fn ra8_c6link_wifi_leave(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = opened(link) catch |err| return guardCode(err);
    return c.priv_c6link_bare_req(handle, Id.disconnect);
}
