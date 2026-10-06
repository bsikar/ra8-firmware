//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C dynamic address assignment and CCC send/receive (RA8FW-822, was part
//! of ra8_i3c.c). Pure: the I3C block comes in as a `regs` value
//! (read32/write32 by offset); the exports live in src/i3c_ccc_abi.zig.

pub const off_ncmdqp: usize = 0x150;
pub const off_ntdtbp0: usize = 0x158;
pub const off_ntst: usize = 0x1E0;
pub const ntst_cmdqef: u32 = 0x8;

pub const ccc_rstdaa: u8 = 0x06;
pub const ccc_entdaa: u8 = 0x07;
pub const ccc_setdasa: u8 = 0x87;
pub const ccc_direct: u8 = 0x80;
pub const addr_mask: u8 = 0x7F;
pub const max_targets: u8 = 8;
pub const immediate_max: usize = 4;

const attr_immed: u32 = 1;
const attr_addr_assgn: u32 = 2;
const roc_toc: u32 = (1 << 30) | (1 << 31);

/// `ra8_i3c_daa_target_t`.
pub const Target = extern struct { pid: [6]u8, bcr: u8, dcr: u8, dynamic_address: u8 };

/// CCC command descriptor (FSP I3C_CMD_DESC_*): CP, opcode [14:7], device
/// index [22:16], RnW, response and STOP on completion.
pub fn cccWord(ccc: u8, addr: u8, rnw: bool) u32 {
    return (1 << 15) | (@as(u32, ccc) << 7) | (@as(u32, addr) << 16) | (@as(u32, @intFromBool(rnw)) << 29) | roc_toc;
}

pub fn entdaaWord(count: u8) u32 {
    return attr_addr_assgn | (@as(u32, ccc_entdaa) << 7) | (@as(u32, count) << 26) | roc_toc;
}

fn immedWord(cmd: u32, len: usize) u32 {
    return cmd | attr_immed | (@as(u32, @intCast(len)) << 23);
}

/// Bytes packed LSB-first into one word (the immediate payload and the
/// FIFO tail).
pub fn pack(bytes: []const u8) u32 {
    var w: u32 = 0;
    for (bytes, 0..) |b, i| w |= @as(u32, b) << @intCast(i * 8);
    return w;
}

pub fn fifoWrite(regs: anytype, data: []const u8) void {
    var i: usize = 0;
    while (i < data.len) : (i += 4) regs.write32(off_ntdtbp0, pack(data[i..@min(i + 4, data.len)]));
}

pub fn fifoRead(regs: anytype, out: []u8) void {
    var i: usize = 0;
    while (i < out.len) : (i += 4) {
        const w = regs.read32(off_ntdtbp0);
        for (out[i..@min(i + 4, out.len)], 0..) |*b, k| b.* = @truncate(w >> @intCast(k * 8));
    }
}

fn push(regs: anytype, w0: u32, w1: u32) void {
    regs.write32(off_ncmdqp, w0);
    regs.write32(off_ncmdqp, w1);
}

fn clearCmdqEmpty(regs: anytype) void {
    regs.write32(off_ntst, regs.read32(off_ntst) & ~ntst_cmdqef);
}

/// ENTDAA: each accepted target answers PID(6) + BCR + DCR, drained in
/// order; dynamic_address stays as the caller set it.
pub fn daa(regs: anytype, targets: []Target) void {
    push(regs, entdaaWord(@intCast(targets.len)), 0);
    for (targets) |*t| {
        var raw: [8]u8 = undefined;
        fifoRead(regs, &raw);
        t.pid = raw[0..6].*;
        t.bcr = raw[6];
        t.dcr = raw[7];
    }
    clearCmdqEmpty(regs);
}

/// SETDASA: one immediate byte, the new dynamic address.
pub fn setdasa(regs: anytype, static_addr: u8, dynamic_addr: u8) void {
    push(regs, immedWord(cccWord(ccc_setdasa, static_addr, false), 1), dynamic_addr);
    clearCmdqEmpty(regs);
}

/// RSTDAA: payload-less broadcast.
pub fn rstdaa(regs: anytype) void {
    push(regs, cccWord(ccc_rstdaa, 0, false), 0);
    clearCmdqEmpty(regs);
}

/// Up to four bytes ride in the descriptor; longer payloads declare the
/// length in [31:16] and go through NTDTBP0.
pub fn send(regs: anytype, ccc: u8, addr: u8, payload: []const u8) void {
    const cmd = cccWord(ccc, addr, false);
    if (payload.len <= immediate_max) {
        push(regs, immedWord(cmd, payload.len), pack(payload));
    } else {
        push(regs, cmd, @as(u32, @intCast(payload.len)) << 16);
        fifoWrite(regs, payload);
    }
    clearCmdqEmpty(regs);
}

pub fn recv(regs: anytype, ccc: u8, addr: u8, buf: []u8) void {
    push(regs, cccWord(ccc, addr, true), @as(u32, @intCast(buf.len)) << 16);
    fifoRead(regs, buf);
    clearCmdqEmpty(regs);
}
