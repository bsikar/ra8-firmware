//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XSPI manual-command core plus RDSR / RDID (RA8FW-869, was part of
//! ra8_xspi_flash.c). Exports live in src/xspi_cmd_abi.zig. Mirrors FSP
//! r_ospi_b_direct_transfer. HUM Ch 44 p 2986.

pub const off_cdctl0: usize = 0x070;
pub const off_cdbuf: usize = 0x080;
pub const off_intc: usize = 0x194;
pub const off_ints: usize = 0x190;
pub const cdctl0_trreq: u32 = 1 << 0;
pub const ints_cmdcmp: u32 = 1 << 0;
/// `k_ra8_xspi_cmd_spin`: CMDCMP poll budget.
pub const cmd_spin: u32 = 50000;
pub const op_read_status: u8 = 0x05;
pub const op_read_id: u8 = 0x9F;
pub const hw_timeout: u16 = 0x203;

/// CDBUF slot 0 words: CDT, CDA, CDD0, CDD1.
pub fn cdbuf(word: usize) usize {
    return off_cdbuf + word * 4;
}

/// CMDSIZE[1:0], ADDSIZE[4:2], DATASIZE[8:5], TRTYPE[15], CMD[31:16]. A
/// 1-byte opcode sits in CMD's upper byte. CMDSIZE 3 encodes CMD = 0 (the
/// C shift was undefined there; no caller uses it).
pub fn makeCdt(opcode: u8, cmd_bytes: u8, addr_bytes: u8, data_bytes: u8, is_write: u8) u32 {
    const size: u32 = cmd_bytes & 0x3;
    const cmd_word: u32 = if (size > 2) 0 else (@as(u32, opcode) << @intCast(8 * (2 - size))) & 0xFFFF;
    return size |
        (@as(u32, addr_bytes & 0x7) << 2) |
        (@as(u32, data_bytes & 0xF) << 5) |
        (@as(u32, is_write & 0x1) << 15) |
        (cmd_word << 16);
}

/// Poll CMDCMP, then clear every pending status bit (INTC = INTS).
pub fn waitDone(regs: anytype) u16 {
    var i: u32 = 0;
    while (i < cmd_spin) : (i += 1) {
        if (regs.poll(i, (regs.read(off_ints) & ints_cmdcmp) != 0)) {
            regs.write(off_intc, regs.read(off_ints));
            return 0;
        }
    }
    return hw_timeout;
}

pub fn kick(regs: anytype) u16 {
    regs.write(off_cdctl0, regs.read(off_cdctl0) | cdctl0_trreq);
    return waitDone(regs);
}

/// 1-byte opcode, no address, `resp_bytes` read back into CDD0/CDD1.
pub fn issue(regs: anytype, opcode: u8, resp_bytes: u8) u16 {
    regs.write(cdbuf(0), makeCdt(opcode, 1, 0, resp_bytes, 0));
    regs.write(cdbuf(1), 0);
    regs.write(cdbuf(2), 0);
    regs.write(cdbuf(3), 0);
    return kick(regs);
}

pub fn readStatus(regs: anytype, out: *u8) u16 {
    const rc = issue(regs, op_read_status, 1);
    if (rc != 0) return rc;
    out.* = @truncate(regs.read(cdbuf(2)));
    return 0;
}

/// JEDEC 0x9F returns MFR, MEMTYPE, CAPACITY in that byte order.
pub fn readId(regs: anytype, out: *u32) u16 {
    const rc = issue(regs, op_read_id, 3);
    if (rc != 0) return rc;
    const w = regs.read(cdbuf(2));
    out.* = ((w & 0xFF) << 16) | (((w >> 8) & 0xFF) << 8) | ((w >> 16) & 0xFF);
    return 0;
}
