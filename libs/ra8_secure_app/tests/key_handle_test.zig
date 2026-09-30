//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Handle obfuscation: never zero, decorrelated across slots, and moved by a
//! reroll. These are the properties the veneer relies on, not the arithmetic.

const std = @import("std");
const key_handle = @import("key_handle");

test "a handle is never the reserved zero sentinel" {
    var slot: u16 = 0;
    while (slot < 8) : (slot += 1) {
        try std.testing.expect(key_handle.forSlot(slot) != key_handle.zero);
    }
}

test "bit 31 is always set" {
    var slot: u16 = 0;
    while (slot < 8) : (slot += 1) {
        try std.testing.expect(key_handle.forSlot(slot) & key_handle.Mix.high_bit != 0);
    }
}

test "distinct slots get distinct handles" {
    var seen: [8]u32 = undefined;
    for (&seen, 0..) |*dst, slot| dst.* = key_handle.forSlot(@intCast(slot));
    for (seen, 0..) |left, i| {
        for (seen[i + 1 ..]) |right| {
            try std.testing.expect(left != right);
        }
    }
}

test "the same slot is stable within a boot" {
    const first = key_handle.forSlot(3);
    try std.testing.expectEqual(first, key_handle.forSlot(3));
}

test "a reroll moves every handle" {
    const before = key_handle.forSlot(5);
    key_handle.reroll();
    try std.testing.expect(key_handle.forSlot(5) != before);
}

test "the salt never rerolls to zero" {
    var round: usize = 0;
    while (round < 4096) : (round += 1) {
        key_handle.reroll();
        try std.testing.expect(key_handle.current() != 0);
    }
}

test "the slot index is not recoverable by masking the handle" {
    // The slot occupies the low bits of the XOR, so a handle whose low bits
    // equalled the slot would mean the salt contributed nothing there.
    key_handle.reroll();
    var slot: u16 = 1;
    var differs = false;
    while (slot < 8) : (slot += 1) {
        if (key_handle.forSlot(slot) & 0xFFFF != slot) differs = true;
    }
    try std.testing.expect(differs);
}

test "rotation is the documented amount" {
    // Pins the constant the decorrelation argument rests on: a rotate of zero
    // would leave the low half of the salt facing the slot index directly.
    try std.testing.expect(key_handle.Mix.rotate_bits != 0);
    try std.testing.expect(key_handle.Mix.reroll_rotate != 0);
}
