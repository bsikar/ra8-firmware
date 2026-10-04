//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ETHA ring sizing, traffic counters and port open (RA8FW-591, was
//! ra8_etha_stats.c). Pure: the per-port counters come in as a *Stats, the
//! port's ETHA block through a `regs` value (read32/write32 by offset) and
//! the RMAC PHY steps and logging through an `ops` value.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const hw_timeout: u16 = 0x203;

pub const off_eamc: usize = 0x00;
pub const off_eams: usize = 0x04;
pub const off_eatdqdc: usize = 0x60; // EATDQDCq, 8 x u32
pub const tc_count = 8;
pub const mask_dqd: u16 = 0x7FF;
pub const opc_operation: u32 = 3;
pub const mode_spin: u32 = 200_000;

pub const ring_count_min: u16 = 1;
pub const ring_count_max: u16 = 4096;
pub const ring_buf_min: u16 = 64;
pub const ring_buf_max: u16 = 16383;

/// `ra8_etha_port_stats_t`.
pub const Stats = extern struct {
    tx_ok: u32 = 0,
    tx_err: u32 = 0,
    rx_ok: u32 = 0,
    rx_err: u32 = 0,
    rx_drop: u32 = 0,
    ring_tx: u16 = 0,
    ring_rx: u16 = 0,
    ring_buf: u16 = 0,
    reserved: u16 = 0,
};

/// `ra8_etha_phy_open_t`.
pub const PhyOpen = extern struct { phy_addr: u8, advertise: u16, timeout_ms: u32 };

pub fn ringArgsOk(tx: u16, rx: u16, buf: u16) bool {
    return tx >= ring_count_min and tx <= ring_count_max and
        rx >= ring_count_min and rx <= ring_count_max and
        buf >= ring_buf_min and buf <= ring_buf_max;
}

/// Records the ring geometry and clamps every class's TX queue depth to
/// the host ring (EATDQDC.DQD is 11 bits, HUM 32.3.2.7 p 1636).
pub fn ringInit(stats: *Stats, regs: anytype, ops: anytype, tx: u16, rx: u16, buf: u16) u16 {
    if (!ringArgsOk(tx, rx, buf)) {
        ops.logError("etha_descriptor_ring_init: ring args out of range");
        return invalid_arg;
    }
    stats.ring_tx = tx;
    stats.ring_rx = rx;
    stats.ring_buf = buf;
    const depth: u32 = @min(tx, mask_dqd);
    var i: usize = 0;
    while (i < tc_count) : (i += 1) regs.write32(off_eatdqdc + 4 * i, depth);
    return ok;
}

/// Adds the deltas, each counter saturating at u32 max.
pub fn account(stats: *Stats, tx_ok: u32, tx_err: u32, rx_ok: u32, rx_err: u32, rx_drop: u32) void {
    stats.tx_ok +|= tx_ok;
    stats.tx_err +|= tx_err;
    stats.rx_ok +|= rx_ok;
    stats.rx_err +|= rx_err;
    stats.rx_drop +|= rx_drop;
}

/// EAMC = OPERATION, then poll EAMS.OPS (HUM 32.3.1.1 / 32.3.1.2 p 1631).
pub fn toOperation(regs: anytype, ops: anytype) u16 {
    regs.write32(off_eamc, opc_operation);
    var i: u32 = 0;
    while (i < mode_spin) : (i += 1) {
        if (regs.read32(off_eams) & 3 == opc_operation) return ok;
    }
    ops.logError("etha_to_operation: EAMS never reached OPERATION");
    return hw_timeout;
}

/// OPERATION mode, then PHY reset, advertise, auto-neg start and wait on
/// the RMAC port with the same index.
pub fn open(regs: anytype, ops: anytype, port: u8, phy: *const PhyOpen, out_link: *anyopaque) u16 {
    const op = toOperation(regs, ops);
    if (op != ok) return op;
    var e = ops.phyReset(port, phy.phy_addr);
    if (e != ok) return failed(ops, "etha_open: phy_reset", e);
    e = ops.phySetAdvertise(port, phy.phy_addr, phy.advertise);
    if (e != ok) return failed(ops, "etha_open: set_advertise", e);
    e = ops.phyAutoNegStart(port, phy.phy_addr);
    if (e != ok) return failed(ops, "etha_open: auto_neg_start", e);
    return ops.phyAutoNegWait(port, phy.phy_addr, phy.timeout_ms, out_link);
}

fn failed(ops: anytype, msg: [*:0]const u8, e: u16) u16 {
    ops.logError(msg);
    return e;
}
