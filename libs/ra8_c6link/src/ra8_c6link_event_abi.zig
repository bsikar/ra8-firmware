//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for `priv_c6link_rpc_event`: turn one decoded `Event_*` message into
//! a first-party `ra8_c6link_event_t` and hand it to `priv_c6link_emit`. Five
//! announcements are modelled; every other event id is ignored rather than
//! half-decoded into a record no caller can interpret. The rest of the RPC
//! layer stays in C for now (RA8FW-646).

const header = @import("c6link_rpc_c.zig");

pub const c = header.c;

/// Room in the record's SSID buffer, terminator included.
const ssid_cap: u8 = @sizeOf(@FieldType(c.ra8_c6link_event_t, "ssid"));

/// Which AP the station reached, by the association's own account. A null
/// body leaves the record with its kind only.
fn connected(ev: *c.ra8_c6link_event_t, body: [*c]const c.WifiEventStaConnected) void {
    if (body == null) return;
    ev.ssid_len = c.priv_c6link_copy_str(&ev.ssid, ssid_cap, &body.*.ssid);
    ev.channel = @truncate(body.*.channel);
    _ = c.priv_c6link_copy_mac(&ev.bssid, &body.*.bssid);
}

/// The AP the station lost and the 802.11 reason code, which is how a
/// vanished AP is told from a rejected passphrase.
fn disconnected(ev: *c.ra8_c6link_event_t, body: [*c]const c.WifiEventStaDisconnected) void {
    if (body == null) return;
    ev.ssid_len = c.priv_c6link_copy_str(&ev.ssid, ssid_cap, &body.*.ssid);
    ev.reason = @truncate(body.*.reason);
    ev.rssi = @truncate(body.*.rssi);
    _ = c.priv_c6link_copy_mac(&ev.bssid, &body.*.bssid);
}

/// Fill `ev` for a modelled announcement; false for any other event id.
fn decode(ev: *c.ra8_c6link_event_t, msg: *const c.Rpc) bool {
    const p = msg.unnamed_0;
    switch (msg.msg_id) {
        c.RPC_ID__Event_ESPInit => {
            ev.kind = c.k_ra8_c6link_event_boot;
            if (p.event_esp_init != null) ev.reset_reason = p.event_esp_init.*.cp_reset_reason;
        },
        c.RPC_ID__Event_StaConnected => {
            ev.kind = c.k_ra8_c6link_event_sta_connected;
            if (p.event_sta_connected != null) connected(ev, p.event_sta_connected.*.sta_connected);
        },
        c.RPC_ID__Event_StaDisconnected => {
            ev.kind = c.k_ra8_c6link_event_sta_disconnected;
            if (p.event_sta_disconnected != null) disconnected(ev, p.event_sta_disconnected.*.sta_disconnected);
        },
        c.RPC_ID__Event_WifiEventNoArgs => {
            ev.kind = c.k_ra8_c6link_event_wifi;
            if (p.event_wifi_event_no_args != null) ev.wifi_event_id = p.event_wifi_event_no_args.*.event_id;
        },
        c.RPC_ID__Event_StaScanDone => ev.kind = c.k_ra8_c6link_event_scan_done,
        else => return false,
    }
    return true;
}

/// `priv_c6link_rpc_event`: decode one announcement and emit it exactly once.
/// A null link or message, or an unmodelled event id, emits nothing.
pub export fn priv_c6link_rpc_event(link: ?*c.ra8_c6link_t, msg_v: ?*const anyopaque) callconv(.c) void {
    const handle = link orelse return;
    const msg: *const c.Rpc = @ptrCast(@alignCast(msg_v orelse return));
    if (msg.msg_id == c.RPC_ID__Event_StaScanDone) handle.scan_done = true;
    var ev = std.mem.zeroes(c.ra8_c6link_event_t);
    if (decode(&ev, msg)) c.priv_c6link_emit(handle, &ev);
}

const std = @import("std");
