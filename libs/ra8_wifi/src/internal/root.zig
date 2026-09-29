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
