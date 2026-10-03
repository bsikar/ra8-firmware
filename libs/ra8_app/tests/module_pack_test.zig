//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for packing a ThreadX module binary into a signed `.ra8app`, the
//! path the `ra8app_pack` tool runs.

const std = @import("std");
const module_pack = @import("module_pack");
const pack = module_pack.packer;
const gate = pack.gate;
const appimg = gate.image;
const preamble = module_pack.preamble;

const Ed25519 = std.crypto.sign.Ed25519;
const image_len: usize = 0x2D4;
const header_bytes = @sizeOf(appimg.Header);

const identity: module_pack.Identity = .{
    .app_id = "com.ra8.txm_hello_m33",
    .display_name = "Hello M33",
};

fn module() [image_len]u8 {
    var bytes = [_]u8{0x5A} ** image_len;
    const words = [_]u32{
        preamble.id, 6, 1,     32,    0x12345678, 0x02000007, 0x211,     0x175,
        0,           1, 0x400, 0x16D, 1,          0x400,      image_len, 0x9C,
    };
    for (words, 0..) |value, index| {
        std.mem.writeInt(u32, bytes[index * 4 ..][0..4], value, .little);
    }
    return bytes;
}

fn keyPair() !Ed25519.KeyPair {
    return Ed25519.KeyPair.generateDeterministic([_]u8{0x24} ** Ed25519.KeyPair.seed_length);
}

fn admit(bytes: []const u8, key_pair: Ed25519.KeyPair) gate.Error!appimg.Header {
    const public_key = key_pair.public_key.toBytes();
    return gate.verify(pack.hostBackend(&public_key, appimg.Capability.known), bytes);
}

test "a packed module is admitted with its preamble's entry and stack" {
    const key_pair = try keyPair();
    const bin = module();
    const image = try module_pack.packModule(std.testing.allocator, &bin, identity, key_pair);
    defer std.testing.allocator.free(image);

    const head = try admit(image, key_pair);
    try std.testing.expectEqual(@as(u32, 0x228), head.entry_offset);
    try std.testing.expectEqual(@as(u32, 0x400), head.stack_size);
    try std.testing.expectEqual(@as(u32, image_len), head.code_size);
    try std.testing.expectEqual(@as(u32, 0), head.data_size);
    try std.testing.expectEqualSlices(u8, &bin, image[header_bytes..]);
}

test "a packed module with a changed code byte is refused" {
    const key_pair = try keyPair();
    const bin = module();
    const image = try module_pack.packModule(std.testing.allocator, &bin, identity, key_pair);
    defer std.testing.allocator.free(image);

    image[header_bytes + 0x228] ^= 0x01;
    try std.testing.expectError(gate.Error.BadSignature, admit(image, key_pair));
}

test "a truncated packed module is refused" {
    const key_pair = try keyPair();
    const bin = module();
    const image = try module_pack.packModule(std.testing.allocator, &bin, identity, key_pair);
    defer std.testing.allocator.free(image);

    try std.testing.expectError(appimg.Error.ShortImage, admit(image[0 .. image.len - 1], key_pair));
}

test "a binary that is not a module is never packed" {
    var bin = module();
    bin[0] = 0;
    try std.testing.expectError(
        preamble.Error.NotAModule,
        module_pack.packModule(std.testing.allocator, &bin, identity, try keyPair()),
    );
}
