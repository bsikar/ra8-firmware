//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Which cell `ra8_keycache` evicts, and what a re-reference does to the
//! recency order. Two policies over the same lists (#345): plain LRU keeps one
//! list, SLRU/2Q keeps a probationary scan absorber beside a protected hot set.
//!
//! The policy decides *which* cell goes, never *how many* are resident: both
//! settings hold residency <= `cell_count`, and both skip pinned cells.
//!
//! No `extern` and no engine calls: this file is a host test root on its own.

const std = @import("std");

const list = @import("keycache_list.zig");

pub const Cell = list.Cell;
pub const Segment = list.Segment;

/// `ra8_keycache_evict_t`, an `enum : uint8_t`. Zero is LRU, so a zero-filled
/// config selects it.
pub const Evict = enum(u8) {
    lru = 0,
    slru = 1,
};

/// Which SLRU segment a cell is currently in, stored in `Cell.seg`. Under LRU
/// every cell stays `probation` and the tag is never read.
pub const Seg = enum(u8) {
    probation = 0,
    protected = 1,
};

/// The SLRU split, as `pub const` rather than the C-style `k_` enum the C used.
pub const split = struct {
    /// Protected share when the caller leaves `protected_pct` at zero.
    pub const default_pct: u32 = 75;
    /// Percent denominator, and the largest split a caller may ask for.
    pub const full_pct: u32 = 100;
};

/// Capacity of the protected segment for a given cell count and requested
/// share. A zero share selects `split.default_pct`.
pub fn protectedCap(cell_count: u32, protected_pct: u8) u32 {
    const pct: u32 = if (protected_pct == 0) split.default_pct else protected_pct;
    return (cell_count * pct) / split.full_pct;
}

/// The recency apparatus of one cache: both segments and the protected
/// accounting. Laid out exactly as the C state spells it
/// (`pb_head`, `pb_tail`, `pt_head`, `pt_tail`, `protected_count`,
/// `protected_cap`), so grouping it costs nothing on the wire.
pub const Sets = extern struct {
    /// Probationary segment. Under LRU this is the single recency list.
    pb: Segment = .{},
    /// Protected segment. Empty under LRU.
    pt: Segment = .{},
    /// Cells currently in the protected segment.
    protected_count: u32 = 0,
    /// Protected capacity; 0 under LRU.
    protected_cap: u32 = 0,

    /// The segment cell `f` currently belongs to.
    fn segmentOf(self: *Sets, meta: []const Cell, f: u32) *Segment {
        return if (meta[f].seg == @intFromEnum(Seg.protected)) &self.pt else &self.pb;
    }

    /// Re-reference cell `f` under `policy`.
    ///
    /// LRU moves it to the MRU of the single list. SLRU promotes: a protected
    /// cell moves to the protected MRU; a probationary cell moves into the
    /// protected segment, demoting the protected LRU back to probation first
    /// when the segment is full.
    pub fn access(self: *Sets, meta: []Cell, policy: Evict, f: u32) void {
        if (policy != .slru) {
            self.pb.unlink(meta, f);
            self.pb.pushHead(meta, f);
            return;
        }
        if (meta[f].seg == @intFromEnum(Seg.protected)) {
            self.pt.unlink(meta, f);
            self.pt.pushHead(meta, f);
            return;
        }
        self.pb.unlink(meta, f);
        if (self.protected_count >= self.protected_cap) self.demoteLru(meta);
        meta[f].seg = @intFromEnum(Seg.protected);
        self.pt.pushHead(meta, f);
        self.protected_count += 1;
    }

    /// Move the protected LRU back to the probationary MRU, making room for a
    /// promotion. A no-op when the protected segment is empty.
    fn demoteLru(self: *Sets, meta: []Cell) void {
        const d = self.pt.tail;
        if (d == list.none) return;
        const idx: u32 = @intCast(d);
        self.pt.unlink(meta, idx);
        meta[idx].seg = @intFromEnum(Seg.probation);
        self.pb.pushHead(meta, idx);
        self.protected_count -= 1;
    }

    /// The cell to evict: the probationary LRU that nobody holds, else the
    /// protected LRU that nobody holds, else null because every cell is pinned.
    ///
    /// Read-only, and O(1) in the common case: the scan only walks past pinned
    /// cells.
    pub fn pickVictim(self: Sets, meta: []const Cell) ?u32 {
        if (self.pb.firstUnpinned(meta)) |v| return v;
        return self.pt.firstUnpinned(meta);
    }

    /// Take cell `f` out of whichever segment holds it, keeping the protected
    /// accounting straight. The caller re-links it once it has been refilled.
    pub fn detach(self: *Sets, meta: []Cell, f: u32) void {
        const protected = meta[f].seg == @intFromEnum(Seg.protected);
        self.segmentOf(meta, f).unlink(meta, f);
        if (protected) self.protected_count -= 1;
    }

    /// Put every cell in the probationary list, cold and unpinned, in index
    /// order, so the victim scan from the tail hands out cell 0 first.
    pub fn seed(self: *Sets, meta: []Cell) void {
        self.pb = .{};
        self.pt = .{};
        self.protected_count = 0;
        for (meta, 0..) |*cell, i| {
            cell.valid = 0;
            cell.pin_count = 0;
            cell.seg = @intFromEnum(Seg.probation);
            cell.hash_next = list.none;
            self.pb.pushHead(meta, @intCast(i));
        }
    }
};

comptime {
    // These six words replace six loose fields on `ra8_keycache_t`, in the
    // order the header declares them.
    std.debug.assert(@offsetOf(Sets, "pb") == 0);
    std.debug.assert(@offsetOf(Sets, "pt") == 8);
    std.debug.assert(@offsetOf(Sets, "protected_count") == 16);
    std.debug.assert(@offsetOf(Sets, "protected_cap") == 20);
    std.debug.assert(@sizeOf(Sets) == 24);
}
