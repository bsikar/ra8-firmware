//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The sealed-blob format and the slot allocator behind it. Every refusal path
//! is covered, because import is the only way a Non-Secure caller gets a key
//! into the vault at all.

const std = @import("std");
const key_import = @import("key_import");

const vault = key_import.vault;
const Blob = key_import.Blob;

const kak = [_]u8{0x40} ** 16;

fn pattern(seed: u8) [Blob.key_bytes]u8 {
    var key: [Blob.key_bytes]u8 = undefined;
    for (&key, 0..) |*dst, i| dst.* = seed ^ @as(u8, @intCast(i));
    return key;
}

fn provision() !void {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&kak));
    try std.testing.expectEqual(key_import.Err.ok, key_import.reset());
}

test "layout: 32 key bytes then a 16-byte tag" {
    try std.testing.expectEqual(@as(u16, 48), Blob.bytes);
    try std.testing.expectEqual(@as(u16, 32), Blob.key_bytes);
    try std.testing.expectEqual(@as(u16, 16), Blob.mac_bytes);
    try std.testing.expectEqual(Blob.bytes, @sizeOf(key_import.Sealed));
}

test "a built blob carries the key verbatim" {
    try provision();
    const key = pattern(0x11);
    var blob: key_import.Sealed = undefined;
    try std.testing.expectEqual(key_import.Err.ok, key_import.buildBlob(&key, &blob));
    try std.testing.expectEqualSlices(u8, &key, blob[0..Blob.key_bytes]);
}

test "a built blob authenticates under the same KAK" {
    try provision();
    const key = pattern(0x22);
    var blob: key_import.Sealed = undefined;
    try std.testing.expectEqual(key_import.Err.ok, key_import.buildBlob(&key, &blob));
    try std.testing.expectEqual(key_import.Err.ok, key_import.authenticate(&blob));
}

test "seal issues a resolvable handle" {
    try provision();
    const key = pattern(0x33);
    var blob: key_import.Sealed = undefined;
    try std.testing.expectEqual(key_import.Err.ok, key_import.buildBlob(&key, &blob));

    var handle: u32 = 0;
    try std.testing.expectEqual(key_import.Err.ok, key_import.seal(&blob, &handle));
    try std.testing.expect(handle != key_import.handle.zero);

    var slot: u16 = 0xFFFF;
    try std.testing.expectEqual(key_import.Err.ok, key_import.resolve(handle, &slot));
    try std.testing.expect(slot < vault.Limits.slots);
}

test "the first seal of a fresh allocator takes slot zero" {
    try provision();
    const key = pattern(0x44);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);
    var handle: u32 = 0;
    _ = key_import.seal(&blob, &handle);

    var slot: u16 = 0xFFFF;
    try std.testing.expectEqual(key_import.Err.ok, key_import.resolve(handle, &slot));
    try std.testing.expectEqual(@as(u16, 0), slot);
}

test "successive seals take successive slots" {
    try provision();
    const key = pattern(0x55);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);

    var slots: [3]u16 = undefined;
    for (&slots) |*dst| {
        var handle: u32 = 0;
        try std.testing.expectEqual(key_import.Err.ok, key_import.seal(&blob, &handle));
        try std.testing.expectEqual(key_import.Err.ok, key_import.resolve(handle, dst));
    }
    try std.testing.expectEqualSlices(u16, &.{ 0, 1, 2 }, &slots);
}

test "a tampered key byte is refused" {
    try provision();
    const key = pattern(0x66);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);

    blob[0] ^= 0x01;
    var handle: u32 = 0;
    try std.testing.expectEqual(key_import.Err.invalid_arg, key_import.seal(&blob, &handle));
}

test "a tampered tag byte is refused" {
    try provision();
    const key = pattern(0x77);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);

    blob[Blob.key_bytes] ^= 0x01;
    var handle: u32 = 0;
    try std.testing.expectEqual(key_import.Err.invalid_arg, key_import.seal(&blob, &handle));
}

test "a blob sealed under another KAK is refused" {
    try provision();
    const key = pattern(0x88);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);

    const other = [_]u8{0x41} ** 16;
    try std.testing.expectEqual(vault.Err.ok, vault.setMacKey(&other));
    try std.testing.expectEqual(key_import.Err.invalid_arg, key_import.authenticate(&blob));
}

test "a refused blob takes no slot" {
    try provision();
    const key = pattern(0x99);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);

    var tampered = blob;
    tampered[1] ^= 0xFF;
    var handle: u32 = 0;
    try std.testing.expectEqual(key_import.Err.invalid_arg, key_import.seal(&tampered, &handle));

    // Slot 0 is still free, so the good blob lands there.
    try std.testing.expectEqual(key_import.Err.ok, key_import.seal(&blob, &handle));
    var slot: u16 = 0xFFFF;
    try std.testing.expectEqual(key_import.Err.ok, key_import.resolve(handle, &slot));
    try std.testing.expectEqual(@as(u16, 0), slot);
}

test "with no KAK provisioned every path fails closed" {
    try std.testing.expectEqual(vault.Err.ok, vault.init());
    try std.testing.expectEqual(key_import.Err.ok, key_import.reset());

    const key = pattern(0xAA);
    var blob: key_import.Sealed = .{0} ** Blob.bytes;
    try std.testing.expectEqual(key_import.Err.not_found, key_import.buildBlob(&key, &blob));
    try std.testing.expectEqual(key_import.Err.not_found, key_import.authenticate(&blob));
    var handle: u32 = 0;
    try std.testing.expectEqual(key_import.Err.not_found, key_import.seal(&blob, &handle));
}

test "a full slot array reports no_mem" {
    try provision();
    const key = pattern(0xBB);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);

    var filled: u16 = 0;
    while (filled < vault.Limits.slots) : (filled += 1) {
        var handle: u32 = 0;
        try std.testing.expectEqual(key_import.Err.ok, key_import.seal(&blob, &handle));
    }
    var overflow: u32 = 0;
    try std.testing.expectEqual(key_import.Err.no_mem, key_import.seal(&blob, &overflow));
}

test "an unknown handle does not resolve" {
    try provision();
    var slot: u16 = 0xFFFF;
    try std.testing.expectEqual(key_import.Err.not_found, key_import.resolve(0xDEADBEEF, &slot));
    try std.testing.expectEqual(@as(u16, 0xFFFF), slot);
}

test "no handle resolves on a fresh allocator" {
    try provision();
    const key = pattern(0xCC);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);
    var handle: u32 = 0;
    _ = key_import.seal(&blob, &handle);

    try std.testing.expectEqual(key_import.Err.ok, key_import.reset());
    var slot: u16 = 0xFFFF;
    try std.testing.expectEqual(key_import.Err.not_found, key_import.resolve(handle, &slot));
}

test "reset frees the slots for reuse" {
    try provision();
    const key = pattern(0xDD);
    var blob: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&key, &blob);

    var filled: u16 = 0;
    while (filled < vault.Limits.slots) : (filled += 1) {
        var handle: u32 = 0;
        _ = key_import.seal(&blob, &handle);
    }
    try std.testing.expectEqual(key_import.Err.ok, key_import.reset());

    var handle: u32 = 0;
    try std.testing.expectEqual(key_import.Err.ok, key_import.seal(&blob, &handle));
}

test "an empty-looking blob of the right length is still refused" {
    try provision();
    const blob: key_import.Sealed = .{0} ** Blob.bytes;
    var handle: u32 = 0;
    try std.testing.expectEqual(key_import.Err.invalid_arg, key_import.seal(&blob, &handle));
}

test "two keys in different slots both resolve to their own slot" {
    try provision();
    const first = pattern(0x01);
    const second = pattern(0x02);
    var blob_a: key_import.Sealed = undefined;
    var blob_b: key_import.Sealed = undefined;
    _ = key_import.buildBlob(&first, &blob_a);
    _ = key_import.buildBlob(&second, &blob_b);

    var handle_a: u32 = 0;
    var handle_b: u32 = 0;
    _ = key_import.seal(&blob_a, &handle_a);
    _ = key_import.seal(&blob_b, &handle_b);
    try std.testing.expect(handle_a != handle_b);

    var slot_a: u16 = 0xFFFF;
    var slot_b: u16 = 0xFFFF;
    try std.testing.expectEqual(key_import.Err.ok, key_import.resolve(handle_a, &slot_a));
    try std.testing.expectEqual(key_import.Err.ok, key_import.resolve(handle_b, &slot_b));
    try std.testing.expect(slot_a != slot_b);
}
