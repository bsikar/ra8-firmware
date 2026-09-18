//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `ra8_usb_pal`. Exports every symbol declared by the
//! unchanged `inc/ra8_usb_pal.h` plus the two promoted predicates declared by
//! `src/ra8_usb_pal_internal.h`, and keeps the Ring-3 `ra8_usb` driver and the
//! logger as link-time seams so host suites substitute their own.

const std = @import("std");
const core = @import("internal/root.zig");

const tag: [*:0]const u8 = "USBPAL";

// =============================================================================
// ra8_err_t values used on this surface
// =============================================================================

const err_ok: u16 = 0;
const err_no_mem: u16 = 0x102;
const err_invalid_arg: u16 = 0x103;
const err_invalid_state: u16 = 0x104;
const err_no_data: u16 = 0x10A;
const err_hw_init_failed: u16 = 0x201;
const err_null_ptr: u16 = 0x504;

// =============================================================================
// Link-time seams
// =============================================================================

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void;
extern fn ra8_log_emit_info(tag: [*:0]const u8, message: [*:0]const u8) void;

extern fn ra8_usb_device_init(speed: u8) u16;
extern fn ra8_usb_device_deinit(speed: u8) u16;
extern fn ra8_usb_device_attach(speed: u8, attached: bool) u16;
extern fn ra8_usb_attach_handler(speed: u8, fn_ptr: ?UsbEventFn, ctx: ?*anyopaque) void;

pub const UsbEventFn = *const fn (ctx: ?*anyopaque, speed: u8, status_mask: u16) callconv(.c) void;
pub const PalEventFn = *const fn (ctx: ?*anyopaque, speed: u8, event_mask: u16) callconv(.c) void;

// =============================================================================
// Singleton state
// =============================================================================

const State = struct {
    speed: u8 = 0,
    state: core.PalState = .detached,
    event_fn: ?PalEventFn = null,
    event_ctx: ?*anyopaque = null,
    initialized: bool = false,
    table: core.Table = .{},
};

var s_state: State = .{};

fn rejectNull(ptr: ?*const anyopaque, message: [*:0]const u8) bool {
    if (ptr == null) {
        ra8_log_emit_error(tag, message);
        return true;
    }
    return false;
}

// =============================================================================
// Promoted predicates (ra8_usb_pal_internal.h)
// =============================================================================

pub export fn priv_usb_pal_should_dispatch_event(
    event_fn: ?*const anyopaque,
    mask: u16,
    none_value: u16,
) callconv(.c) bool {
    return core.shouldDispatchEvent(event_fn, mask, none_value);
}

pub export fn priv_usb_pal_ep_out_of_range(ep_addr: u8, ep_max: u8) callconv(.c) bool {
    return core.epOutOfRange(ep_addr, ep_max);
}

// =============================================================================
// ra8_usb event handler installed at init
// =============================================================================

fn internalUsbEvent(ctx: ?*anyopaque, speed: u8, status_mask: u16) callconv(.c) void {
    _ = ctx;
    if (!s_state.initialized) return;
    if (speed != s_state.speed) return;
    const pal_mask = core.translate(status_mask);
    if (core.shouldDispatchEvent(
        @as(?*const anyopaque, @ptrCast(s_state.event_fn)),
        pal_mask,
        core.event_none,
    )) {
        s_state.event_fn.?(s_state.event_ctx, speed, pal_mask);
    }
}

// =============================================================================
// Lifecycle
// =============================================================================

pub export fn ra8_usb_pal_init(speed: u8) callconv(.c) u16 {
    if (!core.speedValid(speed)) return err_invalid_arg;

    const usb_err = ra8_usb_device_init(speed);
    if (usb_err != err_ok) {
        ra8_log_emit_error_val(tag, "ra8_usb_device_init failed", usb_err);
        return err_hw_init_failed;
    }

    s_state.speed = speed;
    s_state.state = .detached;
    s_state.event_fn = null;
    s_state.event_ctx = null;
    s_state.initialized = true;
    s_state.table.resetAll();

    ra8_usb_attach_handler(speed, internalUsbEvent, null);

    ra8_log_emit_info(tag, "PAL ready");
    return err_ok;
}

pub export fn ra8_usb_pal_deinit() callconv(.c) u16 {
    if (!s_state.initialized) return err_invalid_state;

    _ = ra8_usb_device_attach(s_state.speed, false);
    ra8_usb_attach_handler(s_state.speed, null, null);
    const err = ra8_usb_device_deinit(s_state.speed);

    s_state.initialized = false;
    s_state.event_fn = null;
    s_state.event_ctx = null;
    s_state.state = .detached;
    s_state.table.resetAll();
    return err;
}

pub export fn ra8_usb_pal_attach(attached: bool) callconv(.c) u16 {
    if (!s_state.initialized) return err_invalid_state;

    // `ra8_usb_device_attach` rejects only an invalid controller selector, and
    // a successful init stored a validated one.
    _ = ra8_usb_device_attach(s_state.speed, attached);
    s_state.state = if (attached) .attached else .detached;
    return err_ok;
}

pub export fn ra8_usb_pal_get_state(out_state: ?*u8) callconv(.c) u16 {
    if (rejectNull(out_state, "get_state: out_state")) return err_null_ptr;
    if (!s_state.initialized) return err_invalid_state;
    out_state.?.* = @intFromEnum(s_state.state);
    return err_ok;
}

// =============================================================================
// Endpoints
// =============================================================================

pub export fn ra8_usb_pal_ep_open(
    ep_addr_in: u8,
    dir: u8,
    ep_type: u8,
    max_packet: u16,
) callconv(.c) u16 {
    const ep_addr = core.maskEpAddr(ep_addr_in);
    if (!s_state.initialized) return err_invalid_state;
    if (core.epOutOfRange(ep_addr, core.ep_max)) return err_invalid_arg;
    if (!core.dirValid(dir)) return err_invalid_arg;
    if (!core.typeAndPacketValid(ep_type, max_packet)) return err_invalid_arg;

    s_state.table.at(ep_addr).open(
        @enumFromInt(dir),
        @enumFromInt(ep_type),
        max_packet,
    );
    return err_ok;
}

pub export fn ra8_usb_pal_ep_send(ep_addr_in: u8, data: ?[*]const u8, len: u16) callconv(.c) u16 {
    const ep_addr = core.maskEpAddr(ep_addr_in);
    if (!s_state.initialized) return err_invalid_state;
    if (core.epOutOfRange(ep_addr, core.ep_max)) return err_invalid_arg;
    if ((len > core.xfer_max) or (data == null and len != 0)) {
        return if (data == null) err_null_ptr else err_invalid_arg;
    }

    const slot = s_state.table.at(ep_addr);
    if (!slot.opened) return err_invalid_state;
    if (len > slot.max_packet) return err_invalid_arg;
    if (slot.isFull()) return err_no_mem;

    const payload: []const u8 = if (len > 0) data.?[0..len] else &[_]u8{};
    slot.push(payload) catch return err_no_mem;

    if (s_state.event_fn) |callback| {
        callback(s_state.event_ctx, s_state.speed, core.event_ep_in);
    }
    return err_ok;
}

pub export fn ra8_usb_pal_ep_recv(
    ep_addr_in: u8,
    out_buf: ?[*]u8,
    inout_len: ?*u16,
) callconv(.c) u16 {
    if (rejectNull(out_buf, "ep_recv: out_buf")) return err_null_ptr;
    if (rejectNull(inout_len, "ep_recv: inout_len")) return err_null_ptr;

    const ep_addr = core.maskEpAddr(ep_addr_in);
    if (!s_state.initialized) return err_invalid_state;
    if (core.epOutOfRange(ep_addr, core.ep_max)) return err_invalid_arg;

    const capacity = inout_len.?.*;
    if (capacity == 0) return err_invalid_arg;

    const slot = s_state.table.at(ep_addr);
    if (!slot.opened) return err_invalid_state;
    if (slot.isEmpty()) return err_no_data;

    const written = slot.pop(out_buf.?[0..capacity]) catch return err_no_data;
    inout_len.?.* = written;
    return err_ok;
}

pub export fn ra8_usb_pal_set_event_handler(
    handler: ?PalEventFn,
    ctx: ?*anyopaque,
) callconv(.c) u16 {
    if (!s_state.initialized) return err_invalid_state;
    s_state.event_fn = handler;
    s_state.event_ctx = ctx;
    return err_ok;
}

// =============================================================================
// Test-only views
// =============================================================================

pub fn testResetState() void {
    s_state = .{};
}

pub fn testState() *State {
    return &s_state;
}

pub fn testUsbEventHandler() UsbEventFn {
    return internalUsbEvent;
}
