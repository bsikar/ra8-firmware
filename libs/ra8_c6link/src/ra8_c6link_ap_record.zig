//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One `WifiApRecord` copied into a first-party `ra8_c6link_ap_info_t`. The
//! associated-AP query and the scan both decode this record, so they share
//! one copy and the two can't drift apart.

const field = @import("ra8_c6link_field_abi.zig");
const header = @import("c6link_rpc_c.zig");

pub const c = header.c;

/// Fill `out` from `rec`. Oversized text and short addresses are handled by
/// the field copies, which bound every write to the destination.
pub fn fill(out: *c.ra8_c6link_ap_info_t, rec: *const c.WifiApRecord) void {
    out.ssid_len = field.priv_c6link_copy_str(&out.ssid, out.ssid.len, @ptrCast(&rec.ssid));
    out.channel = @truncate(@as(u32, @bitCast(rec.primary)));
    out.rssi = @truncate(rec.rssi);
    out.authmode = rec.authmode;
    _ = field.priv_c6link_copy_mac(@ptrCast(&out.bssid), @ptrCast(&rec.bssid));
}
