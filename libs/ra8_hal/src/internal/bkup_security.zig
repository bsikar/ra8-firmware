//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Battery-backup security attribution (HUM Ch 12.2): BBFSAR and the
//! VBTBKR secure/privileged boundaries. Port of ra8_bkup_security.c
//! (RA8FW-555). The register accessors keep the C names so the attribution
//! checker's accessor rule sees every store inside the PRC4 window.

const std = @import("std");
const prcr = @import("prcr.zig");
/// Re-exported for the host tests (the test module sees only this root).
pub const prcr_mod = prcr;

/// SYSC + battery backup base (`k_ra8_bkup_base_addr`).
pub const base: usize = 0x4001E000;
pub const off_vbrsabar: usize = 0x3B0;
pub const off_vbrpabars: usize = 0x3B4;
pub const off_vbrpabarns: usize = 0x3B8;
pub const off_bbfsar: usize = 0x3D0;
/// PRCR sits in the same R_SYSTEM block (`k_ra8_sys_off_prcr`).
pub const off_prcr: usize = prcr.addr - base;
/// Bytes a host test buffer must cover.
pub const window_len: usize = off_prcr + 2;

/// All NONSEC bits (`k_ra8_bkup_bbfsar_mask_all`).
pub const bbfsar_mask_all: u32 = 0x7F;
/// Boundaries are 32-byte aligned (`k_ra8_bkup_saba_align_mask`).
pub const saba_align_mask: u16 = 0x1F;
/// Last 32-byte slot (`k_ra8_bkup_saba_max`).
pub const saba_max: u16 = 0xFFE0;

/// `ra8_bkup_security_config_t`.
pub const Config = extern struct {
    bbfsar: u32,
    saba: u16,
    pabas: u16,
    pabans: u16,
};

comptime {
    std.debug.assert(@sizeOf(Config) == 12);
    std.debug.assert(@offsetOf(Config, "saba") == 4);
    std.debug.assert(@offsetOf(Config, "pabas") == 6);
    std.debug.assert(@offsetOf(Config, "pabans") == 8);
}

/// Which field failed validation; the ABI logs each one as the C did.
pub const Error = error{ BadBbfsar, BadSaba, BadPabas, BadPabans };

/// The R_SYSTEM block, by base address so host tests can use a buffer.
pub const Block = struct {
    base: usize,

    fn reg(block: Block, comptime T: type, off: usize) *volatile T {
        return @ptrFromInt(block.base + off);
    }

    // Accessors keep the C names and take no arguments at the call site
    // (`block.ra8_bkup_bbfsar().* = v`), which is the shape
    // check_attribution_gates.py's Zig accessor rule matches.
    pub fn ra8_bkup_bbfsar(block: Block) *volatile u32 {
        return block.reg(u32, off_bbfsar);
    }

    pub fn ra8_bkup_vbrsabar(block: Block) *volatile u16 {
        return block.reg(u16, off_vbrsabar);
    }

    pub fn ra8_bkup_vbrpabars(block: Block) *volatile u16 {
        return block.reg(u16, off_vbrpabars);
    }

    pub fn ra8_bkup_vbrpabarns(block: Block) *volatile u16 {
        return block.reg(u16, off_vbrpabarns);
    }

    pub fn prcrReg(block: Block) *volatile u16 {
        return block.reg(u16, off_prcr);
    }
};

/// A boundary is 32-byte aligned and no higher than the last slot.
pub fn validBoundary(addr: u16) bool {
    return (addr & saba_align_mask) == 0 and addr <= saba_max;
}

/// `internal_validate_security_cfg`.
pub fn validate(cfg: Config) Error!void {
    if ((cfg.bbfsar & ~bbfsar_mask_all) != 0) return error.BadBbfsar;
    if (!validBoundary(cfg.saba)) return error.BadSaba;
    if (!validBoundary(cfg.pabas)) return error.BadPabas;
    if (!validBoundary(cfg.pabans)) return error.BadPabans;
}

/// Validate, then write all four registers inside a PRC4 (SAR) window.
/// BBFSAR and the boundaries sit behind PRC4, not the PRC1 that guards
/// the rest of the block (HUM Ch 13.1 Table 13.1 p 521).
pub fn apply(block: Block, cfg: Config) Error!void {
    try validate(cfg);
    const window = prcr.open(block.prcrReg(), prcr.unlock_sar);
    defer window.close();
    block.ra8_bkup_bbfsar().* = cfg.bbfsar & bbfsar_mask_all;
    block.ra8_bkup_vbrsabar().* = cfg.saba;
    block.ra8_bkup_vbrpabars().* = cfg.pabas;
    block.ra8_bkup_vbrpabarns().* = cfg.pabans;
}

/// Read the four registers back, BBFSAR masked to its NONSEC bits.
pub fn get(block: Block) Config {
    return .{
        .bbfsar = block.ra8_bkup_bbfsar().* & bbfsar_mask_all,
        .saba = block.ra8_bkup_vbrsabar().*,
        .pabas = block.ra8_bkup_vbrpabars().*,
        .pabans = block.ra8_bkup_vbrpabarns().*,
    };
}
