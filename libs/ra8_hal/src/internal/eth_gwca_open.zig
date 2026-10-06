//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA default open: rings, queues, bring-up and the J-Link step trails
//! (RA8FW-859, was ra8_eth_gwca_default.c). Exports live in
//! src/eth_gwca_open_abi.zig. `hw` supplies init(), initRing(...),
//! attachBuffers(...), bringUp(...), setMode(opc), configureQueue(...),
//! reloadQueue(qi), setPreStep(v), setOpenStep(v), nullPtr(msg) and
//! fail(msg, code), which logs msg plus error_val "Error" and returns code.

const recv = @import("eth_gwca_recv.zig");
pub const q = recv.q;
pub const DefaultState = recv.DefaultState;
pub const ExtDesc = q.ExtDesc;

pub const dt_fempty: u8 = 4;
/// The DS field is 12 bits, so a TX slot is at most 2048 bytes.
pub const tx_ext_max_bytes: u32 = 2048;
/// `k_ra8_gwmc_opc_config` and `k_ra8_gwmc_opc_operation`.
pub const opc_config: u32 = 2;
pub const opc_operation: u32 = 3;

/// `k_ra8_eth_gwca_step_fail_N`.
pub fn stepFail(n: u32) u32 {
    return 0x10 | n;
}

/// Prime the 16-byte TX chain: depth-1 FEMPTY slots of `slot` bytes each
/// pointing into `pool`, then a LINK terminator back to chain[0].
pub fn txExtInit(hw: anytype, chain: ?[*]volatile ExtDesc, depth: u32, slot: u32, pool: ?[*]u8) u16 {
    const c = chain orelse return hw.nullPtr("tx_ext_init: chain null");
    const p = pool orelse return hw.nullPtr("tx_ext_init: pool null");
    if (depth < 2 or slot > tx_ext_max_bytes) return q.invalid_arg;
    var i: u32 = 0;
    while (i < depth - 1) : (i += 1) {
        c[i] = .{};
        q.setLinkfixEntry(&c[i].base, @intFromPtr(p) + @as(usize, i) * slot);
        q.setDt(&c[i].base, dt_fempty);
        q.setDs(&c[i].base, slot);
    }
    const term = &c[depth - 1];
    term.* = .{};
    q.setLinkfixEntry(&term.base, @intFromPtr(&c[0]));
    q.setDt(&term.base, recv.dt_link);
    return q.ok;
}

fn head(p: anytype) ?*anyopaque {
    return if (p) |v| @ptrCast(@volatileCast(v)) else null;
}

pub fn openRings(hw: anytype, s: *DefaultState) u16 {
    var err = hw.initRing(s.rx_chain, s.rx_depth, s.rx_slot_bytes);
    if (err != q.ok) return hw.fail("default_open: rx init_ring", err);
    err = hw.attachBuffers(s.rx_chain, s.rx_depth, s.rx_slot_bytes, s.rx_pool);
    if (err != q.ok) return hw.fail("default_open: rx attach", err);
    return txExtInit(hw, s.tx_chain, s.tx_depth, s.tx_slot_bytes, s.tx_pool);
}

pub fn openQueues(hw: anytype, s: *DefaultState) u16 {
    const rx_cfg = q.QueueCfg{ .chain_head = head(s.rx_chain) };
    const err = hw.configureQueue(s.linkfix_table, s.rx_queue_index, &rx_cfg);
    if (err != q.ok) return hw.fail("default_open: rx config", err);
    const tx_cfg = q.QueueCfg{ .is_tx = true, .extended = true, .chain_head = head(s.tx_chain) };
    return hw.configureQueue(s.linkfix_table, s.tx_queue_index, &tx_cfg);
}

/// One pre-phase step: run it, then record ok N or fail N on the pre trail.
fn preStep(hw: anytype, n: u32, err: u16) u16 {
    hw.setPreStep(if (err == q.ok) n else stepFail(n));
    return err;
}

pub fn pre(hw: anytype, s: *DefaultState) u16 {
    hw.setPreStep(0);
    var err = preStep(hw, 1, hw.init());
    if (err != q.ok) return err;
    err = preStep(hw, 2, openRings(hw, s));
    if (err != q.ok) return err;
    err = preStep(hw, 3, hw.bringUp(s.linkfix_table, s.linkfix_count));
    if (err != q.ok) return err;
    return preStep(hw, 4, hw.setMode(opc_config));
}

fn openStep(hw: anytype, n: u32, err: u16) u16 {
    if (err != q.ok) hw.setOpenStep(stepFail(n));
    return err;
}

pub fn open(hw: anytype, state: ?*DefaultState) u16 {
    const s = state orelse return hw.nullPtr("default_open: state null");
    hw.setOpenStep(0);
    var err = openStep(hw, 1, pre(hw, s));
    if (err != q.ok) return err;
    hw.setOpenStep(1);
    err = openStep(hw, 2, openQueues(hw, s));
    if (err != q.ok) return err;
    hw.setOpenStep(2);
    s.rx_head = 0;
    s.tx_tail = 0;
    err = openStep(hw, 3, hw.setMode(opc_operation));
    if (err != q.ok) return err;
    hw.setOpenStep(3);
    err = openStep(hw, 3, hw.reloadQueue(s.rx_queue_index));
    if (err != q.ok) return err;
    return openStep(hw, 3, hw.reloadQueue(s.tx_queue_index));
}
