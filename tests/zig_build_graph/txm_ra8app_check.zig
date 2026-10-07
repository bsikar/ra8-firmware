//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The built arm/txm_hello_m33.ra8app against the in-tree verifier
//! (RA8FW-479): the file the build signed is admitted with the module's own
//! entry and stack, and a copy with one changed byte or a missing tail is
//! refused. txm_ra8app.zig compiles this only when an ARM toolchain built the
//! module, so it is its own test root rather than part of build_graph_test.zig.

const std = @import("std");
const built = @import("built");
const module_pack = @import("module_pack");
const test_key = @import("test_key");

const pack = module_pack.packer;
const gate = pack.gate;
const appimg = gate.image;
const Ed25519 = std.crypto.sign.Ed25519;
const header_bytes = @sizeOf(appimg.Header);

fn load() ![]u8 {
    return std.Io.Dir.cwd().readFileAlloc(std.testing.io, built.ra8app, std.testing.allocator, .limited(1024 * 1024));
}

fn admit(bytes: []const u8) !appimg.Header {
    const key_pair = try Ed25519.KeyPair.generateDeterministic(test_key.seed);
    const public_key = key_pair.public_key.toBytes();
    return gate.verify(pack.hostBackend(&public_key, appimg.Capability.known), bytes);
}

fn expectRefused(bytes: []const u8) !void {
    if (admit(bytes)) |_| return error.TestUnexpectedResult else |_| {}
}

test "the built hello-world module is admitted with its preamble's entry and stack" {
    const bytes = try load();
    defer std.testing.allocator.free(bytes);
    const described = try module_pack.preamble.read(bytes[header_bytes..]);

    const head = try admit(bytes);
    try std.testing.expectEqual(described.entry_offset, head.entry_offset);
    try std.testing.expectEqual(described.stack_size, head.stack_size);
    try std.testing.expectEqual(@as(u32, @intCast(bytes.len - header_bytes)), head.code_size);
    try std.testing.expectEqual(@as(u32, 0), head.data_size);
}

test "the built module with one changed code byte is refused" {
    const bytes = try load();
    defer std.testing.allocator.free(bytes);
    bytes[bytes.len - 1] ^= 0x01;
    try expectRefused(bytes);
}

test "the built module without its last word is refused" {
    const bytes = try load();
    defer std.testing.allocator.free(bytes);
    try expectRefused(bytes[0 .. bytes.len - 4]);
}
