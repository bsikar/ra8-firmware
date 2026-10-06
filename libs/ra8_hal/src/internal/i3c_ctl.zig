//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C bus control: dynamic address, BUSE, INST status, stop (RA8FW-818,
//! was part of ra8_i3c.c). Pure: the I3C block comes in as a `regs` value
//! (read32/write32 by offset); the exports live in src/i3c_ctl_abi.zig.

/// R_I3C0 (`k_ra8_i3c0_base_addr`).
pub const base: usize = 0x4035_F000;

pub const off_cectl: usize = 0x10;
pub const off_bctl: usize = 0x14;
pub const off_msdvad: usize = 0x18;
pub const off_inst: usize = 0x30;

pub const bctl_buse: u32 = 0x8000_0000;
pub const msdvad_mdyad_mask: u32 = 0x007F_0000;
pub const msdvad_mdyadv: u32 = 0x8000_0000;
pub const addr_max: u32 = 0x7F;

/// MSDVAD for a 7-bit dynamic address: MDYAD[22:16] plus MDYADV (bit 31).
/// Null when the address does not fit.
pub fn msdvadWord(addr: u32) ?u32 {
    if (addr > addr_max) return null;
    return ((addr << 16) & msdvad_mdyad_mask) | msdvad_mdyadv;
}

/// BCTL.BUSE is bit 31, not bit 0 (HUM Ch 40 BCTL).
pub fn busEnable(regs: anytype, on: bool) void {
    const v = regs.read32(off_bctl);
    regs.write32(off_bctl, if (on) v | bctl_buse else v & ~bctl_buse);
}

/// INST flags clear by writing 0 to the matching bit.
pub fn clearStatus(regs: anytype, mask: u32) void {
    regs.write32(off_inst, regs.read32(off_inst) & ~mask);
}

/// Bus off, then clocks off, before the module stop.
pub fn stop(regs: anytype) void {
    regs.write32(off_bctl, 0);
    regs.write32(off_cectl, 0);
}
