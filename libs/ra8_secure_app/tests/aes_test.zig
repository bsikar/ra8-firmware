//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! AES-128 / AES-256 block encryption against the FIPS 197 known-answer
//! vectors, plus the key schedule and the field arithmetic underneath them.

const std = @import("std");
const aes = @import("aes");

/// FIPS 197 Appendix B / C.1 AES-128: key 000102...0f, plaintext 00112233...
const fips_key_128 = [16]u8{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

/// FIPS 197 Appendix C.3 AES-256 key.
const fips_key_256 = [32]u8{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

/// The plaintext both appendices use.
const fips_plaintext = [16]u8{
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

test "AES-128 matches the FIPS 197 C.1 known answer" {
    var schedule = aes.Schedule.init(&fips_key_128);
    defer schedule.deinit();
    const expected = [16]u8{
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
    };
    try std.testing.expectEqual(expected, aes.encryptBlock(&schedule, fips_plaintext));
}

test "AES-256 matches the FIPS 197 C.3 known answer" {
    var schedule = aes.Schedule.init(&fips_key_256);
    defer schedule.deinit();
    const expected = [16]u8{
        0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
        0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89,
    };
    try std.testing.expectEqual(expected, aes.encryptBlock(&schedule, fips_plaintext));
}

test "the two key lengths give different ciphertext for one plaintext" {
    var s128 = aes.Schedule.init(&fips_key_128);
    defer s128.deinit();
    var s256 = aes.Schedule.init(&fips_key_256);
    defer s256.deinit();
    try std.testing.expect(!std.mem.eql(
        u8,
        &aes.encryptBlock(&s128, fips_plaintext),
        &aes.encryptBlock(&s256, fips_plaintext),
    ));
}

test "one flipped plaintext bit changes about half the ciphertext bits" {
    var schedule = aes.Schedule.init(&fips_key_128);
    defer schedule.deinit();
    const base = aes.encryptBlock(&schedule, fips_plaintext);
    var flipped = fips_plaintext;
    flipped[0] ^= 1;
    const other = aes.encryptBlock(&schedule, flipped);

    var differing: usize = 0;
    for (base, other) |a, b| differing += @popCount(a ^ b);
    // 64 of 128 bits is the ideal; the band is wide enough that only a
    // genuinely broken cipher falls outside it.
    try std.testing.expect(differing > 40 and differing < 88);
}

test "KeyLen accepts 16 and 32 and nothing else" {
    try std.testing.expectEqual(aes.KeyLen.aes_128, aes.KeyLen.fromBytes(16).?);
    try std.testing.expectEqual(aes.KeyLen.aes_256, aes.KeyLen.fromBytes(32).?);
    for ([_]usize{ 0, 1, 15, 17, 24, 31, 33, 64 }) |len| {
        try std.testing.expect(aes.KeyLen.fromBytes(len) == null);
    }
}

test "round counts and key lengths follow FIPS 197" {
    try std.testing.expectEqual(@as(usize, 10), aes.KeyLen.aes_128.rounds());
    try std.testing.expectEqual(@as(usize, 14), aes.KeyLen.aes_256.rounds());
    try std.testing.expectEqual(@as(usize, 4), aes.KeyLen.aes_128.words());
    try std.testing.expectEqual(@as(usize, 8), aes.KeyLen.aes_256.words());
    try std.testing.expectEqual(@as(usize, 16), aes.KeyLen.aes_128.bytes());
    try std.testing.expectEqual(@as(usize, 32), aes.KeyLen.aes_256.bytes());
}

test "the schedule exposes only the round keys its variant uses" {
    var s128 = aes.Schedule.init(&fips_key_128);
    defer s128.deinit();
    var s256 = aes.Schedule.init(&fips_key_256);
    defer s256.deinit();
    try std.testing.expectEqual(@as(usize, 16 * 11), s128.used().len);
    try std.testing.expectEqual(@as(usize, 16 * 15), s256.used().len);
    try std.testing.expectEqual(aes.Dim.max_round_key_bytes, s256.used().len);
}

test "the schedule starts with the key itself" {
    var schedule = aes.Schedule.init(&fips_key_256);
    defer schedule.deinit();
    try std.testing.expectEqualSlices(u8, &fips_key_256, schedule.used()[0..32]);
}

test "the FIPS 197 A.1 first expanded words" {
    // Appendix A.1 expands 2b7e1516... ; w[4] is a0fafe17.
    const key = [16]u8{
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
    };
    var schedule = aes.Schedule.init(&key);
    defer schedule.deinit();
    try std.testing.expectEqualSlices(
        u8,
        &[_]u8{ 0xa0, 0xfa, 0xfe, 0x17, 0x88, 0x54, 0x2c, 0xb1 },
        schedule.used()[16..24],
    );
}

test "deinit wipes the schedule" {
    var schedule = aes.Schedule.init(&fips_key_256);
    schedule.deinit();
    for (schedule.bytes) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}

test "xtime doubles in GF(2^8) and reduces on overflow" {
    try std.testing.expectEqual(@as(u8, 0x02), aes.xtime(0x01));
    try std.testing.expectEqual(@as(u8, 0xfe), aes.xtime(0x7f));
    // High bit set: shift out, then fold in the reduction polynomial.
    try std.testing.expectEqual(@as(u8, aes.field_poly), aes.xtime(0x80));
    try std.testing.expectEqual(@as(u8, 0x1b ^ 0x02), aes.xtime(0x81));
    try std.testing.expectEqual(@as(u8, 0x00), aes.xtime(0x00));
}

test "encryption is deterministic" {
    var schedule = aes.Schedule.init(&fips_key_128);
    defer schedule.deinit();
    const first = aes.encryptBlock(&schedule, fips_plaintext);
    const second = aes.encryptBlock(&schedule, fips_plaintext);
    try std.testing.expectEqual(first, second);
}

test "the block size is the AES block size" {
    try std.testing.expectEqual(@as(usize, 16), aes.Dim.block_bytes);
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(aes.Block));
}
