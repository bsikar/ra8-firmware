//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the BLE HCI H4 transport plane: `ra8_c6link_set_hci_rx` and
//! `ra8_c6link_hci_send`, as `ra8_c6link.h` declares them.

const std = @import("std");
const tx_admit = @import("internal/tx_admit.zig");
const rx_route = @import("internal/rx_route.zig");
const Err = @import("abi_err.zig");
const header = @import("c6link_c.zig");

/// The public `ra8_c6link.h` view of the handle.
pub const c = header.c;

extern fn priv_c6link_pump(link: ?*c.ra8_c6link_t, max_transactions: u16, stats: ?*c.ra8_c6link_stats_t) callconv(.c) u16;

comptime {
    if (tx_admit.HciBound.max != c.k_ra8_c6link_hci_max_packet) {
        @compileError("hci_max_packet drifted");
    }
}

/// BLE HCI traffic rides on `ESP_HCI_IF`.
const send_if: u8 = rx_route.If.hci;

/// Admission check guarding the H4 indicator read until bounds pass.
fn admitted(handle: *const c.ra8_c6link_t, bytes: [*]const u8, len: u16) u16 {
    if (!handle.open) return Err.not_initialized;
    if (len < tx_admit.HciBound.min or len > tx_admit.HciBound.max) return Err.invalid_size;
    tx_admit.admitHci(handle.open, len, bytes[0], handle.tx_len) catch |refusal| return switch (refusal) {
        error.NotInitialized => Err.not_initialized,
        error.InvalidSize => Err.invalid_size,
        error.InvalidArg => Err.invalid_arg,
        error.Busy => Err.busy,
    };
    return Err.ok;
}

/// `ra8_c6link_set_hci_rx`: attach or detach the inbound H4 HCI callback.
pub export fn ra8_c6link_set_hci_rx(
    link: ?*c.ra8_c6link_t,
    cb: c.ra8_c6link_rx_cb_t,
    ctx: ?*anyopaque,
) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    if (!handle.open) return Err.not_initialized;
    handle.hci_rx_cb = cb;
    handle.hci_ctx = if (cb != null) ctx else null;
    return Err.ok;
}

/// `ra8_c6link_hci_send`: stage one H4 HCI packet and clock it out.
///
/// The H4 type indicator (`h4_pkt[0]`) goes into `esp_payload_header.hci_pkt_type`
/// (octet 11) and `h4_pkt[1..len]` is copied to payload offset 12, matching
/// esp-hosted's `slave_bt.c` TX/RX expectations.
pub export fn ra8_c6link_hci_send(
    link: ?*c.ra8_c6link_t,
    h4_pkt: ?[*]const u8,
    len: u16,
) callconv(.c) c.ra8_err_t {
    const handle = link orelse return Err.null_ptr;
    const bytes = h4_pkt orelse return Err.null_ptr;
    const verdict = admitted(handle, bytes, len);
    if (verdict != Err.ok) return verdict;

    const at = c.k_ra8_c6link_header_bytes;
    const body_len: u16 = len - 1;
    @memcpy(handle.tx[at .. at + body_len], bytes[1..len]);
    handle.tx_len = body_len;
    handle.tx_if = send_if;
    handle.tx_pkt_type = bytes[0];

    var local = std.mem.zeroes(c.ra8_c6link_stats_t);
    const pumped = priv_c6link_pump(handle, c.k_ra8_c6link_hs_giveup, &local);
    const unsent = handle.tx_len != 0;
    handle.tx_len = 0;
    handle.tx_pkt_type = 0;
    if (pumped != Err.ok) return pumped;
    if (unsent) return Err.hw_timeout;
    return Err.ok;
}
