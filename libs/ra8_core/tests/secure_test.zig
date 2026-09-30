//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The secure-comparison and scrub contracts (#2908).
//!
//! What these pin is behaviour, not timing: no host test can prove the
//! compare is constant-work, and `tests/security/src/test_ra8_secure_cov.c`
//! asserts the same outcomes from the other side of the ABI. The value here
//! is that a mismatch anywhere in the buffer is still caught, including at
//! the last byte, which is where an early-out version would still look
//! correct.

const std = @import("std");

const compare = @import("secure_compare");
const scrub = @import("secure_scrub");

test "identical buffers are equal" {
    const a = [_]u8{ 0xDE, 0xAD, 0xBE, 0xEF };
    const b = [_]u8{ 0xDE, 0xAD, 0xBE, 0xEF };
    try std.testing.expect(compare.equal(&a, &b));
}

test "two empty buffers are vacuously equal" {
    try std.testing.expect(compare.equal(&.{}, &.{}));
}

test "a difference is caught at the first, middle and last byte alike" {
    const a = [_]u8{ 0x11, 0x22, 0x33, 0x44 };
    try std.testing.expect(!compare.equal(&a, &[_]u8{ 0xFF, 0x22, 0x33, 0x44 }));
    try std.testing.expect(!compare.equal(&a, &[_]u8{ 0x11, 0xFF, 0x33, 0x44 }));
    try std.testing.expect(!compare.equal(&a, &[_]u8{ 0x11, 0x22, 0x33, 0xFF }));
}

test "different lengths are not equal" {
    try std.testing.expect(!compare.equal(&[_]u8{ 1, 2, 3 }, &[_]u8{ 1, 2 }));
}

test "the scrub clears every byte" {
    var secret = [_]u8{0xA5} ** 16;
    scrub.zeroize(&secret);
    for (secret) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
}

test "the scrub clears exactly the slice it is given" {
    var buf = [_]u8{0xA5} ** 8;
    scrub.zeroize(buf[2..6]);
    for (buf[0..2]) |byte| try std.testing.expectEqual(@as(u8, 0xA5), byte);
    for (buf[2..6]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    for (buf[6..]) |byte| try std.testing.expectEqual(@as(u8, 0xA5), byte);
}

test "an empty scrub writes nothing" {
    var buf = [_]u8{0xA5} ** 4;
    scrub.zeroize(buf[2..2]);
    for (buf) |byte| try std.testing.expectEqual(@as(u8, 0xA5), byte);
}
