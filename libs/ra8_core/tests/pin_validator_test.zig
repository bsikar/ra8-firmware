//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the pin-claim registry. The C ABI membrane over it is
//! covered by the untouched host suite in
//! `tests/security/src/test_ra8_pin_validator.c`.

const std = @import("std");
const registry = @import("pin_validator_registry");

const led1: u16 = 0x0600;
const led2: u16 = 0x0303;
const led3: u16 = 0x0A07;

test "slotOf packs port and pin the way the C header documents" {
    try std.testing.expectEqual(@as(u16, 6 * 16 + 0), try registry.slotOf(led1));
    try std.testing.expectEqual(@as(u16, 3 * 16 + 3), try registry.slotOf(led2));
    try std.testing.expectEqual(@as(u16, 10 * 16 + 7), try registry.slotOf(led3));
    try std.testing.expectEqual(@as(u16, 0), try registry.slotOf(0x0000));
}

test "slotOf rejects a port past the package" {
    try std.testing.expectError(registry.IndexError.InvalidPort, registry.slotOf(0x0F00));
    try std.testing.expectError(registry.IndexError.InvalidPort, registry.slotOf(0xFFFE));
}

test "slotOf rejects a pin past the port" {
    try std.testing.expectError(registry.IndexError.InvalidPin, registry.slotOf(0x0010));
    try std.testing.expectError(registry.IndexError.InvalidPin, registry.slotOf(0x00FE));
}

test "the last legal slot is the last entry of the table" {
    const last = (@as(u16, registry.limits.port_count - 1) << registry.limits.port_shift) |
        @as(u16, registry.limits.pin_count - 1);
    try std.testing.expectEqual(registry.limits.slot_count - 1, try registry.slotOf(last));
}

test "a fresh registry claims nothing" {
    var state: registry.Registry = .{};
    for (0..registry.limits.slot_count) |slot| {
        try std.testing.expect(!state.isClaimed(@intCast(slot)));
    }
}

test "claim then query" {
    var state: registry.Registry = .{};
    const owner: []const u8 = "TEST";
    try state.claim(try registry.slotOf(led1), owner.ptr);
    try std.testing.expect(state.isClaimed(try registry.slotOf(led1)));
    try std.testing.expect(!state.isClaimed(try registry.slotOf(led2)));
}

test "a second claim on the same slot is refused and leaves the first owner" {
    var state: registry.Registry = .{};
    const first: []const u8 = "FIRST";
    const second: []const u8 = "SECOND";
    const slot = try registry.slotOf(led1);

    try state.claim(slot, first.ptr);
    try std.testing.expectError(registry.Registry.ClaimError.AlreadyClaimed, state.claim(slot, second.ptr));
    try std.testing.expectEqual(@as(?*const anyopaque, first.ptr), state.owners[slot]);
}

test "release allows reclaim and clears the owner" {
    var state: registry.Registry = .{};
    const owner: []const u8 = "TEST";
    const slot = try registry.slotOf(led1);

    try state.claim(slot, owner.ptr);
    state.release(slot);
    try std.testing.expect(!state.isClaimed(slot));
    try std.testing.expectEqual(@as(?*const anyopaque, null), state.owners[slot]);
    try state.claim(slot, owner.ptr);
}

test "releasing a slot nobody holds is not an error, as in the C" {
    var state: registry.Registry = .{};
    const slot = try registry.slotOf(led2);
    state.release(slot);
    try std.testing.expect(!state.isClaimed(slot));
}

test "neighbouring slots are independent bits" {
    var state: registry.Registry = .{};
    const owner: []const u8 = "TEST";
    try state.claim(try registry.slotOf(0x0300), owner.ptr);
    try state.claim(try registry.slotOf(0x0302), owner.ptr);

    try std.testing.expect(state.isClaimed(try registry.slotOf(0x0300)));
    try std.testing.expect(!state.isClaimed(try registry.slotOf(0x0301)));
    try std.testing.expect(state.isClaimed(try registry.slotOf(0x0302)));
}

test "reset clears every claim and every owner" {
    var state: registry.Registry = .{};
    const owner: []const u8 = "TEST";
    for (0..registry.limits.slot_count) |slot| {
        try state.claim(@intCast(slot), owner.ptr);
    }
    state.reset();
    for (0..registry.limits.slot_count) |slot| {
        try std.testing.expect(!state.isClaimed(@intCast(slot)));
        try std.testing.expectEqual(@as(?*const anyopaque, null), state.owners[slot]);
    }
}
