//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The static key-handle pool. `ra8_psa_key_t` is a pointer to one `Slot`,
//! so slot identity and the "is this pointer really one of mine" test both
//! live here and nowhere else.

const std = @import("std");
const vocab = @import("vocab.zig");

const Limits = vocab.Limits;

/// One pool slot, backing exactly one `ra8_psa_key_t`.
pub const Slot = extern struct {
    in_use: bool,
    attr: vocab.KeyAttr,
    key_len: usize,
    key: [Limits.max_key_bytes]u8,
    /// Underlying PSA key identifier. Unused by the off-target backend, which
    /// keeps its key material in `key` instead.
    psa_id: u32,

    pub fn material(self: *const Slot) []const u8 {
        return self.key[0..self.key_len];
    }

    pub fn clear(self: *Slot) void {
        @memset(&self.key, 0);
        self.key_len = 0;
        self.in_use = false;
    }
};

/// Fixed-size pool of `Limits.max_keys` slots plus the one-shot init flag.
pub const Pool = struct {
    slots: [Limits.max_keys]Slot = std.mem.zeroes([Limits.max_keys]Slot),
    initialized: bool = false,

    /// First free slot, or null when the pool is full.
    pub fn alloc(self: *Pool) ?*Slot {
        for (&self.slots) |*slot| {
            if (!slot.in_use) return slot;
        }
        return null;
    }

    /// True when `handle` points at a live slot of *this* pool. Guards against
    /// a caller handing back a stack address or a stale pointer.
    pub fn owns(self: *const Pool, handle: ?*const Slot) bool {
        const slot = handle orelse return false;
        const base = @intFromPtr(&self.slots[0]);
        const end = base + (Limits.max_keys * @sizeOf(Slot));
        const at = @intFromPtr(slot);
        if (at < base or at >= end) return false;
        return slot.in_use;
    }

    /// Release every live slot, running `on_release` for each before wiping it.
    pub fn releaseAll(self: *Pool, context: anytype, comptime on_release: fn (@TypeOf(context), *Slot) void) void {
        for (&self.slots) |*slot| {
            if (!slot.in_use) continue;
            on_release(context, slot);
            slot.clear();
        }
    }

    /// Reset every slot to "never issued" without notifying a backend.
    pub fn reset(self: *Pool) void {
        for (&self.slots) |*slot| {
            slot.in_use = false;
            slot.key_len = 0;
        }
    }
};
