//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GWCA operation-mode change and AXI init (RA8FW-848, was part of
//! ra8_eth_gwca.c). Exports live in src/eth_gwca_mode_abi.zig. `hw` supplies
//! eval(reg, iter, cond) (the fake-MMIO seam on host builds) and logError.

/// GWMC.OPC and GWMS.OPS, bits [1:0].
pub const opc_mask: u32 = 0x3;
/// GWARIRM request (ARIOG) and response (ARR) bits.
pub const gwarirm_ariog: u32 = 1 << 0;
pub const gwarirm_arr: u32 = 1 << 1;
/// `k_ra8_eth_gwca_mode_spin`: GWMS.OPS / GWARIRM.ARR poll budget.
pub const mode_spin: u32 = 2_000_000;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const hw_timeout: u16 = 0x203;

/// Write GWMC.OPC = mode, then wait for GWMS.OPS to read back the same value.
pub fn setMode(hw: anytype, gwmc: *volatile u32, gwms: *volatile u32, mode: u32) u16 {
    if (mode > opc_mask) return invalid_arg;
    gwmc.* = (gwmc.* & ~opc_mask) | mode;
    var i: u32 = 0;
    while (i < mode_spin) : (i += 1) {
        if (hw.eval(gwms, i, (gwms.* & opc_mask) == mode)) return ok;
    }
    hw.logError("set_operation_mode: GWMS.OPS never converged");
    return hw_timeout;
}

/// Request AXI RAM init (ARIOG), then wait for ARR.
pub fn axiInit(hw: anytype, gwarirm: *volatile u32) u16 {
    gwarirm.* = gwarirm_ariog;
    var i: u32 = 0;
    while (i < mode_spin) : (i += 1) {
        if (hw.eval(gwarirm, i, (gwarirm.* & gwarirm_arr) != 0)) return ok;
    }
    hw.logError("axi_init: GWARIRM.ARR never asserted");
    return hw_timeout;
}
