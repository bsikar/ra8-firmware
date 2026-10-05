//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI DSI-2 command-mode convenience routing (RA8FW-689), formerly the
//! tail of ra8_mipi_dsi_dispatch.c. HUM Ch 65 "Command-mode packet TX"
//! pp 3839-3934: short writes pack up to two bytes into the packet header,
//! longer writes stage the payload as a long packet. Every send targets
//! virtual channel 0. `tx` supplies short(dt, vc, p0, p1), long(dt, vc,
//! data, len, low_power) and err(msg), so the routing is host-testable.

pub const ok: u16 = 0;
pub const err_null_ptr: u16 = 0x504;
pub const vc0: u8 = 0;
pub const lane_all: u8 = 0b11;
pub const short_payload_max: u16 = 2;

/// Two-parameter DCS short write; a null params pointer logs once.
pub fn sendShort(tx: anytype, dt: u8, params: ?*const [2]u8) u16 {
    const p = params orelse {
        tx.err("params must not be nullptr");
        return err_null_ptr;
    };
    return tx.short(dt, vc0, p[0], p[1]);
}

/// Long write in high-speed mode on VC0.
pub fn sendLong(tx: anytype, dt: u8, payload: ?[*]const u8, len: u16) u16 {
    if (len > 0 and payload == null) return err_null_ptr;
    return tx.long(dt, vc0, payload, len, false);
}

/// Short packet (zero padded) up to two bytes, else long packet via LP escape.
pub fn sendPayload(tx: anytype, dt: u8, payload: ?[*]const u8, len: u16) u16 {
    if (len > 0 and payload == null) return err_null_ptr;
    if (len <= short_payload_max) {
        const p0: u8 = if (len > 0) payload.?[0] else 0;
        const p1: u8 = if (len > 1) payload.?[1] else 0;
        return tx.short(dt, vc0, p0, p1);
    }
    return tx.long(dt, vc0, payload, len, true);
}
