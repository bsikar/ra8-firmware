//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! AES-CMAC against the published NIST SP 800-38B known-answer vectors, plus
//! the argument checks and the verify verdict the key importer depends on.

const std = @import("std");
const cmac = @import("cmac");

/// SP 800-38B example key, AES-128.
const key_128 = [16]u8{
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
};

/// SP 800-38B example key, AES-256.
const key_256 = [32]u8{
    0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe,
    0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
    0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7,
    0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4,
};

/// The SP 800-38B example message, four blocks. The vectors take the first
/// 0, 16, 40 and 64 bytes of it.
const message = [64]u8{
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
    0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
    0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
    0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
    0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
    0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
    0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
};

const Kat = struct { key: []const u8, len: usize, tag: cmac.Tag };

/// The eight published vectors: both key lengths across the four message
/// lengths (empty, one whole block, a partial block, four whole blocks).
const kats = [_]Kat{
    .{ .key = &key_128, .len = 0, .tag = .{
        0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
        0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46,
    } },
    .{ .key = &key_128, .len = 16, .tag = .{
        0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
        0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c,
    } },
    .{ .key = &key_128, .len = 40, .tag = .{
        0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
        0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27,
    } },
    .{ .key = &key_128, .len = 64, .tag = .{
        0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
        0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe,
    } },
    .{ .key = &key_256, .len = 0, .tag = .{
        0x02, 0x89, 0x62, 0xf6, 0x1b, 0x7b, 0xf8, 0x9e,
        0xfc, 0x6b, 0x55, 0x1f, 0x46, 0x67, 0xd9, 0x83,
    } },
    .{ .key = &key_256, .len = 16, .tag = .{
        0x28, 0xa7, 0x02, 0x3f, 0x45, 0x2e, 0x8f, 0x82,
        0xbd, 0x4b, 0xf2, 0x8d, 0x8c, 0x37, 0xc3, 0x5c,
    } },
    .{ .key = &key_256, .len = 40, .tag = .{
        0xaa, 0xf3, 0xd8, 0xf1, 0xde, 0x56, 0x40, 0xc2,
        0x32, 0xf5, 0xb1, 0x69, 0xb9, 0xc9, 0x11, 0xe6,
    } },
    .{ .key = &key_256, .len = 64, .tag = .{
        0xe1, 0x99, 0x21, 0x90, 0x54, 0x9f, 0x6e, 0xd5,
        0x69, 0x6a, 0x2c, 0x05, 0x6c, 0x31, 0x54, 0x10,
    } },
};

test "every SP 800-38B vector computes its published tag" {
    for (kats) |kat| {
        var out: cmac.Tag = undefined;
        try std.testing.expectEqual(.ok, cmac.compute(kat.key, message[0..kat.len], &out));
        try std.testing.expectEqualSlices(u8, &kat.tag, &out);
    }
}

test "every published tag verifies against its own message" {
    for (kats) |kat| {
        try std.testing.expectEqual(.ok, cmac.verify(kat.key, message[0..kat.len], &kat.tag));
    }
}

test "the empty message is one padded block, not a skipped one" {
    // K2-padded rather than a zero tag: the empty-message vector differs from
    // the tag of a 16-byte zero block.
    var empty: cmac.Tag = undefined;
    var zeros: cmac.Tag = undefined;
    try std.testing.expectEqual(.ok, cmac.compute(&key_128, &.{}, &empty));
    try std.testing.expectEqual(.ok, cmac.compute(&key_128, &[_]u8{0} ** 16, &zeros));
    try std.testing.expect(!std.mem.eql(u8, &empty, &zeros));
}

test "a flipped message byte breaks the verdict" {
    var mutated = message;
    mutated[7] ^= 0x01;
    try std.testing.expectEqual(
        .invalid_arg,
        cmac.verify(&key_128, mutated[0..16], &kats[1].tag),
    );
}

test "a flipped tag byte breaks the verdict" {
    var bad = kats[1].tag;
    bad[15] ^= 0x01;
    try std.testing.expectEqual(.invalid_arg, cmac.verify(&key_128, message[0..16], &bad));
}

test "a truncated tag is rejected without being padded out" {
    // MC/DC V2: the length condition alone decides, and the comparison that
    // follows must not read past the caller's short buffer.
    try std.testing.expectEqual(
        .invalid_arg,
        cmac.verify(&key_128, message[0..16], kats[1].tag[0..15]),
    );
}

test "an over-long tag is rejected too" {
    var long = [_]u8{0} ** 17;
    @memcpy(long[0..16], &kats[1].tag);
    try std.testing.expectEqual(.invalid_arg, cmac.verify(&key_128, message[0..16], &long));
}

test "an empty tag is rejected" {
    try std.testing.expectEqual(.invalid_arg, cmac.verify(&key_128, message[0..16], &.{}));
}

test "the wrong key rejects an otherwise authentic tag" {
    var other = key_128;
    other[0] ^= 0x01;
    try std.testing.expectEqual(.invalid_arg, cmac.verify(&other, message[0..16], &kats[1].tag));
}

test "the wrong key length is refused before any hashing" {
    var out: cmac.Tag = undefined;
    for ([_]usize{ 0, 1, 15, 17, 24, 31, 33 }) |len| {
        const key = ([_]u8{0x11} ** 33)[0..len];
        try std.testing.expectEqual(.invalid_arg, cmac.compute(key, message[0..16], &out));
        try std.testing.expectEqual(.invalid_arg, cmac.verify(key, message[0..16], &kats[1].tag));
    }
}

test "a message past the static cap is refused" {
    const big = [_]u8{0xa5} ** (cmac.Limits.max_msg_bytes + 1);
    var out: cmac.Tag = undefined;
    try std.testing.expectEqual(.invalid_size, cmac.compute(&key_128, &big, &out));
    try std.testing.expectEqual(.invalid_size, cmac.verify(&key_128, &big, &kats[1].tag));
}

test "a message exactly at the cap is accepted" {
    const at_cap = [_]u8{0xa5} ** cmac.Limits.max_msg_bytes;
    var out: cmac.Tag = undefined;
    try std.testing.expectEqual(.ok, cmac.compute(&key_128, &at_cap, &out));
    try std.testing.expectEqual(.ok, cmac.verify(&key_128, &at_cap, &out));
}

test "checkArgs is the same gate both entry points take" {
    try std.testing.expectEqual(.ok, cmac.checkArgs(&key_128, message[0..16]));
    try std.testing.expectEqual(.ok, cmac.checkArgs(&key_256, &.{}));
    try std.testing.expectEqual(.invalid_arg, cmac.checkArgs(key_128[0..15], message[0..16]));
    try std.testing.expectEqual(
        .invalid_size,
        cmac.checkArgs(&key_128, &[_]u8{0} ** (cmac.Limits.max_msg_bytes + 1)),
    );
}

test "a partial final block differs from the same bytes zero-padded" {
    // The 0x80 pad marker and K2 are what separate these two; a naive
    // zero-pad would collide.
    var partial: cmac.Tag = undefined;
    var padded_out: cmac.Tag = undefined;
    var padded = [_]u8{0} ** 16;
    @memcpy(padded[0..8], message[0..8]);
    try std.testing.expectEqual(.ok, cmac.compute(&key_128, message[0..8], &partial));
    try std.testing.expectEqual(.ok, cmac.compute(&key_128, &padded, &padded_out));
    try std.testing.expect(!std.mem.eql(u8, &partial, &padded_out));
}

test "one message under two key lengths gives two tags" {
    var tag_128: cmac.Tag = undefined;
    var tag_256: cmac.Tag = undefined;
    try std.testing.expectEqual(.ok, cmac.compute(&key_128, message[0..40], &tag_128));
    try std.testing.expectEqual(.ok, cmac.compute(&key_256, message[0..40], &tag_256));
    try std.testing.expect(!std.mem.eql(u8, &tag_128, &tag_256));
}

test "compute is deterministic" {
    var first: cmac.Tag = undefined;
    var second: cmac.Tag = undefined;
    try std.testing.expectEqual(.ok, cmac.compute(&key_256, message[0..40], &first));
    try std.testing.expectEqual(.ok, cmac.compute(&key_256, message[0..40], &second));
    try std.testing.expectEqualSlices(u8, &first, &second);
}

test "tag() and compute() agree" {
    var out: cmac.Tag = undefined;
    try std.testing.expectEqual(.ok, cmac.compute(&key_128, message[0..40], &out));
    try std.testing.expectEqualSlices(u8, &cmac.tag(&key_128, message[0..40]), &out);
}

test "the limits match the C header" {
    try std.testing.expectEqual(@as(usize, 16), cmac.Limits.tag_bytes);
    try std.testing.expectEqual(@as(usize, 16), cmac.Limits.key_128);
    try std.testing.expectEqual(@as(usize, 32), cmac.Limits.key_256);
    try std.testing.expectEqual(@as(usize, 256), cmac.Limits.max_msg_bytes);
    try std.testing.expectEqual(cmac.Limits.tag_bytes, @sizeOf(cmac.Tag));
}
