//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure core of the `ra8_app` framework: registry table arithmetic, the
//! derived lifecycle state, and the back-stack push rule. Nothing here knows
//! about the C ABI, logging, or vtable dispatch, so every decision the C made
//! can be driven directly from Zig tests.

const std = @import("std");

/// `k_ra8_app_none`: no active app / not found.
pub const none_index: i16 = -1;

/// Lifecycle state of one app (`ra8_app_state_t`).
pub const AppState = enum(u8) {
    /// Not registered (initial, or after uninstall).
    unmounted = 0,
    /// Mounted (init ran) but not focused.
    background = 1,
    /// Mounted and focused.
    foreground = 2,
};

/// Derive an app's lifecycle state from its registry index and the focus.
///
/// The state is never stored: membership plus the focused index is the single
/// source of truth, so this can never disagree with a find or an active read.
pub fn stateFor(index: i16, active: i16) AppState {
    if (index == none_index) return .unmounted;
    return if (index == active) .foreground else .background;
}

/// Registry index bound check (`idx >= count` is out of range).
pub fn inRange(index: u16, count: u16) bool {
    return index < count;
}

/// Whether a navigation move records a trail entry, and which id it records.
pub const PushPlan = struct {
    /// True when the outgoing app must be pushed onto the back-stack.
    push: bool = false,
    /// Id of the outgoing app; meaningful only when `push` is true.
    prev_id: u16 = 0,
};

/// Decide whether a navigation to `target_id` pushes the current app.
///
/// Only a switch away from a *different* app records a trail entry: the very
/// first focus (no current app) and an idempotent re-tap push nothing.
pub fn pushPlan(current_id: ?u16, target_id: u16) PushPlan {
    const current = current_id orelse return .{};
    if (current == target_id) return .{};
    return .{ .push = true, .prev_id = current };
}

/// Whether the back-stack has no room for another entry.
pub fn stackFull(depth: u16, cap: u16) bool {
    return depth >= cap;
}

/// Fix the focused index up after the slot at `removed` left the table.
///
/// A focus sitting after the hole shifts down one place so it keeps pointing
/// at the same app; a focus before it, or no focus at all, is untouched.
pub fn adjustActive(active: i16, removed: u16) i16 {
    const removed_index: i16 = @bitCast(removed);
    return if (active > removed_index) active - 1 else active;
}

/// Table operations over a registry of `?*Slot`, where `Slot` carries a `u16`
/// `id` field. Generic so the core can be tested without the C ABI structs.
pub fn Table(comptime Slot: type) type {
    return struct {
        /// Index of the first live slot whose id matches, else `none_index`.
        ///
        /// A NULL slot is legitimate caller storage and is skipped rather than
        /// dereferenced. The index narrows exactly as the C's `(int16_t)i` did.
        pub fn find(slots: []const ?*Slot, id: u16) i16 {
            for (slots, 0..) |maybe_slot, position| {
                const slot = maybe_slot orelse continue;
                if (slot.id == id) return @bitCast(@as(u16, @intCast(position)));
            }
            return none_index;
        }

        /// Shift every slot after `index` down one place (forward sweep,
        /// bounded by the live slice, NASA Rule 2). The caller decrements the
        /// count and fixes the focus up.
        pub fn compactAt(slots: []?*Slot, index: u16) void {
            var position: usize = index;
            while (position + 1 < slots.len) : (position += 1) {
                slots[position] = slots[position + 1];
            }
        }
    };
}
