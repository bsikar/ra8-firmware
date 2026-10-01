//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The fixed-cell slab allocator (layer 0 of the memory hierarchy). A
//! caller-owned buffer is divided into equal cells handed out and taken back
//! in O(1). Equal cells mean no external fragmentation, and a caller-supplied
//! buffer means nothing is allocated after init.
//!
//! The freelist is threaded through the free cells themselves: the first four
//! bytes of a free cell hold the index of the next one, so the slab needs no
//! side table. That is why a cell is at least four bytes and a multiple of
//! four, and why the buffer must be four-byte aligned.

const std = @import("std");

const vocab = @import("vocab.zig");

pub const Err = vocab.Err;
pub const Limits = vocab.SlabLimits;

/// `ra8_slab_t`, field for field.
pub const Slab = extern struct {
    base: ?[*]u8 = null,
    cell_bytes: u32 = 0,
    cell_count: u32 = 0,
    free_head: u32 = 0,
    free_count: u32 = 0,

    fn cell(self: *const Slab, idx: u32) [*]u8 {
        const off = @as(usize, idx) * @as(usize, self.cell_bytes);
        return self.base.? + off;
    }

    /// Read the freelist link out of a free cell.
    ///
    /// The C copies the four bytes rather than casting, because the cell is
    /// only guaranteed four-byte aligned and the payload type is unknown. The
    /// port copies too, so the in-memory representation stays identical and a
    /// slab threaded by one implementation reads correctly in the other.
    fn next(self: *const Slab, idx: u32) u32 {
        var out: u32 = undefined;
        @memcpy(std.mem.asBytes(&out), self.cell(idx)[0..@sizeOf(u32)]);
        return out;
    }

    fn setNext(self: *Slab, idx: u32, link: u32) void {
        @memcpy(self.cell(idx)[0..@sizeOf(u32)], std.mem.asBytes(&link));
    }
};

pub fn init(slab: *Slab, buffer: [*]u8, buffer_bytes: u32, cell_bytes: u32) Err {
    if (cell_bytes < Limits.min_cell_bytes) return .invalid_size;
    if (cell_bytes % Limits.align_bytes != 0) return .invalid_size;

    const count = buffer_bytes / cell_bytes;
    if (count == 0) return .invalid_size;

    slab.base = buffer;
    slab.cell_bytes = cell_bytes;
    slab.cell_count = count;

    var i: u32 = 0;
    while (i < count) : (i += 1) {
        slab.setNext(i, if (i + 1 < count) i + 1 else Limits.nil);
    }

    slab.free_head = 0;
    slab.free_count = count;
    return .ok;
}

pub fn alloc(slab: *Slab, out_cell: *?*anyopaque) Err {
    if (slab.free_head == Limits.nil) return .no_mem;

    const idx = slab.free_head;
    slab.free_head = slab.next(idx);
    slab.free_count -= 1;
    out_cell.* = @ptrCast(slab.cell(idx));
    return .ok;
}

/// Return a cell to the freelist.
///
/// A pointer that is outside the slab or off a cell boundary is rejected. A
/// double free of an in-bounds cell is not detectable without a side table and
/// corrupts the freelist, exactly as the C documents.
pub fn free(slab: *Slab, cell_ptr: *anyopaque) Err {
    const cell_addr = @intFromPtr(cell_ptr);
    const base_addr = @intFromPtr(slab.base.?);
    if (cell_addr < base_addr) return .invalid_arg;

    const off = cell_addr - base_addr;
    const span = @as(usize, slab.cell_count) * @as(usize, slab.cell_bytes);
    if (off >= span) return .invalid_arg;
    if (off % slab.cell_bytes != 0) return .invalid_arg;

    const idx: u32 = @intCast(off / slab.cell_bytes);
    slab.setNext(idx, slab.free_head);
    slab.free_head = idx;
    slab.free_count += 1;
    return .ok;
}

/// Report the free and total cell counts. Either output may be omitted.
pub fn stats(slab: *const Slab, out_free: ?*u32, out_total: ?*u32) Err {
    if (slab.base == null) return .invalid_state;
    if (out_free) |dst| dst.* = slab.free_count;
    if (out_total) |dst| dst.* = slab.cell_count;
    return .ok;
}

comptime {
    std.debug.assert(@offsetOf(Slab, "base") == 0);
    std.debug.assert(@offsetOf(Slab, "cell_bytes") == @sizeOf(usize));
    std.debug.assert(@offsetOf(Slab, "cell_count") == @sizeOf(usize) + 4);
    std.debug.assert(@offsetOf(Slab, "free_head") == @sizeOf(usize) + 8);
    std.debug.assert(@offsetOf(Slab, "free_count") == @sizeOf(usize) + 12);
}
