//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The static session pool. `ra8_tls_session_t` is a pointer to one slot, so
//! slot identity and the "is this pointer really one of mine" test both live
//! here and nowhere else. Generic over the slot payload so the backend can
//! decide what a live session carries without this file knowing.

const std = @import("std");

pub fn Pool(comptime Slot: type, comptime capacity: usize) type {
    return struct {
        const Self = @This();

        pub const slot_count = capacity;

        slots: [capacity]Slot = std.mem.zeroes([capacity]Slot),
        used: [capacity]bool = @splat(false),

        /// Index of `slot` in this pool, or null when the pointer is forged,
        /// stale, or simply not ours. Rejects an interior pointer too.
        pub fn indexOf(self: *const Self, slot: ?*const Slot) ?usize {
            const live = slot orelse return null;
            const base = @intFromPtr(&self.slots[0]);
            const at = @intFromPtr(live);
            if (at < base) return null;
            const offset = at - base;
            if (offset % @sizeOf(Slot) != 0) return null;
            const index = offset / @sizeOf(Slot);
            if (index >= capacity) return null;
            return index;
        }

        /// True when `slot` is a live, currently-allocated slot of this pool.
        pub fn owns(self: *const Self, slot: ?*const Slot) bool {
            const index = self.indexOf(slot) orelse return false;
            return self.used[index];
        }

        /// First free slot, marked allocated, or null when the pool is full.
        pub fn acquire(self: *Self) ?*Slot {
            for (&self.used, 0..) |*flag, index| {
                if (flag.*) continue;
                flag.* = true;
                return &self.slots[index];
            }
            return null;
        }

        /// Release one live slot and wipe it. No-op for a pointer we do not own.
        pub fn release(self: *Self, slot: ?*Slot) void {
            const index = self.indexOf(slot) orelse return;
            self.used[index] = false;
            self.slots[index] = std.mem.zeroes(Slot);
        }

        /// Return every slot to "never issued", running `on_release` over the
        /// live ones first so a backend can tear its own state down.
        pub fn reset(
            self: *Self,
            context: anytype,
            comptime on_release: fn (@TypeOf(context), *Slot) void,
        ) void {
            for (&self.used, 0..) |*flag, index| {
                if (flag.*) on_release(context, &self.slots[index]);
                flag.* = false;
                self.slots[index] = std.mem.zeroes(Slot);
            }
        }
    };
}
