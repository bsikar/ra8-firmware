//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C private write and read in native mode (RA8FW-823, was part of
//! ra8_i3c.c). Pure over a `regs` value; the exports live in
//! src/i3c_xfer_abi.zig.

const ccc = @import("i3c_ccc.zig");

pub const length_max: u32 = 0xFFFF;

/// Private-transfer descriptor: device index [22:16], RnW, response and
/// STOP on completion.
pub fn xferWord(addr: u8, rnw: bool) u32 {
    return (@as(u32, addr) << 16) | (@as(u32, @intFromBool(rnw)) << 29) | (1 << 30) | (1 << 31);
}

/// Up to four bytes ride in the descriptor; longer writes declare the length
/// in [31:16] and go through NTDTBP0.
pub fn write(regs: anytype, addr: u8, data: []const u8) void {
    const cmd = xferWord(addr, false);
    if (data.len <= ccc.immediate_max) {
        ccc.push(regs, cmd | 1 | (@as(u32, @intCast(data.len)) << 23), ccc.pack(data));
    } else {
        ccc.push(regs, cmd, @as(u32, @intCast(data.len)) << 16);
        ccc.fifoWrite(regs, data);
    }
    ccc.clearCmdqEmpty(regs);
}

pub fn read(regs: anytype, addr: u8, buf: []u8) void {
    ccc.push(regs, xferWord(addr, true), @as(u32, @intCast(buf.len)) << 16);
    ccc.fifoRead(regs, buf);
    ccc.clearCmdqEmpty(regs);
}
