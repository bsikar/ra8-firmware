//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA default receive and RX queue re-arm (RA8FW-857, was part of
//! ra8_eth_gwca_default.c). Exports live in src/eth_gwca_recv_abi.zig.
//! `hw` supplies rxFrame(...), reloadQueue(qi) and nullPtr(msg).

pub const q = @import("eth_gwca_queue.zig");

/// `k_ra8_gwdcc_dt_lempty` (queue disabled) and `k_ra8_gwdcc_dt_link`.
pub const dt_lempty: u8 = 12;
pub const dt_link: u8 = 14;

/// Mirror of `ra8_eth_gwca_default_state_t` (ra8_eth_gwca.h).
pub const DefaultState = extern struct {
    linkfix_table: ?[*]volatile q.Desc,
    linkfix_count: u32,
    rx_chain: ?[*]volatile q.Desc,
    rx_depth: u32,
    rx_pool: ?[*]u8,
    rx_slot_bytes: u32,
    rx_queue_index: u32,
    rx_head: u32,
    tx_chain: ?*anyopaque,
    tx_depth: u32,
    tx_pool: ?[*]u8,
    tx_slot_bytes: u32,
    tx_queue_index: u32,
    tx_tail: u32,
    mac_port: u8,
};

comptime {
    if (@sizeOf(usize) == 4) {
        if (@sizeOf(DefaultState) != 60) @compileError("DefaultState size");
        if (@offsetOf(DefaultState, "rx_queue_index") != 24 or @offsetOf(DefaultState, "rx_head") != 28) @compileError("DefaultState rx");
        if (@offsetOf(DefaultState, "tx_chain") != 32 or @offsetOf(DefaultState, "mac_port") != 56) @compileError("DefaultState tx");
    }
}

/// When the GWCA idle-disabled the queue (terminator LEMPTY), restore the
/// LINK back to chain[0], snap the cursor to 0 and reload the queue.
pub fn rearmIfDisabled(hw: anytype, chain: [*]volatile q.Desc, depth: u32, qi: u32, cursor: *u32) void {
    const term = &chain[depth - 1];
    if (q.getDt(term) != dt_lempty) return;
    q.setLinkfixEntry(term, @intFromPtr(&chain[0]));
    q.setDt(term, dt_link);
    cursor.* = 0;
    _ = hw.reloadQueue(qi);
}

pub fn recv(hw: anytype, state: ?*DefaultState, out: ?[*]u8, capacity: u32, out_len: ?*u32) u16 {
    const s = state orelse return hw.nullPtr("default_recv: state null");
    const err = hw.rxFrame(s.rx_chain, s.rx_depth, &s.rx_head, out, capacity, s.rx_slot_bytes, out_len);
    if (err == q.no_data) rearmIfDisabled(hw, s.rx_chain.?, s.rx_depth, s.rx_queue_index, &s.rx_head);
    return err;
}
