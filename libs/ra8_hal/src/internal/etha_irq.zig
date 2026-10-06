//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ETHA error-IRQ status, enable and dispatch (RA8FW-815, was part of
//! ra8_etha.c). Pure: one port's ETHA block comes in as a `regs` value
//! (read32/write32 by offset). Port, block and pointer checks and the
//! callback slots stay in src/etha_irq_abi.zig.

pub const block_count = 3;

pub const off_eams: usize = 0x004;
pub const off_eatasctm: usize = 0x3B4;
pub const off_eaeis0: usize = 0x500; // block n at +0x10*n
pub const mask_ops: u32 = 0x3;

/// `ra8_etha_status_t`.
pub const Status = extern struct { ops: u8, eaeis0: u32, eaeis1: u32, eaeis2: u32, tas_cycle: u32 };

pub fn blockOk(block: u8) bool {
    return block < block_count;
}

/// EAEISn, EAEIEn, EAEIDn for block n (HUM 32.3.7.1.1-9 p 1659-1668).
pub fn eis(block: u8) usize {
    return off_eaeis0 + 0x10 * @as(usize, block);
}
pub fn eie(block: u8) usize {
    return eis(block) + 4;
}
pub fn eid(block: u8) usize {
    return eis(block) + 8;
}

/// Mode (EAMS.OPS), the three error status words and the TAS cycle monitor.
pub fn status(regs: anytype) Status {
    return .{
        .ops = @intCast(regs.read32(off_eams) & mask_ops),
        .eaeis0 = regs.read32(eis(0)),
        .eaeis1 = regs.read32(eis(1)),
        .eaeis2 = regs.read32(eis(2)),
        .tas_cycle = regs.read32(off_eatasctm),
    };
}

/// Disables the masked sources, then clears their status bits.
pub fn clear(regs: anytype, block: u8, mask: u32) void {
    regs.write32(eid(block), mask);
    regs.write32(eis(block), regs.read32(eis(block)) & ~mask);
}

pub fn enable(regs: anytype, block: u8, mask: u32) void {
    regs.write32(eie(block), regs.read32(eie(block)) | mask);
}

pub fn disable(regs: anytype, block: u8, mask: u32) void {
    regs.write32(eie(block), regs.read32(eie(block)) & ~mask);
}

/// Snapshots EAEIS0..2, disables what fired, zeroes the status words and
/// returns the snapshot for the callback.
pub fn dispatch(regs: anytype) [block_count]u32 {
    var snap: [block_count]u32 = undefined;
    for (&snap, 0..) |*s, b| s.* = regs.read32(eis(@intCast(b)));
    for (snap, 0..) |s, b| regs.write32(eid(@intCast(b)), s);
    for (0..block_count) |b| regs.write32(eis(@intCast(b)), 0);
    return snap;
}
