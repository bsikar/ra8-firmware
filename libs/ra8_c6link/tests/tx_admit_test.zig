//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the verdict one outgoing Ethernet frame is admitted under.

const std = @import("std");
const tx_admit = @import("implementation").tx_admit;
const frame = @import("implementation").frame;

test "an open link with a free slot takes an ordinary frame" {
    try tx_admit.admit(true, 64, 0);
}

test "a closed link takes nothing" {
    try std.testing.expectError(error.NotInitialized, tx_admit.admit(false, 64, 0));
}

test "an empty frame carries nothing and is refused" {
    try std.testing.expectError(error.InvalidSize, tx_admit.admit(true, 0, 0));
}

test "the shortest admitted frame is one octet" {
    try tx_admit.admit(true, tx_admit.Bound.min, 0);
    try std.testing.expectEqual(@as(u16, 1), tx_admit.Bound.min);
}

test "a frame filling the payload exactly is admitted" {
    try tx_admit.admit(true, tx_admit.Bound.max, 0);
}

test "one octet past the payload is refused" {
    try std.testing.expectError(error.InvalidSize, tx_admit.admit(true, tx_admit.Bound.max + 1, 0));
}

test "the length bound is the frame geometry's payload, not a second number" {
    try std.testing.expectEqual(frame.Frame.max_payload, tx_admit.Bound.max);
}

test "an occupied slot is busy, not a size refusal" {
    try std.testing.expectError(error.Busy, tx_admit.admit(true, 64, 1));
}

test "a closed link is reported before a bad length" {
    try std.testing.expectError(error.NotInitialized, tx_admit.admit(false, 0, 0));
    try std.testing.expectError(error.NotInitialized, tx_admit.admit(false, 9999, 7));
}

test "a bad length is reported before an occupied slot" {
    try std.testing.expectError(error.InvalidSize, tx_admit.admit(true, 0, 7));
    try std.testing.expectError(error.InvalidSize, tx_admit.admit(true, 9999, 7));
}

test "every length in the payload range is admitted on an idle open link" {
    var len: u16 = tx_admit.Bound.min;
    while (len <= tx_admit.Bound.max) : (len += 1) {
        tx_admit.admit(true, len, 0) catch return error.TestUnexpectedResult;
    }
}

test "no length is admitted while the slot is occupied" {
    var len: u16 = 0;
    while (len < 2048) : (len += 1) {
        try std.testing.expect(std.meta.isError(tx_admit.admit(true, len, 1)));
    }
}
