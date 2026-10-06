//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA RX frame drain (RA8FW-856, was part of ra8_eth_gwca_default.c).
//! Exports live in src/eth_gwca_rx_abi.zig. `hw` supplies
//! findSlot(chain, depth, dt, start, out) and nullPtr(msg).

pub const q = @import("eth_gwca_queue.zig");

/// Copies the slot's frame out and hands the slot back to the DMAC as
/// FEMPTY with DS restored to the full slot size.
pub fn drainSlot(desc: *volatile q.Desc, out: [*]u8, capacity: u32, slot_bytes: u32, out_len: *u32) u16 {
    const buf = q.decodePtr(desc) orelse return q.invalid_arg;
    const ds = q.getDs(desc);
    if (ds > capacity) return q.invalid_arg;
    @memcpy(out[0..ds], buf[0..ds]);
    out_len.* = ds;
    q.setDs(desc, slot_bytes);
    q.setDt(desc, q.dt_fempty);
    return q.ok;
}

/// Finds the next FSINGLE slot from the head cursor, drains it, advances.
pub fn rxFrame(
    hw: anytype,
    chain: ?[*]volatile q.Desc,
    depth: u32,
    head: ?*u32,
    out: ?[*]u8,
    capacity: u32,
    slot_bytes: u32,
    out_len: ?*u32,
) u16 {
    const ch = chain orelse return hw.nullPtr("rx_frame: chain null");
    const h = head orelse return hw.nullPtr("rx_frame: head_idx null");
    const o = out orelse return hw.nullPtr("rx_frame: out_frame null");
    const len = out_len orelse return hw.nullPtr("rx_frame: out_len null");
    if (capacity == 0 or depth < 2) return q.invalid_arg;
    const count = depth - 1;
    var slot: u32 = 0;
    const err = hw.findSlot(ch, depth, q.dt_fsingle, h.* % count, &slot);
    if (err != q.ok) return err;
    const drain = drainSlot(&ch[slot], o, capacity, slot_bytes, len);
    if (drain != q.ok) return drain;
    h.* = (slot + 1) % count;
    return q.ok;
}
