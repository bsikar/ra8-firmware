//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Round-trip tests for the `.ra8app` packer: what it signs, the gate admits
//! under the matching key, and any change to the signed bytes, the key or the
//! signature itself is refused. Format v2 with and without icon metadata.

const std = @import("std");
const pack = @import("appimg_pack");
const gate = pack.gate;
const appimg = gate.image;

comptime {
    _ = @import("app_api_fake.zig");
}

const Ed25519 = std.crypto.sign.Ed25519;
const header_bytes = @sizeOf(appimg.Header);

const code = [_]u8{ 0x00, 0xBF, 0x70, 0x47, 0x01, 0x02, 0x03, 0x04 };
const data = [_]u8{ 0xAA, 0xBB, 0xCC, 0xDD };

const manifest: pack.Manifest = .{
    .entry_offset = 4,
    .stack_size = 0x800,
    .capabilities = appimg.Capability.display,
    .app_id = "com.ra8.hello",
    .display_name = "Hello",
};

fn keyPair(fill: u8) !Ed25519.KeyPair {
    return Ed25519.KeyPair.generateDeterministic(@as([Ed25519.KeyPair.seed_length]u8, @splat(fill)));
}

fn signedImage(key_pair: Ed25519.KeyPair) ![]u8 {
    return pack.pack(std.testing.allocator, manifest, &code, &data, key_pair);
}

fn admit(bytes: []const u8, key_pair: Ed25519.KeyPair) gate.Error!appimg.Header {
    const public_key = key_pair.public_key.toBytes();
    return gate.verify(pack.hostBackend(&public_key, appimg.Capability.known), bytes);
}

test "a packed image is admitted under its own key with the manifest intact" {
    const key_pair = try keyPair(0x42);
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);

    const head = try admit(bytes, key_pair);
    try std.testing.expectEqual(@as(usize, header_bytes + code.len + data.len), bytes.len);
    try std.testing.expectEqual(@as(u32, 2), head.format_version);
    try std.testing.expectEqual(@as(u32, 4), head.entry_offset);
    try std.testing.expectEqual(@as(u32, code.len), head.code_size);
    try std.testing.expectEqual(@as(u32, data.len), head.data_size);
    try std.testing.expectEqual(@as(u32, 0), head.icon_offset);
    try std.testing.expectEqual(@as(u32, 0), head.icon_length);
    try std.testing.expectEqual(appimg.Capability.display, head.capabilities);
    try std.testing.expectEqualStrings("com.ra8.hello", std.mem.sliceTo(&head.app_id, 0));
    try std.testing.expectEqualSlices(u8, &code, bytes[header_bytes..][0..code.len]);
    try std.testing.expectEqualSlices(u8, &data, bytes[header_bytes + code.len ..]);
}

test "a packed image with icon is admitted and icon matches" {
    const icon_data: [64]u8 = @splat(0x55);
    var icon_manifest = manifest;
    icon_manifest.icon = &icon_data;

    const key_pair = try keyPair(0x42);
    const bytes = try pack.pack(std.testing.allocator, icon_manifest, &code, &data, key_pair);
    defer std.testing.allocator.free(bytes);

    const head = try admit(bytes, key_pair);
    try std.testing.expectEqual(@as(u32, 2), head.format_version);
    try std.testing.expectEqual(@as(u32, code.len + data.len), head.icon_offset);
    try std.testing.expectEqual(@as(u32, 64), head.icon_length);
    try std.testing.expectEqual(@as(usize, header_bytes + code.len + data.len + 64), bytes.len);

    const icon = (try appimg.iconSlice(head, bytes)).?;
    try std.testing.expectEqualSlices(u8, &icon_data, icon);
}

test "a flipped icon byte in payload is refused as a bad signature" {
    const icon_data: [64]u8 = @splat(0xAA);
    var icon_manifest = manifest;
    icon_manifest.icon = &icon_data;

    const key_pair = try keyPair(0x42);
    const bytes = try pack.pack(std.testing.allocator, icon_manifest, &code, &data, key_pair);
    defer std.testing.allocator.free(bytes);

    // Flip a byte in the icon payload
    bytes[bytes.len - 1] ^= 0x01;
    try std.testing.expectError(gate.Error.BadSignature, admit(bytes, key_pair));
}

test "a flipped payload byte is refused as a bad signature" {
    const key_pair = try keyPair(0x42);
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);

    bytes[header_bytes + 1] ^= 0x01;
    try std.testing.expectError(gate.Error.BadSignature, admit(bytes, key_pair));
}

test "a flipped signed header byte is refused as a bad signature" {
    const key_pair = try keyPair(0x42);
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);

    bytes[@offsetOf(appimg.Header, "display_name")] ^= 0x20;
    try std.testing.expectError(gate.Error.BadSignature, admit(bytes, key_pair));
}

test "an image signed by another key is refused" {
    const bytes = try signedImage(try keyPair(0x42));
    defer std.testing.allocator.free(bytes);

    try std.testing.expectError(gate.Error.BadSignature, admit(bytes, try keyPair(0x43)));
}

test "a corrupted signature is refused" {
    const key_pair = try keyPair(0x42);
    const bytes = try signedImage(key_pair);
    defer std.testing.allocator.free(bytes);

    bytes[appimg.Header.signature_offset + 3] ^= 0x80;
    try std.testing.expectError(gate.Error.BadSignature, admit(bytes, key_pair));
}

test "an assembled but unsigned image is refused before any verify" {
    const key_pair = try keyPair(0x42);
    const bytes = try pack.assemble(std.testing.allocator, manifest, &code, &data);
    defer std.testing.allocator.free(bytes);

    try std.testing.expectError(appimg.Error.Validation, admit(bytes, key_pair));
}

test "packing the same input twice gives the same bytes" {
    const key_pair = try keyPair(0x42);
    const first = try signedImage(key_pair);
    defer std.testing.allocator.free(first);
    const second = try signedImage(key_pair);
    defer std.testing.allocator.free(second);

    try std.testing.expectEqualSlices(u8, first, second);
}

test "a name that fills its whole field is refused" {
    var long = manifest;
    long.app_id = &@as([appimg.Width.app_id:0]u8, @splat('a'));
    try std.testing.expectError(
        pack.Error.NameTooLong,
        pack.assemble(std.testing.allocator, long, &code, &data),
    );
}

test "a manifest the parser would refuse is never assembled" {
    var bad = manifest;
    bad.entry_offset = code.len;
    try std.testing.expectError(
        appimg.Error.OutOfRange,
        pack.assemble(std.testing.allocator, bad, &code, &data),
    );
    bad = manifest;
    bad.stack_size = appimg.Format.stack_min - 4;
    try std.testing.expectError(
        appimg.Error.OutOfRange,
        pack.assemble(std.testing.allocator, bad, &code, &data),
    );
}

test "legacy format v1 packaging can be requested explicitly" {
    var v1_manifest = manifest;
    v1_manifest.format_version = appimg.Format.version_v1;

    const key_pair = try keyPair(0x42);
    const bytes = try pack.pack(std.testing.allocator, v1_manifest, &code, &data, key_pair);
    defer std.testing.allocator.free(bytes);

    const head = try admit(bytes, key_pair);
    try std.testing.expectEqual(@as(u32, 1), head.format_version);
    try std.testing.expectEqual(@as(usize, 160 + code.len + data.len), bytes.len);
}
