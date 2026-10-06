//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XSPI suspend / resume opcodes and the RSTEN + RST software reset
//! (RA8FW-867, was part of ra8_xspi.c). Exports live in
//! src/xspi_reset_abi.zig. HUM Ch 44 p 2986; IS25LX512M Ch 8.20-8.21 p 39.

pub const op_suspend: u8 = 0x75;
pub const op_resume: u8 = 0x7A;
pub const op_reset_en: u8 = 0x66;
pub const op_reset_dev: u8 = 0x99;

pub const cmd_bytes_1s: u8 = 1;
pub const cmd_bytes_8d: u8 = 2;

/// CDBUF slot 0: CDT, CDA, CDD0, CDD1.
pub const off_cdt: usize = 0x80;
pub const off_cda: usize = 0x84;
pub const off_cdd0: usize = 0x88;
pub const off_cdd1: usize = 0x8C;

const trtype_write: u32 = 1 << 15;
const cmd_shift = 16;

pub const Error = error{InvalidArg};

/// CMD goes out MSB-first from CDT[31:16]: a 1S opcode is left-justified
/// to [31:24]; an 8D pair is the opcode and its complement.
pub fn cdtWord(opcode: u8, cmd_bytes: u8) u32 {
    const cmd: u16 = if (cmd_bytes == cmd_bytes_8d)
        @as(u16, opcode) | @as(u16, ~opcode) << 8
    else
        @as(u16, opcode) << 8;
    return (@as(u32, cmd_bytes) & 0x3) | trtype_write | @as(u32, cmd) << cmd_shift;
}

pub fn stage(regs: anytype, opcode: u8, cmd_bytes: u8) void {
    regs.write(off_cdt, cdtWord(opcode, cmd_bytes));
    regs.write(off_cda, 0);
    regs.write(off_cdd0, 0);
    regs.write(off_cdd1, 0);
}

/// RSTEN must immediately precede RST or the device ignores RST. Returns
/// the first non-zero code from `ops.kick`.
pub fn reset(ops: anytype, cmd_bytes: u8) Error!u16 {
    if (cmd_bytes != cmd_bytes_1s and cmd_bytes != cmd_bytes_8d) return error.InvalidArg;
    for ([_]u8{ op_reset_en, op_reset_dev }) |op| {
        stage(ops, op, cmd_bytes);
        const rc = ops.kick();
        if (rc != 0) return rc;
    }
    return 0;
}
