//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The calls a client has sent and not yet had answered.

/// A fixed table from request id to whoever is waiting on it.
///
/// The waiter is a number of the caller's choosing: an index, a handle, a
/// pointer as an integer. The table only keeps it and hands it back.
pub fn Pending(comptime capacity: usize) type {
    return struct {
        const Self = @This();
        const Slot = struct { id: u32, waiter: usize };

        slots: [capacity]?Slot = @splat(null),
        next_id: u32 = 1,

        /// Reserve an id for `waiter`, or `TableFull` if every slot is taken.
        pub fn add(self: *Self, waiter: usize) error{TableFull}!u32 {
            const slot = for (&self.slots) |*slot| {
                if (slot.* == null) break slot;
            } else return error.TableFull;

            // After 2^32 calls the counter comes round to ids that may still
            // be waiting. At most `capacity` are, so this ends.
            var id = self.next_id;
            for (0..capacity) |_| {
                if (self.find(id) == null) break;
                id +%= 1;
            }
            self.next_id = id +% 1;
            slot.* = .{ .id = id, .waiter = waiter };
            return id;
        }

        /// Release `id` and return its waiter, or `UnknownId`.
        pub fn take(self: *Self, id: u32) error{UnknownId}!usize {
            const slot = self.find(id) orelse return error.UnknownId;
            defer slot.* = null;
            return slot.*.?.waiter;
        }

        pub fn count(self: *const Self) usize {
            var total: usize = 0;
            for (self.slots) |slot| total += @intFromBool(slot != null);
            return total;
        }

        fn find(self: *Self, id: u32) ?*?Slot {
            for (&self.slots) |*slot| {
                if (slot.*) |held| if (held.id == id) return slot;
            }
            return null;
        }
    };
}
