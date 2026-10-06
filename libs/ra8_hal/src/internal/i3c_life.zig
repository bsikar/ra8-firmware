//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C native bring-up, tear-down and status take (RA8FW-819, was part of
//! ra8_i3c.c). Pure: the I3C block comes in as a `regs` value
//! (read32/write32 by offset); the exports live in src/i3c_life_abi.zig.

const ctl = @import("i3c_ctl.zig");

pub const off_prts: usize = 0x00;
pub const off_rstctl: usize = 0x20;
pub const off_inste: usize = 0x34;
pub const off_inie: usize = 0x38;
pub const off_instfc: usize = 0x3C;

pub const rstctl_ri3crst: u32 = 0x0000_0001;
pub const rstctl_intlrst: u32 = 0x0001_0000;

/// Clocks on, bus off, both software resets pulsed, then every status and
/// enable register zeroed (HUM Ch 40 CECTL/BCTL/RSTCTL). INSTFC is
/// write-only (force), so writing 0 is a clear.
pub fn nativeInit(regs: anytype) void {
    regs.write32(ctl.off_cectl, 1);
    regs.write32(ctl.off_bctl, 0);
    regs.write32(off_rstctl, rstctl_ri3crst);
    regs.write32(off_rstctl, 0);
    regs.write32(off_rstctl, rstctl_intlrst);
    regs.write32(off_rstctl, 0);
    regs.write32(off_prts, 0);
    for ([_]usize{ ctl.off_inst, off_inste, off_inie, off_instfc, ctl.off_msdvad }) |off| regs.write32(off, 0);
}

/// Interrupts and status enables off, then the bus, then the clocks.
pub fn nativeDeinit(regs: anytype) void {
    for ([_]usize{ off_inie, off_inste, ctl.off_bctl, ctl.off_cectl }) |off| regs.write32(off, 0);
}

/// The latched INST flags, cleared in one write.
pub fn takeStatus(regs: anytype) u32 {
    const mask = regs.read32(ctl.off_inst);
    regs.write32(ctl.off_inst, 0);
    return mask;
}
