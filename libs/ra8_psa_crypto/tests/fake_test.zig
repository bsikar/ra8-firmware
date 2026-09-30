//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host cover for the off-target crypto stand-ins.

const std = @import("std");
const fake = @import("fake");

const tag_len = 16;

fn hex(comptime s: []const u8) [s.len / 2]u8 {
    var out: [s.len / 2]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, s) catch unreachable;
    return out;
}

test "sha256 matches the FIPS 180-4 empty-string vector" {
    var digest: [32]u8 = undefined;
    fake.sha256("", &digest);
    try std.testing.expectEqualSlices(
        u8,
        &hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        &digest,
    );
}

test "sha256 matches the abc vector" {
    var digest: [32]u8 = undefined;
    fake.sha256("abc", &digest);
    try std.testing.expectEqualSlices(
        u8,
        &hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        &digest,
    );
}

test "sha256 spans the 55, 56 and 64 byte padding boundaries" {
    // The C picked one or two tail blocks on `remaining < 56`; each of these
    // lands on a different side of that decision.
    inline for (.{ 55, 56, 64, 119, 120 }) |len| {
        var input: [len]u8 = undefined;
        for (&input, 0..) |*byte, i| byte.* = @truncate(i);
        var mine: [32]u8 = undefined;
        fake.sha256(&input, &mine);
        var reference: [32]u8 = undefined;
        std.crypto.hash.sha2.Sha256.hash(&input, &reference, .{});
        try std.testing.expectEqualSlices(u8, &reference, &mine);
    }
}

test "keystream is deterministic and key-dependent" {
    var a: [70]u8 = undefined;
    var b: [70]u8 = undefined;
    fake.keystream("key", "nonce-123456", &a);
    fake.keystream("key", "nonce-123456", &b);
    try std.testing.expectEqualSlices(u8, &a, &b);

    var other: [70]u8 = undefined;
    fake.keystream("kez", "nonce-123456", &other);
    try std.testing.expect(!std.mem.eql(u8, &a, &other));
}

test "keystream crosses the 32 byte block boundary" {
    // 70 bytes needs three SHA-256 blocks, so a counter bug shows up as a
    // repeat at offset 32.
    var stream: [70]u8 = undefined;
    fake.keystream("k", "n", &stream);
    try std.testing.expect(!std.mem.eql(u8, stream[0..32], stream[32..64]));
}

test "tag truncates the digest to 16 bytes" {
    var computed: [tag_len]u8 = undefined;
    fake.tag("k", "n", "a", "c", &computed);
    var digest: [32]u8 = undefined;
    var buffer: [4]u8 = undefined;
    @memcpy(&buffer, "knac");
    fake.sha256(&buffer, &digest);
    try std.testing.expectEqualSlices(u8, digest[0..tag_len], &computed);
}

test "tag changes with every input it covers" {
    var base: [tag_len]u8 = undefined;
    fake.tag("key", "nonce", "aad", "cipher", &base);
    inline for (.{
        .{ "KEY", "nonce", "aad", "cipher" },
        .{ "key", "NONCE", "aad", "cipher" },
        .{ "key", "nonce", "AAD", "cipher" },
        .{ "key", "nonce", "aad", "CIPHER" },
    }) |case| {
        var other: [tag_len]u8 = undefined;
        fake.tag(case[0], case[1], case[2], case[3], &other);
        try std.testing.expect(!std.mem.eql(u8, &base, &other));
    }
}

test "aead round-trips and recovers the plaintext" {
    const plain = "the quick brown fox";
    var sealed: [plain.len + tag_len]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0), fake.aeadEncrypt("key", "nonce", "aad", plain, &sealed));

    var opened: [plain.len]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0), fake.aeadDecrypt("key", "nonce", "aad", &sealed, &opened));
    try std.testing.expectEqualSlices(u8, plain, &opened);
}

test "aead ciphertext is not the plaintext" {
    const plain = "the quick brown fox";
    var sealed: [plain.len + tag_len]u8 = undefined;
    _ = fake.aeadEncrypt("key", "nonce", "aad", plain, &sealed);
    try std.testing.expect(!std.mem.eql(u8, plain, sealed[0..plain.len]));
}

test "aead rejects a tampered tag, ciphertext, aad or nonce" {
    const plain = "payload";
    var sealed: [plain.len + tag_len]u8 = undefined;
    _ = fake.aeadEncrypt("key", "nonce", "aad", plain, &sealed);
    var opened: [plain.len]u8 = undefined;

    var flipped_tag = sealed;
    flipped_tag[plain.len] ^= 0x01;
    try std.testing.expectEqual(
        @as(u16, 0x405),
        fake.aeadDecrypt("key", "nonce", "aad", &flipped_tag, &opened),
    );

    var flipped_body = sealed;
    flipped_body[0] ^= 0x01;
    try std.testing.expectEqual(
        @as(u16, 0x405),
        fake.aeadDecrypt("key", "nonce", "aad", &flipped_body, &opened),
    );

    try std.testing.expectEqual(
        @as(u16, 0x405),
        fake.aeadDecrypt("key", "nonce", "AAD", &sealed, &opened),
    );
    try std.testing.expectEqual(
        @as(u16, 0x405),
        fake.aeadDecrypt("key", "NONCE", "aad", &sealed, &opened),
    );
}

test "aead handles an empty plaintext as tag only" {
    var sealed: [tag_len]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0), fake.aeadEncrypt("key", "nonce", "aad", "", &sealed));
    var opened: [0]u8 = undefined;
    try std.testing.expectEqual(@as(u16, 0), fake.aeadDecrypt("key", "nonce", "aad", &sealed, &opened));
}

test "aead refuses a plaintext past the scratch budget" {
    var plain: [fake.scratch_bytes + 1]u8 = undefined;
    @memset(&plain, 0xAB);
    var sealed: [plain.len + tag_len]u8 = undefined;
    try std.testing.expectEqual(
        @as(u16, 0x105),
        fake.aeadEncrypt("key", "nonce", "aad", &plain, &sealed),
    );
}

test "tag truncation past the scratch budget is documented, not an error" {
    // The C appended each field only while `off < sizeof buf`, so two inputs
    // that differ beyond 256 bytes share a tag. Pinned so the port cannot
    // quietly "fix" it into a behaviour change.
    var long_a: [400]u8 = undefined;
    var long_b: [400]u8 = undefined;
    @memset(&long_a, 0x11);
    @memset(&long_b, 0x11);
    long_b[399] = 0x22;
    var tag_a: [tag_len]u8 = undefined;
    var tag_b: [tag_len]u8 = undefined;
    fake.tag("k", "n", "", &long_a, &tag_a);
    fake.tag("k", "n", "", &long_b, &tag_b);
    try std.testing.expectEqualSlices(u8, &tag_a, &tag_b);
}

test "xorshift32 is reproducible from a fresh generator" {
    var first: fake.Rng = .{};
    var second: fake.Rng = .{};
    var a: [32]u8 = undefined;
    var b: [32]u8 = undefined;
    first.fill(&a);
    second.fill(&b);
    try std.testing.expectEqualSlices(u8, &a, &b);
}

test "xorshift32 advances rather than repeating" {
    var rng: fake.Rng = .{};
    var a: [16]u8 = undefined;
    var b: [16]u8 = undefined;
    rng.fill(&a);
    rng.fill(&b);
    try std.testing.expect(!std.mem.eql(u8, &a, &b));
}

test "xorshift32 matches the Marsaglia y[13,17,5] sequence" {
    var rng: fake.Rng = .{};
    var state: u32 = 0xACE1ACE1;
    var expected: [8]u8 = undefined;
    for (&expected) |*byte| {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte.* = @truncate(state);
    }
    var got: [8]u8 = undefined;
    rng.fill(&got);
    try std.testing.expectEqualSlices(u8, &expected, &got);
}

test "xorshift32 never stalls on zero" {
    var rng: fake.Rng = .{};
    var seen_nonzero = false;
    var out: [64]u8 = undefined;
    rng.fill(&out);
    for (out) |byte| {
        if (byte != 0) seen_nonzero = true;
    }
    try std.testing.expect(seen_nonzero);
}
