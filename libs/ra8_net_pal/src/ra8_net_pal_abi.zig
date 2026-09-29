//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_net_pal/inc/ra8_net_pal.h`. The ring and the
//! event translation live in `internal/root.zig`; this file owns the exported
//! symbols, the singleton state, the argument guards in their original order,
//! the `ra8_err_t` mapping, and the `ra8_eth` seam.
//!
//! The Ring-3 driver stays a link-time seam: `ra8_eth_init`, `ra8_eth_deinit`
//! and `ra8_eth_attach_handler` are declared extern, so the host suite's fake
//! Ethernet fixture substitutes for the real ESWM block exactly as it did
//! under the C implementation.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// 48-bit MAC container (`ra8_net_pal_mac_t`).
pub const Mac = implementation.Mac;
/// Link state surfaced to the stack (`ra8_net_pal_link_state_t`).
pub const LinkState = implementation.LinkState;

/// Subset of `ra8_err_t` this library returns.
pub const NetPalError = enum(u16) {
    ok = 0,
    no_mem = 0x102,
    invalid_arg = 0x103,
    invalid_state = 0x104,
    no_data = 0x10A,
    hw_init_failed = 0x201,
    null_ptr = 0x504,
};

/// Component tag on the library's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "NETPAL";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;

/// Stack-facing event callback (`ra8_net_pal_event_fn_t`).
pub const EventFn = *const fn (ctx: ?*anyopaque, event_mask: u32) callconv(.c) void;

/// Ring-3 driver event callback (`ra8_eth_event_fn_t`).
const EthEventFn = *const fn (ctx: ?*anyopaque, status_mask: u32) callconv(.c) void;

extern fn ra8_eth_init() u16;
extern fn ra8_eth_deinit() u16;
extern fn ra8_eth_attach_handler(handler: ?EthEventFn, ctx: ?*anyopaque) void;

/// Singleton PAL state: one ESWM block per chip, so one instance.
const State = struct {
    mac: Mac = Mac.zero,
    link_state: LinkState = .down,
    event_fn: ?EventFn = null,
    event_ctx: ?*anyopaque = null,
    initialized: bool = false,
    ring: implementation.Ring = .{},
};

var state: State = .{};

const ok = @intFromEnum(NetPalError.ok);

/// Reject a NULL argument with the C's log line and `ra8_err_t` code.
fn rejectNull(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return @intFromEnum(NetPalError.null_ptr);
}

/// `ra8_eth` handler installed during init: translate, then fan out.
///
/// Drops events while the PAL is uninitialized, then forwards only a
/// non-empty translated mask to a callback that is actually attached. Both
/// conditions of that AND are load-bearing and the C's order is preserved.
fn ethEvent(ctx: ?*anyopaque, status_mask: u32) callconv(.c) void {
    _ = ctx;
    if (!state.initialized) {
        return;
    }
    const pal_mask = implementation.translateEvent(status_mask);
    if (state.event_fn != null and pal_mask != implementation.event_none) {
        state.event_fn.?(state.event_ctx, pal_mask);
    }
}

/// Bring up the PAL singleton over `ra8_eth`.
///
/// The underlying driver comes up first, so a hardware failure reports
/// `hw_init_failed` without touching PAL state. A NULL `mac` keeps the
/// all-zero default; the handler is attached last, as in the C.
pub export fn ra8_net_pal_init(mac: ?*const Mac) callconv(.c) u16 {
    const eth_err = ra8_eth_init();
    if (eth_err != ok) {
        ra8_log_emit_error_val(tag, "ra8_eth_init failed", @as(u32, eth_err));
        return @intFromEnum(NetPalError.hw_init_failed);
    }

    state.mac = Mac.zero;
    state.link_state = .down;
    state.event_fn = null;
    state.event_ctx = null;
    state.initialized = true;
    state.ring.reset();

    if (mac) |supplied| {
        state.mac = supplied.*;
    }

    ra8_eth_attach_handler(ethEvent, null);

    ra8_log_emit_info(tag, "PAL ready");
    return ok;
}

/// Tear the PAL singleton down and release `ra8_eth`.
///
/// Returns whatever `ra8_eth_deinit` reports, but the PAL is marked down
/// either way: the C cleared its own state after the driver call and did not
/// roll back on a driver error.
pub export fn ra8_net_pal_deinit() callconv(.c) u16 {
    if (!state.initialized) {
        return @intFromEnum(NetPalError.invalid_state);
    }
    ra8_eth_attach_handler(null, null);
    const err = ra8_eth_deinit();
    state.initialized = false;
    state.event_fn = null;
    state.event_ctx = null;
    state.link_state = .down;
    state.ring.reset();
    return err;
}

/// Programme the stored MAC address.
///
/// Guard order is the contract: a NULL descriptor is rejected (and logged)
/// before the initialization check, so a pre-init NULL call reports
/// `null_ptr`, not `invalid_state`.
pub export fn ra8_net_pal_set_mac_addr(mac: ?*const Mac) callconv(.c) u16 {
    const supplied = mac orelse return rejectNull("set_mac_addr: mac");
    if (!state.initialized) {
        return @intFromEnum(NetPalError.invalid_state);
    }
    state.mac = supplied.*;
    return ok;
}

/// Read the currently programmed MAC address.
pub export fn ra8_net_pal_get_mac_addr(out_mac: ?*Mac) callconv(.c) u16 {
    const out = out_mac orelse return rejectNull("get_mac_addr: out_mac");
    if (!state.initialized) {
        return @intFromEnum(NetPalError.invalid_state);
    }
    out.* = state.mac;
    return ok;
}

/// Queue a complete Ethernet frame for transmit.
///
/// Guards run frame -> initialized -> length -> ring depth, which is what
/// lets the host suite tell `null_ptr`, `invalid_state`, `invalid_arg` and
/// `no_mem` apart. A successful enqueue fans out `tx_done` whenever a
/// handler is attached, with no mask test, as the C did.
pub export fn ra8_net_pal_send_frame(frame: ?[*]const u8, len: u16) callconv(.c) u16 {
    const bytes = frame orelse return rejectNull("send_frame: frame");
    if (!state.initialized) {
        return @intFromEnum(NetPalError.invalid_state);
    }
    if (!implementation.sendLenValid(len)) {
        return @intFromEnum(NetPalError.invalid_arg);
    }
    if (state.ring.isFull()) {
        return @intFromEnum(NetPalError.no_mem);
    }
    std.debug.assert(state.ring.push(bytes[0..len]));
    if (state.event_fn) |handler| {
        handler(state.event_ctx, implementation.event_tx_done);
    }
    return ok;
}

/// Pop the next queued frame into a caller buffer.
///
/// `inout_len` carries the buffer capacity in and the byte count out. An
/// empty ring answers `no_data` so a caller can poll without blocking.
pub export fn ra8_net_pal_recv_frame(out_buf: ?[*]u8, inout_len: ?*u16) callconv(.c) u16 {
    const buffer = out_buf orelse return rejectNull("recv_frame: out_buf");
    const length = inout_len orelse return rejectNull("recv_frame: inout_len");
    if (!state.initialized) {
        return @intFromEnum(NetPalError.invalid_state);
    }
    if (!implementation.recvCapacityValid(length.*)) {
        return @intFromEnum(NetPalError.invalid_arg);
    }
    const written = state.ring.pop(buffer[0..implementation.frame_max]) orelse {
        return @intFromEnum(NetPalError.no_data);
    };
    length.* = written;
    return ok;
}

/// Read the last observed link state.
pub export fn ra8_net_pal_link_status(out_state: ?*LinkState) callconv(.c) u16 {
    const out = out_state orelse return rejectNull("link_status: out_state");
    if (!state.initialized) {
        return @intFromEnum(NetPalError.invalid_state);
    }
    out.* = state.link_state;
    return ok;
}

/// Install (or, with a NULL handler, detach) the single event callback.
///
/// The C emitted no diagnostic here and accepted a NULL handler as a
/// detach, so there is no null guard: only the initialization check.
pub export fn ra8_net_pal_set_event_handler(handler: ?EventFn, ctx: ?*anyopaque) callconv(.c) u16 {
    if (!state.initialized) {
        return @intFromEnum(NetPalError.invalid_state);
    }
    state.event_fn = handler;
    state.event_ctx = ctx;
    return ok;
}

comptime {
    // `ra8_err_t` is 16-bit across the repo; these are the only codes the
    // library can return.
    std.debug.assert(@intFromEnum(NetPalError.ok) == 0);
    std.debug.assert(@intFromEnum(NetPalError.no_mem) == 0x102);
    std.debug.assert(@intFromEnum(NetPalError.invalid_arg) == 0x103);
    std.debug.assert(@intFromEnum(NetPalError.invalid_state) == 0x104);
    std.debug.assert(@intFromEnum(NetPalError.no_data) == 0x10A);
    std.debug.assert(@intFromEnum(NetPalError.hw_init_failed) == 0x201);
    std.debug.assert(@intFromEnum(NetPalError.null_ptr) == 0x504);
    // Caller-owned layouts and the documented enumerators.
    std.debug.assert(@sizeOf(Mac) == implementation.mac_addr_len);
    std.debug.assert(@sizeOf(LinkState) == 1);
    std.debug.assert(@intFromEnum(LinkState.down) == 0);
    std.debug.assert(@intFromEnum(LinkState.up) == 1);
    std.debug.assert(implementation.event_none == 0x00);
    std.debug.assert(implementation.event_link_up == 0x01);
    std.debug.assert(implementation.event_link_down == 0x02);
    std.debug.assert(implementation.event_rx_ready == 0x04);
    std.debug.assert(implementation.event_tx_done == 0x08);
    std.debug.assert(implementation.event_error == 0x10);
}
