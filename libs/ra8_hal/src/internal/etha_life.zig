//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ETHA lifecycle and mode control (RA8FW-817, the last of ra8_etha.c).
//! Pure: one port's ETHA block comes in as a `regs` value (read32/write32 by
//! offset). The EAMS poll, MSTP gate, slots and checks live in
//! src/etha_life_abi.zig.

const irq = @import("etha_irq.zig");

pub const off_eamc: usize = 0x00;
pub const off_eams: usize = 0x04;
pub const mask_opc: u32 = 0x3;
pub const mask_ops: u32 = 0x3;

pub const opc_reset: u32 = 0;
pub const opc_disable: u32 = 1;
pub const opc_config: u32 = 2;
pub const opc_operation: u32 = 3;

/// `ra8_etha_config_t`.
pub const Config = extern struct { initial_mode: u8, eaeie0_mask: u32, eaeie1_mask: u32, eaeie2_mask: u32 };

/// Initial mode, then every error source disabled, then the cfg enables
/// (HUM 32.3.1.1 p 1630, 32.3.7.1.2-9 p 1661-1668).
pub fn init(regs: anytype, cfg: *const Config) void {
    regs.write32(off_eamc, cfg.initial_mode & mask_opc);
    for (0..irq.block_count) |b| regs.write32(irq.eid(@intCast(b)), 0xFFFF_FFFF);
    const masks = [_]u32{ cfg.eaeie0_mask, cfg.eaeie1_mask, cfg.eaeie2_mask };
    for (masks, 0..) |m, b| regs.write32(irq.eie(@intCast(b)), m);
}

/// RESET, then every error enable cleared. The MSTP gate stays on: the rest
/// of the Ethernet subsystem shares it and ra8_eth_deinit drops it.
pub fn deinit(regs: anytype) void {
    regs.write32(off_eamc, opc_reset);
    for (0..irq.block_count) |b| regs.write32(irq.eie(@intCast(b)), 0);
}

pub fn reset(regs: anytype) void {
    regs.write32(off_eamc, opc_reset);
    regs.write32(off_eamc, opc_config);
}

/// Only CONFIG and DISABLE are polled for: MRMAC writes need the port in
/// CONFIG, and OPERATION -> DISABLE must land before the next CONFIG. RESET
/// and OPERATION are fire-and-forget (EAMS never leaves CONFIG on an unwired
/// port, so polling there would hang the bench loopback test).
pub fn waits(mode: u8) bool {
    return mode == opc_config or mode == opc_disable;
}
