//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA default send on the extended-descriptor TX ring (RA8FW-858, was part
//! of ra8_eth_gwca_default.c). Exports live in src/eth_gwca_send_abi.zig.
//! `hw` supplies dsb(), reloadQueue(qi), kickTx(qi), txDone(desc, iter),
//! nullPtr(msg) and logError(msg).

const std = @import("std");
const recv = @import("eth_gwca_recv.zig");
pub const q = recv.q;
pub const DefaultState = recv.DefaultState;
pub const ExtDesc = q.ExtDesc;

/// `k_ra8_gwca_info1_tx_fmt_direct` and the DV field (ra8_ether_regs.h).
pub const info1_fmt_direct: u32 = 1 << 2;
pub const info1_dv_shift: u5 = 16;
pub const info1_dv_mask: u32 = 0x7F << 16;
/// `k_ra8_eth_gwca_tx_done_spin`.
pub const tx_done_spin: u32 = 2_000_000;
pub const dt_fsingle: u8 = 8;
pub const hw_timeout: u16 = 0x203;

/// INFO1[63:32] for a frame to one MAC port: DV = 1 << port. Ports past the
/// 7-bit DV field give 0 (the C shift was undefined at 32 and up).
pub fn info1Hi(mac_port: u8) u32 {
    const dv = std.math.shl(u32, 1, mac_port);
    return (dv << info1_dv_shift) & info1_dv_mask;
}

/// When the GWCA idle-disabled the TX queue (terminator LEMPTY), LINK the
/// terminator back to chain[0] and reload the queue (result ignored).
pub fn rearmExt(hw: anytype, chain: [*]volatile ExtDesc, depth: u32, qi: u32) void {
    const term = &chain[depth - 1].base;
    if (q.getDt(term) != recv.dt_lempty) return;
    q.setLinkfixEntry(term, @intFromPtr(&chain[0]));
    q.setDt(term, recv.dt_link);
    _ = hw.reloadQueue(qi);
}

/// Spin until the GWCA writes slot 0 back (DT leaves FSINGLE).
pub fn waitTx0(hw: anytype, d: *volatile ExtDesc) u16 {
    var i: u32 = 0;
    while (i < tx_done_spin) : (i += 1) {
        if (hw.txDone(d, i)) return q.ok;
    }
    hw.logError("default_send: TX completion timeout");
    return hw_timeout;
}

pub fn send(hw: anytype, state: ?*DefaultState, frame: ?[*]const u8, len: u32) u16 {
    const s = state orelse return hw.nullPtr("default_send: state null");
    const f = frame orelse return hw.nullPtr("default_send: frame null");
    if (len == 0 or len > s.tx_slot_bytes) return q.invalid_arg;
    const chain = s.tx_chain.?;
    rearmExt(hw, chain, s.tx_depth, s.tx_queue_index);
    const d = &chain[0];
    @memcpy(s.tx_pool.?[0..len], f[0..len]);
    q.setDs(&d.base, len);
    d.info1_lo = info1_fmt_direct;
    d.info1_hi = info1Hi(s.mac_port);
    q.setDt(&d.base, dt_fsingle);
    s.tx_tail = 0;
    hw.dsb();
    const reload_err = hw.reloadQueue(s.tx_queue_index);
    if (reload_err != q.ok) return reload_err;
    const kick_err = hw.kickTx(s.tx_queue_index);
    if (kick_err != q.ok) return kick_err;
    return waitTx0(hw, d);
}
