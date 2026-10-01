//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! How this host asks the co-processor to associate as a station.
//!
//! Four selectors cross the link as plain integers belonging to four
//! different ESP-IDF enumerations, and every one of them answers zero today.
//! That coincidence is the reason they are named and tested rather than
//! written as literals at the call site: they are four separate decisions
//! that would stop agreeing the moment one enumeration gained a member.

const field_copy = @import("field_copy.zig");

/// The co-processor-side numbering the station requests transmit.
pub const Wire = struct {
    /// `WIFI_IF_STA`: the interface index `Req_WifiSetConfig` configures.
    ///
    /// Also what `Req_GetMACAddress.mode` selects. That field is named for
    /// `wifi_mode_t`, but the co-processor hands it to
    /// `esp_wifi_get_mac(wifi_interface_t, ...)`, so it is an interface index.
    /// `WIFI_IF_AP` (1) answers with the SoftAP address, the station address
    /// plus one, which never associates: stamping frames with it is why an
    /// associated station could not finish DHCP.
    pub const iface_sta: i32 = 0;

    /// `WIFI_FAST_SCAN`: stop at the first acceptable AP.
    pub const scan_fast: i32 = 0;

    /// `WIFI_CONNECT_AP_BY_SIGNAL`: strongest candidate first.
    pub const sort_signal: i32 = 0;

    /// `WIFI_AUTH_OPEN` as a threshold: impose no minimum.
    pub const auth_open: i32 = 0;

    /// Protected management frames are supported but not demanded.
    pub const pmf_capable: i32 = 1;
};

/// The selectors one `Req_WifiSetConfig` carries, as the C side reads them.
pub const Policy = extern struct {
    iface: i32,
    scan_method: i32,
    sort_method: i32,
    auth_threshold: i32,
    pmf_capable: i32,
};

/// The association policy this host sends for every station join.
pub fn policy() Policy {
    return .{
        .iface = Wire.iface_sta,
        .scan_method = Wire.scan_fast,
        .sort_method = Wire.sort_signal,
        .auth_threshold = Wire.auth_open,
        .pmf_capable = Wire.pmf_capable,
    };
}

/// Octets of BSSID to transmit, given whether the caller pinned one.
///
/// Zero when none was pinned. Sending the field at full length regardless
/// would pin the association to the all-zero address, which no AP answers.
pub fn bssidLen(pinned: bool) usize {
    return if (pinned) field_copy.Bound.mac_octets else 0;
}
