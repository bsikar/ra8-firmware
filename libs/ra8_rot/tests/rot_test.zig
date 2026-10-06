//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The root-of-trust decision table, driven with no crypto engine.
//!
//! These cases were white-box C in `tests/security/src/test_ra8_root_of_trust.c`,
//! which reached `internal_ct_equal` and `internal_bind_version` by
//! `#include`-ing `ra8_rot.c`. They are reachable here because the decisions
//! are separated from the hash and the ECDSA verify, so the suite no longer
//! has to be welded to the translation unit to see them.

const std = @import("std");
const rot = @import("rot");

fn signedTrailer(body_len: u32) rot.Trailer {
    return .{
        .magic = rot.Format.trailer_magic,
        .version = rot.Format.version,
        .img_version = 7,
        .body_len = body_len,
        .sig_len = rot.Size.sig_bytes,
        .digest = @splat(0xAB),
        .sig = @splat(0xCD),
    };
}

test "a well-formed trailer passes the screen" {
    const trailer = signedTrailer(256);
    try std.testing.expectEqual(rot.Verdict.ok, rot.screen(&trailer, 256));
}

test "a wrong magic or format version is a malformed trailer" {
    var trailer = signedTrailer(256);
    trailer.magic = rot.Format.trailer_magic ^ 1;
    try std.testing.expectEqual(rot.Verdict.malformed, rot.screen(&trailer, 256));

    trailer = signedTrailer(256);
    trailer.version = rot.Format.version + 1;
    try std.testing.expectEqual(rot.Verdict.malformed, rot.screen(&trailer, 256));
}

test "body_len is refused at zero, past the cap, and when the trailer disagrees" {
    const trailer = signedTrailer(256);
    try std.testing.expectEqual(rot.Verdict.bad_body_len, rot.screen(&signedTrailer(0), 0));
    try std.testing.expectEqual(
        rot.Verdict.bad_body_len,
        rot.screen(&signedTrailer(rot.Format.body_max + 1), rot.Format.body_max + 1),
    );
    // Same trailer, a body one byte longer than it claims to cover.
    try std.testing.expectEqual(rot.Verdict.bad_body_len, rot.screen(&trailer, 257));
}

test "sig_len is refused at zero and past an ECDSA-P256 signature" {
    var trailer = signedTrailer(256);
    trailer.sig_len = 0;
    try std.testing.expectEqual(rot.Verdict.bad_sig_len, rot.screen(&trailer, 256));

    trailer.sig_len = rot.Size.sig_bytes + 1;
    try std.testing.expectEqual(rot.Verdict.bad_sig_len, rot.screen(&trailer, 256));

    // The off-target stand-in signature is shorter than the on-target one and
    // must still pass: sig_len records the active length.
    trailer.sig_len = rot.Size.digest_bytes;
    try std.testing.expectEqual(rot.Verdict.ok, rot.screen(&trailer, 256));
}

test "the screen checks the trailer before the lengths" {
    // A trailer that is wrong in both ways reports the trailer, so a caller
    // logging the verdict names the first thing that failed.
    var trailer = signedTrailer(0);
    trailer.magic = 0;
    try std.testing.expectEqual(rot.Verdict.malformed, rot.screen(&trailer, 0));
}

test "constant-time compare: equal, differing, and the degenerate lengths" {
    const a: [rot.Size.digest_bytes]u8 = @splat(0x5A);
    var b: [rot.Size.digest_bytes]u8 = @splat(0x5A);
    try std.testing.expect(rot.equalConstantTime(&a, &b));

    // A difference in the last byte is as fatal as one in the first: the fold
    // has no early exit.
    b[rot.Size.digest_bytes - 1] ^= 0x01;
    try std.testing.expect(!rot.equalConstantTime(&a, &b));
    b = @splat(0x5A);
    b[0] ^= 0x80;
    try std.testing.expect(!rot.equalConstantTime(&a, &b));

    // Zero length is not a match, and neither is a length mismatch.
    try std.testing.expect(!rot.equalConstantTime(&.{}, &.{}));
    try std.testing.expect(!rot.equalConstantTime(&a, b[0 .. rot.Size.digest_bytes - 1]));
}

test "the signed material is the little-endian version ahead of the digest" {
    const digest: [rot.Size.digest_bytes]u8 = @splat(0x11);
    const material = rot.signedMaterial(0x0403_0201, digest);

    try std.testing.expectEqual(@as(usize, 4 + rot.Size.digest_bytes), material.len);
    try std.testing.expectEqualSlices(u8, &.{ 0x01, 0x02, 0x03, 0x04 }, material[0..4]);
    try std.testing.expectEqualSlices(u8, &digest, material[4..]);
}

test "a different version changes the signed material, which is the point" {
    const digest: [rot.Size.digest_bytes]u8 = @splat(0x11);
    const first = rot.signedMaterial(1, digest);
    const forged = rot.signedMaterial(2, digest);
    try std.testing.expect(!std.mem.eql(u8, &first, &forged));
}

test "the trailer offset is the body length, and refuses an impossible one" {
    try std.testing.expectEqual(@as(?u32, 256), rot.trailerOffset(256));
    try std.testing.expectEqual(@as(?u32, rot.Format.body_max), rot.trailerOffset(rot.Format.body_max));
    try std.testing.expectEqual(@as(?u32, null), rot.trailerOffset(0));
    try std.testing.expectEqual(@as(?u32, null), rot.trailerOffset(rot.Format.body_max + 1));
}

test "the trailer layout matches the C header's static_assert" {
    try std.testing.expectEqual(@as(usize, 20 + 32 + 64), @sizeOf(rot.Trailer));
    try std.testing.expectEqual(@as(usize, 20), @offsetOf(rot.Trailer, "digest"));
    try std.testing.expectEqual(@as(usize, 52), @offsetOf(rot.Trailer, "sig"));
}
