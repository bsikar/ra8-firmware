//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the Ethernet send path: `ra8_c6link_eth_send`, as
//! `ra8_c6link.h` declares it. Admission comes from `tx_admit.zig`; the
//! frame goes out through `priv_c6link_pump`, reached over the C ABI.

const std = @import("std");
const tx_admit = @import("internal/tx_admit.zig");
const rx_route = @import("internal/rx_route.zig");
const Err = @import("abi_err.zig");
const header = @import("c6link_c.zig");

/// The public `ra8_c6link.h` view of the handle.
pub const c = header.c;

extern fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max_transactions: u16, stats: ?*c.ra8_c6link_stats_t) callconv(.c) u16;

comptime {
    if (tx_admit.Bound.max != c.k_ra8_c6link_max_payload) @compileError("max_payload drifted");
}

/// Station traffic goes out on `ESP_STA_IF`.
const send_if: u8 = rx_route.If.sta;

/// The admission verdict as a C error code.
fn admitted(open: bool, len: u16, tx_len: u16) u16 {
    tx_admit.admit(open, len, tx_len) catch |refusal| return switch (refusal) {
        error.NotInitialized => Err.not_initialized,
        error.InvalidSize => Err.invalid_size,
        error.Busy => Err.busy,
    };
    return Err.ok;
}

/// `ra8_c6link_eth_send`: stage one 802.3 frame and clock it out.
///
/// Returns the pump's fault if it failed, `k_ra8_err_hw_timeout` if the
/// frame is still staged when the handshake budget runs out, else `ok`.
/// The staged length is always clear on return.
pub export fn ra8_c6link_eth_send(link: ?*c.ra8_c6link_t, frame: ?[*]const u8, len: u16) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const bytes = frame orelse return Err.null_ptr;
    const verdict = admitted(handle.open, len, handle.tx_len);
    if (verdict != Err.ok) return verdict;

    const at = c.k_ra8_c6link_header_bytes;
    @memcpy(handle.tx[at .. at + len], bytes[0..len]);
    handle.tx_len = len;
    handle.tx_if = send_if;

    var local = std.mem.zeroes(c.ra8_c6link_stats_t);
    const pumped = priv_c6link_pump(handle, c.k_ra8_c6link_hs_giveup, &local);
    const unsent = handle.tx_len != 0;
    handle.tx_len = 0;
    if (pumped != Err.ok) return pumped;
    if (unsent) return Err.hw_timeout;
    return Err.ok;
}
