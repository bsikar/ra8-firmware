//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the ra8app setup example walkthrough (RA8FW-1224 / RA8FW-1282).

const std = @import("std");
const module_pack = @import("module_pack");
const setup_example = @import("setup_example");

const appimg = module_pack.packer.gate.image;
const header_bytes = @sizeOf(appimg.Header);

test "the example module packs, signs and is admitted with its entry, stack and grant" {
    const key_pair = try setup_example.keyPair();
    const bin = setup_example.module();
    const image = try module_pack.packModule(std.testing.allocator, &bin, setup_example.identity, key_pair);
    defer std.testing.allocator.free(image);

    const head = try setup_example.admit(image, key_pair, appimg.Capability.known);
    try std.testing.expectEqual(@as(u32, 0x228), head.entry_offset);
    try std.testing.expectEqual(@as(u32, 0x400), head.stack_size);
    try std.testing.expectEqual(@as(u32, setup_example.image_len), head.code_size);
    try std.testing.expectEqual(@as(u32, 0), head.data_size);
    try std.testing.expectEqual(setup_example.identity.capabilities, head.capabilities);
    try std.testing.expectEqualSlices(u8, &bin, image[header_bytes..]);
}

test "a host that withholds the declared capability refuses the load" {
    const key_pair = try setup_example.keyPair();
    const bin = setup_example.module();
    const image = try module_pack.packModule(std.testing.allocator, &bin, setup_example.identity, key_pair);
    defer std.testing.allocator.free(image);

    // The app declared display|storage; a host granting nothing must refuse
    // the load rather than run the app with an empty permission set.
    try setup_example.expectRefused(image, key_pair, appimg.Capability.none);
}

test "a tampered image is refused under a full grant" {
    const key_pair = try setup_example.keyPair();
    const bin = setup_example.module();
    const image = try module_pack.packModule(std.testing.allocator, &bin, setup_example.identity, key_pair);
    defer std.testing.allocator.free(image);

    image[header_bytes + 0x228] ^= 0x01;
    try setup_example.expectRefused(image, key_pair, appimg.Capability.known);
}

test "a truncated image is refused under a full grant" {
    const key_pair = try setup_example.keyPair();
    const bin = setup_example.module();
    const image = try module_pack.packModule(std.testing.allocator, &bin, setup_example.identity, key_pair);
    defer std.testing.allocator.free(image);

    try setup_example.expectRefused(image[0 .. image.len - 4], key_pair, appimg.Capability.known);
}
