//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XSPI flash page program and sector erase (RA8FW-871, was the rest of
//! ra8_xspi_flash.c). Exports live in src/xspi_program_abi.zig. The
//! payload goes into CDD0/CDD1 BEFORE TRREQ, as FSP
//! r_ospi_b_direct_transfer does; kicking first clocks stale CDBUF words
//! onto the bus. HUM Ch 44 p 2986.

const cmd = @import("xspi_cmd.zig");
const rd = @import("xspi_read.zig");

pub const op_write_enable: u8 = 0x06;
pub const op_page_program: u8 = 0x02;
pub const op_erase_sector: u8 = 0x20;
/// `k_ra8_xspi_page_len`: one PP must not cross a NOR page.
pub const page_len: u32 = 256;
/// `k_ra8_flash_program_timeout_us`: RDSR polls before giving up.
pub const wip_polls: u32 = 32000;
pub const status_wip: u8 = 1 << 0;
pub const timeout: u16 = 0x108;

/// RDSR until WIP clears; a read fault wins over the timeout.
pub fn pollWipClear(regs: anytype) u16 {
    var i: u32 = 0;
    while (i < wip_polls) : (i += 1) {
        var status: u8 = 0;
        const rc = cmd.readStatus(regs, &status);
        if (rc != 0) return rc;
        if (status & status_wip == 0) return 0;
    }
    return timeout;
}

/// Pack bytes 0..3 into CDD0 and 4..7 into CDD1, little-endian.
pub fn stagePayload(regs: anytype, data: []const u8) void {
    var w: [2]u32 = .{ 0, 0 };
    for (data, 0..) |b, i| w[i / 4] |= @as(u32, b) << @intCast((i % 4) * 8);
    regs.write(cmd.cdbuf(2), w[0]);
    regs.write(cmd.cdbuf(3), w[1]);
}

/// WREN, PP header and payload, then TRREQ and the WIP poll.
pub fn programChunk(regs: anytype, addr: u32, data: []const u8) u16 {
    const wren = cmd.issue(regs, op_write_enable, 0);
    if (wren != 0) return wren;
    rd.chunkHeader(regs, op_page_program, addr, @intCast(data.len), 1);
    stagePayload(regs, data);
    const rc = cmd.kick(regs);
    if (rc != 0) return rc;
    return pollWipClear(regs);
}

/// Range-check, then program in chunks clamped to 8 bytes and to the page.
pub fn program(regs: anytype, addr: u32, data: []const u8) u16 {
    const rng = rd.rangeCheck(addr, @intCast(data.len));
    if (rng != 0) return rng;
    var off: usize = 0;
    while (off < data.len) {
        const a = addr + @as(u32, @intCast(off));
        const page_left = page_len - (a & (page_len - 1));
        const n = @min(data.len - off, rd.max_chunk, page_left);
        const rc = programChunk(regs, a, data[off .. off + n]);
        if (rc != 0) return rc;
        off += n;
    }
    return 0;
}

/// WREN, 0x20 with no payload, TRREQ, then the WIP poll.
pub fn eraseSector(regs: anytype, addr: u32) u16 {
    const rng = rd.rangeCheck(addr, 0);
    if (rng != 0) return rng;
    const wren = cmd.issue(regs, op_write_enable, 0);
    if (wren != 0) return wren;
    rd.chunkHeader(regs, op_erase_sector, addr, 0, 1);
    const rc = cmd.kick(regs);
    if (rc != 0) return rc;
    return pollWipClear(regs);
}
