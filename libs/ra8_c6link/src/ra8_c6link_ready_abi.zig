//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the two public entry points that drive the pump:
//! `ra8_c6link_poll` and `ra8_c6link_await_ready`, as `ra8_c6link.h`
//! declares them. The pump itself is `priv_c6link_pump` and the identity
//! request is `ra8_c6link_fw_version`; both are reached through the C ABI.

const std = @import("std");
const caps = @import("internal/caps.zig");
const frame = @import("internal/frame.zig");
const rx_route = @import("internal/rx_route.zig");
const Err = @import("abi_err.zig");
const header = @import("c6link_c.zig");

/// The public `ra8_c6link.h` view of the handle and its results.
pub const c = header.c;

extern fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max_transactions: u16, stats: ?*c.ra8_c6link_stats_t) callconv(.c) u16;
extern fn ra8_c6link_fw_version(link: ?*c.ra8_c6link_t, out: ?*c.ra8_c6link_fw_version_t) callconv(.c) c.ra8_err_t;

comptime {
    if (Err.invalid_arg != c.k_ra8_err_invalid_arg) @compileError("k_ra8_err_invalid_arg drifted");
    if (Err.busy != c.k_ra8_err_busy) @compileError("k_ra8_err_busy drifted");
    if (Err.hw_timeout != c.k_ra8_err_hw_timeout) @compileError("k_ra8_err_hw_timeout drifted");
    if (frame.Frame.bytes != c.k_ra8_c6link_frame_bytes) @compileError("frame size drifted");
}

/// The announcement goes out on `ESP_PRIV_IF`.
const announce_if: u8 = rx_route.If.privileged;

/// The first reason `link` cannot be pumped, or `ok`.
fn pumpable(link: *const c.ra8_c6link_t, max_transactions: u16) u16 {
    if (!link.open) return Err.not_initialized;
    if (max_transactions == 0) return Err.invalid_arg;
    return Err.ok;
}

/// `ra8_c6link_poll`: run up to `max_transactions` and report the counters.
pub export fn ra8_c6link_poll(link: ?*c.ra8_c6link_t, max_transactions: u16, stats: ?*c.ra8_c6link_stats_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const bad = pumpable(handle, max_transactions);
    if (bad != Err.ok) return bad;

    var local = std.mem.zeroes(c.ra8_c6link_stats_t);
    const err = priv_c6link_pump(handle, max_transactions, &local);
    if (stats) |out| out.* = local;
    return err;
}

/// Stage the capabilities frame and pump it, retrying only while the pump
/// clocked nothing at all (`hw_timeout`): a busy co-processor never saw the
/// frame, so a retry restates nothing. Any other verdict ends the loop.
fn announce(link: *c.ra8_c6link_t, max_transactions: u16) u16 {
    var verdict: u16 = Err.hw_timeout;
    var attempt: u16 = 0;
    while (attempt < c.k_ra8_c6link_ready_attempts) : (attempt += 1) {
        const payload = link.tx[c.k_ra8_c6link_header_bytes..];
        const len = caps.write(payload) orelse return Err.invalid_size;
        link.tx_len = len;
        link.tx_if = announce_if;

        var local = std.mem.zeroes(c.ra8_c6link_stats_t);
        verdict = priv_c6link_pump(link, max_transactions, &local);
        link.tx_len = 0;
        if (verdict != Err.hw_timeout) break;
    }
    return verdict;
}

/// `ra8_c6link_await_ready`: introduce this host, then ask who the peer is.
///
/// Readiness is the answer to the identity request, not the boot event,
/// which fires only when the co-processor (not this host) boots.
pub export fn ra8_c6link_await_ready(
    link: ?*c.ra8_c6link_t,
    max_transactions: u16,
    out: ?*c.ra8_c6link_fw_version_t,
) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const dst = out orelse return Err.null_ptr;
    const bad = pumpable(handle, max_transactions);
    if (bad != Err.ok) return bad;
    if (handle.tx_len != 0) return Err.busy;

    const announced = announce(handle, max_transactions);
    if (announced != Err.ok) return announced;
    return ra8_c6link_fw_version(handle, dst);
}
