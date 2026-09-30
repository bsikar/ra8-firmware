//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The intrusive recency list `ra8_keycache` orders its cells with, and the
//! per-cell record that carries the links. One doubly-linked list of cell
//! indices, threaded through a caller-owned metadata array: under LRU the
//! engine keeps one of these, under SLRU two (probationary and protected).
//!
//! The list knows nothing about hashing, pinning policy or eviction beyond
//! this: a victim scan walks from the tail and wants the first cell nobody
//! holds, so `firstUnpinned` lives here with the walk it belongs to.
//!
//! No `extern` and no engine: this file is a host test root on its own.

const std = @import("std");

/// A list terminator. The links are `i32` because `ra8_keycache_cell_t`
/// publishes them that way; -1 is C's "no cell".
pub const none: i32 = -1;

/// `ra8_keycache_cell_t`: one cell's link metadata, caller-allocated, one per
/// cell. `prev`/`next` are this file's; `hash_next` belongs to the bucket
/// chain and `seg`/`pin_count`/`valid` to the engine, but C publishes them in
/// one record and the layout is part of the ABI, so they ride together.
pub const Cell = extern struct {
    prev: i32 = none,
    next: i32 = none,
    hash_next: i32 = none,
    pin_count: u16 = 0,
    seg: u8 = 0,
    valid: u8 = 0,
};

/// The two ends of one recency list. C spells these as four loose `int32_t`
/// on the cache state (`pb_head`, `pb_tail`, `pt_head`, `pt_tail`), which is
/// how a caller ends up passing the probationary head beside the protected
/// tail. Grouping them into a pair costs no bytes (two `int32_t` in the same
/// order) and makes that call impossible to write.
pub const Segment = extern struct {
    /// Most recently used cell, or `none`.
    head: i32 = none,
    /// Least recently used cell, or `none`.
    tail: i32 = none,

    /// Splice `f` out of this list, fixing its neighbours and either end.
    ///
    /// Tolerates an already-detached cell, exactly as the C did: a cell with
    /// no `prev` that is not the head, or no `next` that is not the tail, is
    /// simply left alone.
    pub fn unlink(self: *Segment, meta: []Cell, f: u32) void {
        const cell = &meta[f];
        if (cell.prev != none) {
            meta[@intCast(cell.prev)].next = cell.next;
        } else if (self.head == @as(i32, @intCast(f))) {
            self.head = cell.next;
        }
        if (cell.next != none) {
            meta[@intCast(cell.next)].prev = cell.prev;
        } else if (self.tail == @as(i32, @intCast(f))) {
            self.tail = cell.prev;
        }
    }

    /// Link `f` in as the new most-recently-used cell. `f` must be detached.
    pub fn pushHead(self: *Segment, meta: []Cell, f: u32) void {
        const idx: i32 = @intCast(f);
        meta[f].prev = none;
        meta[f].next = self.head;
        if (self.head != none) meta[@intCast(self.head)].prev = idx;
        self.head = idx;
        if (self.tail == none) self.tail = idx;
    }

    /// The least-recently-used cell nobody holds a pin on, walking from the
    /// tail toward the head, or null when every cell in the list is pinned.
    ///
    /// The walk is bounded by the cell count (NASA P10 Rule 2), so a corrupt
    /// link ring terminates instead of spinning.
    pub fn firstUnpinned(self: Segment, meta: []const Cell) ?u32 {
        var cur = self.tail;
        var guard: usize = 0;
        while (cur != none and guard < meta.len) : (guard += 1) {
            const idx: u32 = @intCast(cur);
            if (meta[idx].pin_count == 0) return idx;
            cur = meta[idx].prev;
        }
        return null;
    }
};

comptime {
    // The ABI the C header fixes: three `int32_t`, a `uint16_t` and two
    // `uint8_t`, in that order, with no tail padding.
    std.debug.assert(@offsetOf(Cell, "prev") == 0);
    std.debug.assert(@offsetOf(Cell, "next") == 4);
    std.debug.assert(@offsetOf(Cell, "hash_next") == 8);
    std.debug.assert(@offsetOf(Cell, "pin_count") == 12);
    std.debug.assert(@offsetOf(Cell, "seg") == 14);
    std.debug.assert(@offsetOf(Cell, "valid") == 15);
    std.debug.assert(@sizeOf(Cell) == 16);

    // A segment is exactly the two `int32_t` it replaces on the state.
    std.debug.assert(@offsetOf(Segment, "head") == 0);
    std.debug.assert(@offsetOf(Segment, "tail") == 4);
    std.debug.assert(@sizeOf(Segment) == 8);
}
