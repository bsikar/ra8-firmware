//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure core: the derived lifecycle state, the registry table
//! scan and compaction, the focus fix-up, and the back-stack push rule.

const std = @import("std");
const implementation = @import("implementation");

const Slot = struct { id: u16 };
const Table = implementation.Table(Slot);

test "stateFor: an unregistered index is unmounted" {
    try std.testing.expectEqual(
        implementation.AppState.unmounted,
        implementation.stateFor(implementation.none_index, 0),
    );
}

test "stateFor: the focused index is foreground" {
    try std.testing.expectEqual(implementation.AppState.foreground, implementation.stateFor(2, 2));
}

test "stateFor: a registered but unfocused index is background" {
    try std.testing.expectEqual(implementation.AppState.background, implementation.stateFor(2, 1));
}

test "stateFor: registered with no focus at all is background" {
    try std.testing.expectEqual(
        implementation.AppState.background,
        implementation.stateFor(0, implementation.none_index),
    );
}

test "stateFor: an unregistered index stays unmounted even when nothing is focused" {
    try std.testing.expectEqual(
        implementation.AppState.unmounted,
        implementation.stateFor(implementation.none_index, implementation.none_index),
    );
}

test "inRange: an index below the count is in range" {
    try std.testing.expect(implementation.inRange(0, 1));
    try std.testing.expect(implementation.inRange(2, 3));
}

test "inRange: the count itself and beyond are out of range" {
    try std.testing.expect(!implementation.inRange(3, 3));
    try std.testing.expect(!implementation.inRange(9, 3));
}

test "inRange: an empty table has no valid index" {
    try std.testing.expect(!implementation.inRange(0, 0));
}

test "pushPlan: the first focus pushes nothing" {
    const plan = implementation.pushPlan(null, 7);
    try std.testing.expect(!plan.push);
    try std.testing.expectEqual(@as(u16, 0), plan.prev_id);
}

test "pushPlan: an idempotent re-tap pushes nothing" {
    const plan = implementation.pushPlan(7, 7);
    try std.testing.expect(!plan.push);
}

test "pushPlan: a switch records the outgoing id" {
    const plan = implementation.pushPlan(10, 20);
    try std.testing.expect(plan.push);
    try std.testing.expectEqual(@as(u16, 10), plan.prev_id);
}

test "pushPlan: id zero is a real id, not an absent focus" {
    const plan = implementation.pushPlan(0, 5);
    try std.testing.expect(plan.push);
    try std.testing.expectEqual(@as(u16, 0), plan.prev_id);
}

test "stackFull: room below the capacity" {
    try std.testing.expect(!implementation.stackFull(0, 1));
    try std.testing.expect(!implementation.stackFull(3, 4));
}

test "stackFull: at and past the capacity" {
    try std.testing.expect(implementation.stackFull(4, 4));
    try std.testing.expect(implementation.stackFull(5, 4));
}

test "stackFull: a zero capacity is always full" {
    try std.testing.expect(implementation.stackFull(0, 0));
}

test "adjustActive: a focus after the hole shifts down" {
    try std.testing.expectEqual(@as(i16, 2), implementation.adjustActive(3, 1));
}

test "adjustActive: a focus before the hole is untouched" {
    try std.testing.expectEqual(@as(i16, 1), implementation.adjustActive(1, 3));
}

test "adjustActive: a focus at the removed index is untouched" {
    try std.testing.expectEqual(@as(i16, 2), implementation.adjustActive(2, 2));
}

test "adjustActive: no focus stays none" {
    try std.testing.expectEqual(
        implementation.none_index,
        implementation.adjustActive(implementation.none_index, 0),
    );
}

test "Table.find: an empty table finds nothing" {
    try std.testing.expectEqual(implementation.none_index, Table.find(&[_]?*Slot{}, 1));
}

test "Table.find: a matching id reports its index" {
    var a = Slot{ .id = 10 };
    var b = Slot{ .id = 20 };
    const slots = [_]?*Slot{ &a, &b };
    try std.testing.expectEqual(@as(i16, 1), Table.find(&slots, 20));
}

test "Table.find: an absent id reports none" {
    var a = Slot{ .id = 10 };
    const slots = [_]?*Slot{&a};
    try std.testing.expectEqual(implementation.none_index, Table.find(&slots, 99));
}

test "Table.find: a NULL slot is skipped, not dereferenced" {
    var a = Slot{ .id = 1 };
    const slots = [_]?*Slot{ null, &a };
    try std.testing.expectEqual(@as(i16, 1), Table.find(&slots, 1));
    try std.testing.expectEqual(implementation.none_index, Table.find(&slots, 7));
}

test "Table.find: every slot NULL reports none" {
    const slots = [_]?*Slot{ null, null };
    try std.testing.expectEqual(implementation.none_index, Table.find(&slots, 0));
}

test "Table.find: the first match wins" {
    var a = Slot{ .id = 5 };
    var b = Slot{ .id = 5 };
    const slots = [_]?*Slot{ &a, &b };
    try std.testing.expectEqual(@as(i16, 0), Table.find(&slots, 5));
}

test "Table.find: id zero is searchable" {
    var a = Slot{ .id = 0 };
    const slots = [_]?*Slot{&a};
    try std.testing.expectEqual(@as(i16, 0), Table.find(&slots, 0));
}

test "Table.compactAt: removing the middle shifts the tail down" {
    var a = Slot{ .id = 1 };
    var b = Slot{ .id = 2 };
    var c = Slot{ .id = 3 };
    var slots = [_]?*Slot{ &a, &b, &c };
    Table.compactAt(&slots, 1);
    try std.testing.expectEqual(@as(u16, 1), slots[0].?.id);
    try std.testing.expectEqual(@as(u16, 3), slots[1].?.id);
}

test "Table.compactAt: removing the head shifts everything down" {
    var a = Slot{ .id = 1 };
    var b = Slot{ .id = 2 };
    var c = Slot{ .id = 3 };
    var slots = [_]?*Slot{ &a, &b, &c };
    Table.compactAt(&slots, 0);
    try std.testing.expectEqual(@as(u16, 2), slots[0].?.id);
    try std.testing.expectEqual(@as(u16, 3), slots[1].?.id);
}

test "Table.compactAt: removing the tail leaves the earlier slots alone" {
    var a = Slot{ .id = 1 };
    var b = Slot{ .id = 2 };
    var slots = [_]?*Slot{ &a, &b };
    Table.compactAt(&slots, 1);
    try std.testing.expectEqual(@as(u16, 1), slots[0].?.id);
    try std.testing.expectEqual(@as(u16, 2), slots[1].?.id);
}

test "Table.compactAt: a single-slot table needs no shifting" {
    var a = Slot{ .id = 1 };
    var slots = [_]?*Slot{&a};
    Table.compactAt(&slots, 0);
    try std.testing.expectEqual(@as(u16, 1), slots[0].?.id);
}

test "Table.compactAt: a NULL slot compacts like any other" {
    var a = Slot{ .id = 1 };
    var slots = [_]?*Slot{ &a, null, &a };
    Table.compactAt(&slots, 0);
    try std.testing.expect(slots[0] == null);
    try std.testing.expectEqual(@as(u16, 1), slots[1].?.id);
}

test "compact then fix up: the focus keeps pointing at the same app" {
    var a = Slot{ .id = 1 };
    var b = Slot{ .id = 2 };
    var c = Slot{ .id = 3 };
    var slots = [_]?*Slot{ &a, &b, &c };
    var active: i16 = 2; // focused on c
    Table.compactAt(&slots, 1); // remove b
    active = implementation.adjustActive(active, 1);
    try std.testing.expectEqual(@as(i16, 1), active);
    try std.testing.expectEqual(@as(u16, 3), slots[@intCast(active)].?.id);
}
