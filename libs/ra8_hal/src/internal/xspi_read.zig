//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XSPI flash read path (RA8FW-870, was part of ra8_xspi_flash.c): the
//! 3-byte address bound, the per-chunk CDT/CDA header and the JEDEC 0x03
//! read. Exports live in src/xspi_read_abi.zig. HUM Ch 44 p 2986.

const cmd = @import("xspi_cmd.zig");

/// 2^24: ADDSIZE = 3 cannot carry a larger address.
pub const addr_space_3byte: u32 = 0x100_0000;
/// CDD0 + CDD1 carry 8 data bytes per manual command.
pub const max_chunk: u32 = 8;
/// `k_ra8_xspi_max_xfer`: bytes per read/program call.
pub const max_xfer: u32 = 4096;
pub const op_read: u8 = 0x03;
pub const invalid_arg: u16 = 0x103;

/// Reject a [addr, addr + len) window the 3-byte address phase can't reach.
pub fn rangeCheck(addr: u32, len: u32) u16 {
    if (addr >= addr_space_3byte) return invalid_arg;
    if (len > addr_space_3byte - addr) return invalid_arg;
    return 0;
}

/// CDT (1-byte opcode, 3-byte address, `n` data bytes) and CDA for slot 0.
pub fn chunkHeader(regs: anytype, opcode: u8, addr: u32, n: u8, is_write: u8) void {
    regs.write(cmd.cdbuf(0), cmd.makeCdt(opcode, 1, 3, n, is_write));
    regs.write(cmd.cdbuf(1), addr);
}

/// One 0x03 read of out.len (1..8) bytes; CDD0 holds bytes 0..3, CDD1 4..7.
pub fn readChunk(regs: anytype, addr: u32, out: []u8) u16 {
    chunkHeader(regs, op_read, addr, @intCast(out.len), 0);
    const rc = cmd.kick(regs);
    if (rc != 0) return rc;
    for (out, 0..) |*b, i| {
        const word = regs.read(cmd.cdbuf(if (i < 4) 2 else 3));
        b.* = @truncate(word >> @intCast((i % 4) * 8));
    }
    return 0;
}

/// Range-check, then walk `buf` in 8-byte chunks; the first error wins.
pub fn read(regs: anytype, addr: u32, buf: []u8) u16 {
    const rng = rangeCheck(addr, @intCast(buf.len));
    if (rng != 0) return rng;
    var off: usize = 0;
    while (off < buf.len) {
        const n = @min(buf.len - off, max_chunk);
        const rc = readChunk(regs, addr + @as(u32, @intCast(off)), buf[off .. off + n]);
        if (rc != 0) return rc;
        off += n;
    }
    return 0;
}
