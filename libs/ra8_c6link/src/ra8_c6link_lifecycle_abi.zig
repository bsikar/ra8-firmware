//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the handle lifecycle: `ra8_c6link_open`, `ra8_c6link_close`,
//! `ra8_c6link_is_open` and `ra8_c6link_last_fault`, as `ra8_c6link.h`
//! declares them. The handle is the translate-c view of the public struct.

const std = @import("std");
const Err = @import("abi_err.zig");
const header = @import("c6link_c.zig");

/// The public `ra8_c6link.h` view of the handle and its configuration.
pub const c = header.c;

comptime {
    if (Err.null_ptr != c.k_ra8_err_null_ptr) @compileError("k_ra8_err_null_ptr drifted");
    if (Err.invalid_state != c.k_ra8_err_invalid_state) @compileError("k_ra8_err_invalid_state drifted");
    if (Err.invalid_size != c.k_ra8_err_invalid_size) @compileError("k_ra8_err_invalid_size drifted");
    if (Err.not_initialized != c.k_ra8_err_not_initialized) @compileError("k_ra8_err_not_initialized drifted");
}

/// The first reason `cfg` is unusable, or `ok`.
///
/// Every transport row and the arena are required, and the arena must hold
/// the largest message this library decodes.
fn checkCfg(cfg: *const c.ra8_c6link_cfg_t) u16 {
    const seam = cfg.transport;
    if (seam.transfer == null or seam.handshake_active == null or seam.delay_ms == null) return Err.null_ptr;
    if (cfg.arena == null) return Err.null_ptr;
    if (cfg.arena_bytes < c.k_ra8_c6link_arena_min) return Err.invalid_size;
    return Err.ok;
}

/// `ra8_c6link_open`: bind a configuration to a closed handle.
///
/// Copies the seam, callbacks and arena, and starts a fresh session: no
/// request outstanding, no fault, no pump counters, nothing staged.
pub export fn ra8_c6link_open(link: ?*c.ra8_c6link_t, cfg: ?*const c.ra8_c6link_cfg_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const config = cfg orelse return Err.null_ptr;
    if (handle.open) return Err.invalid_state;
    const bad = checkCfg(config);
    if (bad != Err.ok) return bad;

    handle.transport = config.transport;
    handle.event_cb = config.event_cb;
    handle.rx_cb = config.rx_cb;
    handle.cb_ctx = config.cb_ctx;
    handle.arena = config.arena;
    handle.arena_bytes = config.arena_bytes;
    handle.arena_used = 0;
    handle.arena_last = 0;
    handle.next_uid = 0;
    handle.wait = std.mem.zeroes(c.ra8_c6link_wait_t);
    handle.fault = std.mem.zeroes(c.ra8_c6link_fault_t);
    handle.stats = null;
    handle.tx_len = 0;
    handle.tx_if = 0;
    handle.boot_seen = false;
    handle.open = true;
    return Err.ok;
}

/// `ra8_c6link_close`: unbind an open handle.
///
/// The seam, callbacks and arena are forgotten; the recorded fault is kept
/// so it can still be read after close.
pub export fn ra8_c6link_close(link: ?*c.ra8_c6link_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    if (!handle.open) return Err.not_initialized;

    handle.transport = std.mem.zeroes(c.ra8_c6link_transport_t);
    handle.event_cb = null;
    handle.rx_cb = null;
    handle.cb_ctx = null;
    handle.arena = null;
    handle.stats = null;
    handle.tx_len = 0;
    handle.wait = std.mem.zeroes(c.ra8_c6link_wait_t);
    handle.open = false;
    return Err.ok;
}

/// `ra8_c6link_is_open`: has an open succeeded with no close since?
pub export fn ra8_c6link_is_open(link: ?*const c.ra8_c6link_t) callconv(.c) bool {
    const handle = link orelse return false;
    return handle.open;
}

/// `ra8_c6link_last_fault`: copy out the last failing request.
pub export fn ra8_c6link_last_fault(link: ?*const c.ra8_c6link_t, out: ?*c.ra8_c6link_fault_t) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const dst = out orelse return Err.null_ptr;
    dst.* = handle.fault;
    return Err.ok;
}
