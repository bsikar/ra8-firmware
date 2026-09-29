//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_wifi/inc/ra8_wifi.h`. The lifecycle arithmetic
//! lives in `internal/root.zig`; this file owns the exported symbols, the
//! handle and vtable layouts, the argument guards in their original order and
//! the `ra8_err_t` mapping.
//!
//! The radio is a caller-supplied seam: every operation dispatches through the
//! `ra8_wifi_backend_t` rows an application selected by address, so this
//! translation unit names no radio and links against no driver, exactly as the
//! C did. That is what keeps it host-testable against a mock table.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// 48-bit station address (`ra8_wifi_mac_t`).
pub const Mac = implementation.Mac;
/// DHCP lease record (`ra8_wifi_lease_t`).
pub const Lease = implementation.Lease;
/// Associated-AP record (`ra8_wifi_ap_t`).
pub const Ap = implementation.Ap;
/// Handle snapshot (`ra8_wifi_status_t`).
pub const Status = implementation.Status;
/// Lifecycle position (`ra8_wifi_state_t`).
pub const State = implementation.State;
/// Association reading (`ra8_wifi_link_t`).
pub const Link = implementation.Link;

/// Component tag on facade log lines, matching the C `RA8_WIFI_TAG`.
const tag: [*:0]const u8 = "WIFI";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// Log a rejected pointer the way `RA8_CHECK_NULL_PTR` did, then answer
/// `k_ra8_err_null_ptr`.
fn nullPtr(message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return implementation.err_null_ptr;
}

/// IP provider seam (`ra8_wifi_ip_bind_fn`).
pub const IpBindFn = *const fn (
    ip_ctx: ?*anyopaque,
    mac: ?*const Mac,
    out: ?*Lease,
) callconv(.c) u16;

/// The radio-operation vtable (`ra8_wifi_backend_t`). Every row is optional
/// here because the C struct holds plain function pointers a caller may leave
/// null, which is exactly what `ra8_wifi_init` rejects.
pub const Backend = extern struct {
    open: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    close: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    radio_up: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    radio_down: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    join: ?*const fn (
        ctx: ?*anyopaque,
        ssid: ?[*:0]const u8,
        psk: ?[*:0]const u8,
    ) callconv(.c) u16 = null,
    leave: ?*const fn (ctx: ?*anyopaque) callconv(.c) u16 = null,
    service: ?*const fn (ctx: ?*anyopaque, out_link: ?*u8) callconv(.c) u16 = null,
    get_mac: ?*const fn (ctx: ?*anyopaque, out: ?*Mac) callconv(.c) u16 = null,
    get_ap: ?*const fn (ctx: ?*anyopaque, out: ?*Ap) callconv(.c) u16 = null,
    idle: ?*const fn (ctx: ?*anyopaque, ms: u16) callconv(.c) void = null,
};

/// Selection a caller hands `ra8_wifi_init` (`ra8_wifi_cfg_t`).
pub const Config = extern struct {
    backend: ?*const Backend = null,
    backend_ctx: ?*anyopaque = null,
    ip_bind: ?IpBindFn = null,
    ip_ctx: ?*anyopaque = null,
};

/// Caller-owned handle (`ra8_wifi_t`). `state` stays a raw byte: the initial
/// zeroed struct is the only guaranteed initial value, so nothing here may
/// assume the byte names a valid enumerator.
pub const Wifi = extern struct {
    backend: ?*const Backend = null,
    backend_ctx: ?*anyopaque = null,
    ip_bind: ?IpBindFn = null,
    ip_ctx: ?*anyopaque = null,
    mac: Mac = Mac.zero,
    lease: Lease = .{},
    state: u8 = 0,
    rssi: i8 = 0,
    open: bool = false,
    radio_on: bool = false,
    mac_valid: bool = false,
};

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Backend) == ptr * 10);
    std.debug.assert(@sizeOf(Config) == ptr * 4);
    std.debug.assert(@offsetOf(Config, "ip_bind") == ptr * 2);

    std.debug.assert(@offsetOf(Wifi, "backend") == 0);
    std.debug.assert(@offsetOf(Wifi, "backend_ctx") == ptr);
    std.debug.assert(@offsetOf(Wifi, "ip_bind") == ptr * 2);
    std.debug.assert(@offsetOf(Wifi, "ip_ctx") == ptr * 3);
    std.debug.assert(@offsetOf(Wifi, "mac") == ptr * 4);
    // The 6-byte address is followed by the 4-aligned lease, so two bytes of
    // padding sit between them on both the host and Arm.
    std.debug.assert(@offsetOf(Wifi, "lease") == ptr * 4 + 8);
    std.debug.assert(@offsetOf(Wifi, "state") == ptr * 4 + 28);
    std.debug.assert(@offsetOf(Wifi, "mac_valid") == ptr * 4 + 32);
    std.debug.assert(@sizeOf(Wifi) == std.mem.alignForward(usize, ptr * 4 + 33, ptr));
}

/// Which rows of a candidate table are filled in, in the C's validation order.
fn presenceOf(b: *const Backend) implementation.RowPresence {
    return .{
        .open = b.open != null,
        .close = b.close != null,
        .radio_up = b.radio_up != null,
        .radio_down = b.radio_down != null,
        .join = b.join != null,
        .leave = b.leave != null,
        .service = b.service != null,
        .get_mac = b.get_mac != null,
        .get_ap = b.get_ap != null,
        .idle = b.idle != null,
    };
}

/// Validate every row of a candidate backend table, naming the first gap.
fn checkBackend(backend: ?*const Backend) u16 {
    const b = backend orelse return nullPtr("backend");
    if (implementation.missingRow(presenceOf(b))) |row| {
        return nullPtr(row.message());
    }
    return implementation.err_ok;
}

/// Raise the radio if it is not already up.
///
/// Idempotent by design: `ra8_wifi_connect` may be called repeatedly and must
/// not cycle a radio that is already running, because a backend is entitled to
/// treat a second `radio_up` as an error.
fn ensureRadioUp(wifi: *Wifi) u16 {
    if (wifi.radio_on) return implementation.err_ok;
    const up = wifi.backend.?.radio_up.?(wifi.backend_ctx);
    if (up != implementation.err_ok) return up;
    wifi.radio_on = true;
    return implementation.err_ok;
}

/// Pump the backend until the station associates or the budget runs out.
///
/// A failed attempt ends the attempt, not the wait: servicing a link whose
/// radio is mid-association routinely fails, so the loop records the error and
/// carries on, reporting it only if no attempt in the entire budget succeeded.
fn awaitAssociation(wifi: *Wifi) u16 {
    var last_fault: u16 = implementation.err_ok;
    var answered = false;

    var i: u16 = 0;
    while (i < implementation.join_polls) : (i += 1) {
        var link: u8 = @intFromEnum(Link.down);
        const serviced = wifi.backend.?.service.?(wifi.backend_ctx, &link);
        if (serviced != implementation.err_ok) {
            last_fault = serviced;
        } else {
            answered = true;
            if (link == @intFromEnum(Link.up)) {
                wifi.state = @intFromEnum(State.associated);
                return implementation.err_ok;
            }
        }
        wifi.backend.?.idle.?(wifi.backend_ctx, implementation.poll_gap_ms);
    }

    return implementation.waitVerdict(answered, last_fault);
}

pub export fn ra8_wifi_init(wifi: ?*Wifi, cfg: ?*const Config) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    const config = cfg orelse return nullPtr("cfg");
    if (config.ip_bind == null) return nullPtr("cfg.ip_bind");

    const table = checkBackend(config.backend);
    if (table != implementation.err_ok) return table;
    if (handle.open) return implementation.err_invalid_state;

    const opened = config.backend.?.open.?(config.backend_ctx);
    if (opened != implementation.err_ok) return opened;

    handle.* = .{};
    handle.backend = config.backend;
    handle.backend_ctx = config.backend_ctx;
    handle.ip_bind = config.ip_bind;
    handle.ip_ctx = config.ip_ctx;
    handle.state = @intFromEnum(State.down);
    handle.open = true;
    return implementation.err_ok;
}

pub export fn ra8_wifi_deinit(wifi: ?*Wifi) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    if (!handle.open) return implementation.err_not_initialized;

    const closed = handle.backend.?.close.?(handle.backend_ctx);
    handle.open = false;
    handle.radio_on = false;
    handle.state = @intFromEnum(State.down);
    return closed;
}

pub export fn ra8_wifi_connect(wifi: ?*Wifi, ssid: ?[*:0]const u8, psk: ?[*:0]const u8) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    if (ssid == null) return nullPtr("ssid");
    if (!handle.open) return implementation.err_not_initialized;

    const powered = ensureRadioUp(handle);
    if (powered != implementation.err_ok) return powered;

    const got_mac = handle.backend.?.get_mac.?(handle.backend_ctx, &handle.mac);
    if (got_mac != implementation.err_ok) return got_mac;
    handle.mac_valid = true;

    handle.state = @intFromEnum(State.associating);
    const asked = handle.backend.?.join.?(handle.backend_ctx, ssid, psk);
    if (asked != implementation.err_ok) return asked;
    return awaitAssociation(handle);
}

pub export fn ra8_wifi_disconnect(wifi: ?*Wifi) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    if (!handle.open) return implementation.err_not_initialized;

    const left = handle.backend.?.leave.?(handle.backend_ctx);
    const stopped = handle.backend.?.radio_down.?(handle.backend_ctx);
    handle.radio_on = false;
    handle.state = @intFromEnum(State.down);
    handle.lease = .{};
    return if (left != implementation.err_ok) left else stopped;
}

pub export fn ra8_wifi_wait_ip(wifi: ?*Wifi, out: ?*Lease) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    const sink = out orelse return nullPtr("out");
    sink.* = .{};
    if (!handle.open) return implementation.err_not_initialized;
    if (!implementation.isAssociated(handle.state)) return implementation.err_invalid_state;

    var lease: Lease = .{};
    const bound = handle.ip_bind.?(handle.ip_ctx, &handle.mac, &lease);
    if (bound != implementation.err_ok) {
        handle.lease = .{};
        return bound;
    }
    lease.bound = implementation.leaseBound(lease.ip);
    if (!lease.bound) {
        handle.lease = .{};
        return implementation.err_timeout;
    }

    handle.lease = lease;
    handle.state = @intFromEnum(State.ip_bound);
    sink.* = lease;
    return implementation.err_ok;
}

pub export fn ra8_wifi_get_ip(wifi: ?*const Wifi, out: ?*Lease) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    const sink = out orelse return nullPtr("out");
    if (!handle.open) return implementation.err_not_initialized;
    sink.* = handle.lease;
    return implementation.err_ok;
}

pub export fn ra8_wifi_status(wifi: ?*const Wifi, out: ?*Status) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    const sink = out orelse return nullPtr("out");
    if (!handle.open) return implementation.err_not_initialized;

    sink.* = implementation.statusFrom(handle.state, handle.rssi, handle.lease);
    return implementation.err_ok;
}

pub export fn ra8_wifi_poll(wifi: ?*Wifi, out: ?*u8) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    const sink = out orelse return nullPtr("out");
    sink.* = @intFromEnum(Link.down);
    if (!handle.open) return implementation.err_not_initialized;
    if (implementation.isIpBound(handle.state)) return implementation.err_invalid_state;

    var link: u8 = @intFromEnum(Link.down);
    const serviced = handle.backend.?.service.?(handle.backend_ctx, &link);
    if (serviced != implementation.err_ok) return serviced;

    handle.state = implementation.stateForLink(link);
    sink.* = link;
    return implementation.err_ok;
}

pub export fn ra8_wifi_get_mac(wifi: ?*Wifi, out: ?*Mac) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    const sink = out orelse return nullPtr("out");
    if (!handle.open) return implementation.err_not_initialized;

    const got = handle.backend.?.get_mac.?(handle.backend_ctx, sink);
    if (got != implementation.err_ok) {
        // A station's own address does not change while it is associated, so a
        // cached one is the right answer when the radio cannot be re-asked;
        // handing the caller 00:00:00:00:00:00 instead is how the bench printed
        // a null MAC out of a run that had in fact associated.
        if (handle.mac_valid) {
            sink.* = handle.mac;
            return implementation.err_ok;
        }
        sink.* = Mac.zero;
        return got;
    }
    handle.mac = sink.*;
    handle.mac_valid = true;
    return implementation.err_ok;
}

pub export fn ra8_wifi_get_ap(wifi: ?*Wifi, out: ?*Ap) callconv(.c) u16 {
    const handle = wifi orelse return nullPtr("wifi");
    const sink = out orelse return nullPtr("out");
    if (!handle.open) return implementation.err_not_initialized;
    if (!implementation.isAssociated(handle.state)) return implementation.err_invalid_state;

    const got = handle.backend.?.get_ap.?(handle.backend_ctx, sink);
    if (got != implementation.err_ok) {
        sink.* = .{};
        return got;
    }
    handle.rssi = sink.rssi;
    return implementation.err_ok;
}
