//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA init, deinit and enter_stop (RA8FW-851, the last of ra8_eth_gwca.c).
//! Exports live in src/eth_gwca_life_abi.zig. `ops` supplies mstpEnable,
//! mstpDisable, fail(msg, err), info(msg) and clearHandler.

pub const events = @import("eth_gwca_events.zig");

pub const ok: u16 = 0;

/// FWPCn.DDE, bit 0: extended descriptor format for that agent. Without it
/// the AXI init handshake (GWARIRM.ARR) never asserts.
pub const fwpc_dde: u32 = 0x1;

/// GWCA's four control words plus the three MFWD agent FWPC registers.
pub const View = struct {
    gwca: *volatile events.Regs,
    fwpc: [3]*volatile u32,
};

/// Power the block, clear GWCA control/status, then turn on DDE per agent.
pub fn init(ops: anytype, v: View) u16 {
    const err = ops.mstpEnable();
    if (err != ok) {
        ops.fail("gwca_init: mstp enable", err);
        return err;
    }
    v.gwca.ctrl = 0;
    v.gwca.sts = 0;
    v.gwca.ie = 0;
    v.gwca.iclr = 0;
    for (v.fwpc) |r| r.* = (r.* & ~fwpc_dde) | fwpc_dde;
    ops.info("gwca_init");
    return ok;
}

/// Stop the block, drop the handler, then gate its clock.
pub fn deinit(ops: anytype, v: View) u16 {
    v.gwca.ctrl = 0;
    v.gwca.ie = 0;
    ops.clearHandler();
    return ops.mstpDisable();
}

/// Stop the block and gate its clock; the handler stays attached.
pub fn enterStop(ops: anytype, v: View) u16 {
    v.gwca.ctrl = 0;
    return ops.mstpDisable();
}
