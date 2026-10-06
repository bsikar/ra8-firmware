//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C HDR mode, IBI enable/read and target-mode open (RA8FW-826, the last
//! of ra8_i3c.c). Pure over a `regs` value; the exports live in
//! src/i3c_ibi_abi.zig.

const ccc = @import("i3c_ccc.zig");
const xfer = @import("i3c_xfer.zig");

pub const off_bctl: usize = 0x14;
pub const off_nsdvad: usize = 0xB4;
pub const off_ntibivctl: usize = 0xB8;
pub const off_nibiqp: usize = 0x17C;
const ntst_ibiqeff: u32 = 0x4;
const bctl_slve: u32 = 0x0001_0000;
const bctl_buse: u32 = 0x8000_0000;

pub const hdr_sdr: u8 = 0;
pub const hdr_ddr: u8 = 1;
pub const hdr_ts: u8 = 2;

pub const type_interrupt: u8 = 0;
pub const type_hot_join: u8 = 1;
pub const type_main_request: u8 = 2;
const id_hot_join: u8 = 0x02;

/// `ra8_i3c_ibi_t`.
pub const Ibi = extern struct { address: u8, type: u8, payload_len: u8, payload: [8]u8, last: u8 };

/// Neither SDR, DDR nor TS (`priv_ra8_i3c_internal_hdr_mode_invalid`).
pub fn hdrModeInvalid(sdr: u32, ddr: u32, ts: u32, mode: u32) bool {
    return mode != sdr and mode != ddr and mode != ts;
}

/// Regular-transfer descriptor with the transfer mode in [27:26].
pub fn hdrWord(addr: u8, mode: u8) u32 {
    return xfer.xferWord(addr, false) | (@as(u32, mode & 0x3) << 26);
}

/// One descriptor word only, as the C did.
pub fn setHdr(regs: anytype, addr: u8, mode: u8) void {
    regs.write32(ccc.off_ncmdqp, hdrWord(addr, mode));
    ccc.clearCmdqEmpty(regs);
}

/// NTIBIVCTL.VLCNT = one accepted entry.
pub fn ibiEnable(regs: anytype) void {
    regs.write32(off_ntibivctl, 1);
}

/// Drop BUSE, set the static address valid, then enable SLVE.
pub fn targetOpen(regs: anytype, addr: u8) void {
    regs.write32(off_bctl, regs.read32(off_bctl) & ~bctl_buse);
    regs.write32(off_nsdvad, ((@as(u32, addr) << 16) & 0x007F_0000) | 0x8000_0000);
    regs.write32(off_bctl, regs.read32(off_bctl) | bctl_slve);
}

/// Classify by IBI_ST (bit 31) then the hot-join id.
pub fn ibiType(status: u32) u8 {
    if (status & 0x8000_0000 == 0) return type_interrupt;
    return if (@as(u8, @truncate(status >> 8)) == id_hot_join) type_hot_join else type_main_request;
}

/// False when the IBI queue is empty; otherwise decode one status word and
/// up to eight payload bytes, then clear IBIQEFF.
pub fn ibiRead(regs: anytype, out: *Ibi) bool {
    if (regs.read32(ccc.off_ntst) & ntst_ibiqeff == 0) return false;
    const status = regs.read32(off_nibiqp);
    const id: u8 = @truncate(status >> 8);
    const n: u8 = @min(@as(u8, @truncate(status)), out.payload.len);
    out.address = id >> 1;
    out.type = ibiType(status);
    out.payload_len = n;
    out.payload = .{0} ** 8;
    if (n > 0) ccc.fifoRead(regs, out.payload[0..n]);
    out.last = @truncate((status >> 24) & 1);
    regs.write32(ccc.off_ntst, regs.read32(ccc.off_ntst) & ~ntst_ibiqeff);
    return true;
}
