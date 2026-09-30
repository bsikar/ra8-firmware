//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The fetch decision and the download size cap.

const std = @import("std");
const testing = std.testing;

const policy = @import("policy");

const AddrClass = policy.AddrClass;

test "public is fetchable whatever the caller opted into" {
    try testing.expect(policy.fetchable(.public, false));
    try testing.expect(policy.fetchable(.public, true));
}

test "unknown is never fetchable, even with the opt-in" {
    try testing.expect(!policy.fetchable(.unknown, false));
    try testing.expect(!policy.fetchable(.unknown, true));
}

test "the other classes ride the opt-in" {
    for ([_]AddrClass{ .loopback, .private, .linklocal }) |cls| {
        try testing.expect(!policy.fetchable(cls, false));
        try testing.expect(policy.fetchable(cls, true));
    }
}

test "a zero cap means no cap" {
    try testing.expect(!policy.sizeExceeds(0, 0, 0));
    try testing.expect(!policy.sizeExceeds(1 << 40, 1 << 40, 0));
}

test "the cap is a ceiling the total may reach but not pass" {
    try testing.expect(!policy.sizeExceeds(0, 100, 100));
    try testing.expect(policy.sizeExceeds(0, 101, 100));
    try testing.expect(!policy.sizeExceeds(60, 40, 100));
    try testing.expect(policy.sizeExceeds(60, 41, 100));
}

test "already over the cap exceeds before anything is added" {
    try testing.expect(policy.sizeExceeds(101, 0, 100));
}

test "the sum is never formed, so a hostile length cannot wrap past the check" {
    const max = std.math.maxInt(u64);
    try testing.expect(policy.sizeExceeds(10, max, 100));
    try testing.expect(policy.sizeExceeds(max, max, 100));
}
