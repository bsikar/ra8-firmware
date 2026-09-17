//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure core of the Wi-Fi facade: the value types the C ABI shares, the
//! lifecycle arithmetic (which state a link reading puts a handle in, when a
//! lease counts as bound, what a spent poll budget means) and the backend
//! table's row-by-row validation order.
//!
//! Nothing here dispatches through a function pointer or touches a radio, so
//! every rule the facade enforces is testable on its own.

const std = @import("std");

/// Octets in a station address (`k_ra8_wifi_mac_bytes`).
pub const mac_bytes: usize = 6;
/// Longest SSID the AP record carries (`k_ra8_wifi_ssid_max`).
pub const ssid_max: usize = 32;
/// Times `ra8_wifi_connect` services the link (`k_ra8_wifi_join_polls`).
pub const join_polls: u16 = 200;
/// Milliseconds the facade idles between attempts (`k_ra8_wifi_poll_gap_ms`).
pub const poll_gap_ms: u16 = 50;

/// Lifecycle position of a handle (`ra8_wifi_state_t`).
pub const State = enum(u8) {
    down = 0,
    associating = 1,
    associated = 2,
    ip_bound = 3,
};

/// Association reading from the backend (`ra8_wifi_link_t`).
pub const Link = enum(u8) {
    down = 0,
    up = 1,
};

/// Subset of `ra8_err_t` the facade produces itself; backend codes pass through.
pub const err_ok: u16 = 0x0000;
pub const err_invalid_state: u16 = 0x0104;
pub const err_timeout: u16 = 0x0108;
pub const err_not_initialized: u16 = 0x010F;
pub const err_null_ptr: u16 = 0x0504;

/// 48-bit station address (`ra8_wifi_mac_t`).
pub const Mac = extern struct {
    octet: [mac_bytes]u8,

    /// The all-zero address the C returns on an unanswerable read.
    pub const zero: Mac = .{ .octet = .{0} ** mac_bytes };
};

/// What an IP provider handed back (`ra8_wifi_lease_t`).
pub const Lease = extern struct {
    ip: u32 = 0,
    mask: u32 = 0,
    gateway: u32 = 0,
    dhcp_server: u32 = 0,
    bound: bool = false,

    /// A cleared lease, the C's `(ra8_wifi_lease_t){}`.
    pub const empty: Lease = .{};
};

/// What the radio knows about the associated AP (`ra8_wifi_ap_t`).
pub const Ap = extern struct {
    bssid: Mac = Mac.zero,
    ssid: [ssid_max + 1]u8 = .{0} ** (ssid_max + 1),
    ssid_len: u8 = 0,
    channel: u8 = 0,
    rssi: i8 = 0,
    authmode: i32 = 0,

    /// A cleared record, the C's `(ra8_wifi_ap_t){}`.
    pub const empty: Ap = .{};
};

/// Snapshot handed to `ra8_wifi_status` (`ra8_wifi_status_t`).
pub const Status = extern struct {
    state: u8 = 0,
    associated: bool = false,
    ip_bound: bool = false,
    rssi: i8 = 0,
    ip: Lease = .{},
};

comptime {
    std.debug.assert(@sizeOf(Mac) == 6);
    std.debug.assert(@sizeOf(Lease) == 20);
    std.debug.assert(@offsetOf(Lease, "bound") == 16);
    std.debug.assert(@sizeOf(Ap) == 48);
    std.debug.assert(@offsetOf(Ap, "ssid") == 6);
    std.debug.assert(@offsetOf(Ap, "ssid_len") == 39);
    std.debug.assert(@offsetOf(Ap, "channel") == 40);
    std.debug.assert(@offsetOf(Ap, "rssi") == 41);
    std.debug.assert(@offsetOf(Ap, "authmode") == 44);
    std.debug.assert(@sizeOf(Status) == 24);
    std.debug.assert(@offsetOf(Status, "ip") == 4);
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
///
/// It lives in the pure core rather than in one membrane because both
/// membranes need it: the facade validates a candidate table row by row, and
/// the ESP32-C6 backend defines one.
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

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Backend) == ptr * 10);
    std.debug.assert(@sizeOf(Config) == ptr * 4);
    std.debug.assert(@offsetOf(Config, "ip_bind") == ptr * 2);
}

/// A provider that answered with address 0.0.0.0 has not bound anything.
pub fn leaseBound(ip: u32) bool {
    return ip != 0;
}

/// `wifi->state >= k_ra8_wifi_state_associated`.
pub fn isAssociated(state: u8) bool {
    return state >= @intFromEnum(State.associated);
}

/// `wifi->state == k_ra8_wifi_state_ip_bound`.
pub fn isIpBound(state: u8) bool {
    return state == @intFromEnum(State.ip_bound);
}

/// Where one link reading leaves a polling handle.
pub fn stateForLink(link: u8) u8 {
    return if (link == @intFromEnum(Link.up))
        @intFromEnum(State.associated)
    else
        @intFromEnum(State.down);
}

/// Verdict once the whole join budget is spent.
///
/// Nothing answered across the entire budget means the radio is absent rather
/// than slow, so the fault it kept reporting is the honest answer; otherwise
/// the link simply never came up and that is a timeout.
pub fn waitVerdict(answered: bool, last_fault: u16) u16 {
    return if (!answered) last_fault else err_timeout;
}

/// The status record `ra8_wifi_status` derives from a handle.
pub fn statusFrom(state: u8, rssi: i8, lease: Lease) Status {
    return .{
        .state = state,
        .associated = isAssociated(state),
        .ip_bound = isIpBound(state),
        .rssi = rssi,
        .ip = lease,
    };
}

/// One row of `ra8_wifi_backend_t`, in the order the C validates them.
pub const Row = enum {
    open,
    close,
    radio_up,
    radio_down,
    join,
    leave,
    service,
    get_mac,
    get_ap,
    idle,

    /// The message `RA8_CHECK_NULL_PTR` logged for this row.
    pub fn message(self: Row) [*:0]const u8 {
        return switch (self) {
            .open => "backend.open",
            .close => "backend.close",
            .radio_up => "backend.radio_up",
            .radio_down => "backend.radio_down",
            .join => "backend.join",
            .leave => "backend.leave",
            .service => "backend.service",
            .get_mac => "backend.get_mac",
            .get_ap => "backend.get_ap",
            .idle => "backend.idle",
        };
    }
};

/// Which rows of a candidate table are filled in.
pub const RowPresence = struct {
    open: bool = true,
    close: bool = true,
    radio_up: bool = true,
    radio_down: bool = true,
    join: bool = true,
    leave: bool = true,
    service: bool = true,
    get_mac: bool = true,
    get_ap: bool = true,
    idle: bool = true,
};

/// The first missing row, in the C's three role-sized passes: lifecycle, then
/// session, then query. Stopping at the first gap is what lets the error name
/// the row rather than only reporting that the table is wrong.
pub fn missingRow(present: RowPresence) ?Row {
    if (!present.open) return .open;
    if (!present.close) return .close;
    if (!present.radio_up) return .radio_up;
    if (!present.radio_down) return .radio_down;
    if (!present.join) return .join;
    if (!present.leave) return .leave;
    if (!present.service) return .service;
    if (!present.get_mac) return .get_mac;
    if (!present.get_ap) return .get_ap;
    if (!present.idle) return .idle;
    return null;
}

// --- The ESP32-C6 backend's own core (`ra8_wifi_c6link.c`) -----------------
//
// The backend is one thin mapping per facade operation, so the only judgement
// it owns is what an announcement does to its two latches and what those
// latches then mean. That judgement lives here, testable with no radio, no
// link and no transport.

/// Octets in a link address on the `ra8_c6link` side (`k_ra8_c6link_mac_bytes`).
pub const c6_mac_bytes: usize = 6;
/// Longest SSID `ra8_c6link` carries (`k_ra8_c6link_ssid_max`).
pub const c6_ssid_max: usize = 32;
/// Longest WPA passphrase `ra8_c6link` carries (`k_ra8_c6link_pass_max`).
pub const c6_pass_max: usize = 64;
/// Smallest decode arena `ra8_c6link_open` accepts (`k_ra8_c6link_arena_min`).
pub const c6_arena_min: u32 = 2048;
/// Transactions one announcement pump is allowed (`k_ra8_c6link_announce_transfers`).
pub const c6_announce_transfers: u16 = 8;

/// Rejection code for an arena below the floor (`k_ra8_err_invalid_size`).
pub const err_invalid_size: u16 = 0x0105;

comptime {
    // The straight copies in `get_mac` and `get_ap` depend on these, exactly
    // as the C's two `static_assert`s did.
    std.debug.assert(c6_mac_bytes == mac_bytes);
    std.debug.assert(c6_ssid_max == ssid_max);
}

/// Which announcement arrived (`ra8_c6link_event_kind_t`).
pub const EventKind = enum(u8) {
    boot = 0,
    sta_connected = 1,
    sta_disconnected = 2,
    wifi = 3,
};

/// The station state the backend's event callback latches for `service` to
/// report. Boot and bare Wi-Fi announcements are informational, so they leave
/// every field alone rather than clearing a latch a later poll still owes the
/// facade.
pub const Latches = struct {
    connected: bool = false,
    disconnected: bool = false,
    reason: u16 = 0,

    /// The cleared state `join` restores before asking for an association.
    pub const clear: Latches = .{};

    /// What one announcement does to the latches.
    pub fn latch(self: Latches, kind: u8, reason: u16) Latches {
        if (kind == @intFromEnum(EventKind.sta_connected)) {
            return .{ .connected = true, .disconnected = self.disconnected, .reason = self.reason };
        }
        if (kind == @intFromEnum(EventKind.sta_disconnected)) {
            return .{ .connected = self.connected, .disconnected = true, .reason = reason };
        }
        return self;
    }
};

/// The association reading `service` reports from the latches.
///
/// Down unless a connect was seen, and a later disconnect wins outright. Two
/// single-condition tests applied in that order, so the precedence lives in
/// the sequence rather than in a compound decision that would then owe MC/DC
/// vectors -- the C's own comment, and its own shape.
pub fn linkForLatches(connected: bool, disconnected: bool) Link {
    var link = Link.down;
    if (connected) {
        link = Link.up;
    }
    if (disconnected) {
        link = Link.down;
    }
    return link;
}

/// `cfg->arena_bytes < (uint32_t)k_ra8_c6link_arena_min`.
pub fn arenaTooSmall(bytes: u32) bool {
    return bytes < c6_arena_min;
}
