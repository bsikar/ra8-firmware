//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The anti-rollback decisions, driven with no storage and no device.
//!
//! These are the cases `tests/misc/src/test_ra8_dfu_antirollback.c` had to
//! reach by including the implementation: the MC/DC pairs on the downgrade
//! policy, the erased-word mapping, and the monotone commit. They are
//! reachable here because the decisions carry no storage with them.

const std = @import("std");
const ar = @import("antirollback");

test "the policy accepts newer and equal, denies older" {
    // MC/DC on `image_version >= stored_min`: each side decides the outcome
    // on its own with the other held fixed.
    try std.testing.expect(ar.accepts(3, 2));
    try std.testing.expect(ar.accepts(2, 2));
    try std.testing.expect(!ar.accepts(1, 2));

    // A fresh device (floor 0) accepts anything, including version 0.
    try std.testing.expect(ar.accepts(0, 0));
    try std.testing.expect(ar.accepts(std.math.maxInt(u32), 0));

    // The floor at its maximum denies everything below it.
    try std.testing.expect(!ar.accepts(std.math.maxInt(u32) - 1, std.math.maxInt(u32)));
    try std.testing.expect(ar.accepts(std.math.maxInt(u32), std.math.maxInt(u32)));
}

test "an erased counter word is a floor of zero, every other word is itself" {
    try std.testing.expectEqual(@as(u32, 0), ar.storedFrom(ar.Nv.erased));
    try std.testing.expectEqual(@as(u32, 0), ar.storedFrom(0));
    try std.testing.expectEqual(@as(u32, 7), ar.storedFrom(7));

    // One bit below erased is a real version, not a blank word.
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFE), ar.storedFrom(0xFFFF_FFFE));
}

test "the counter only advances, so a re-flash costs no program cycle" {
    try std.testing.expect(ar.needsCommit(3, 2));
    try std.testing.expect(!ar.needsCommit(2, 2));
    try std.testing.expect(!ar.needsCommit(1, 2));

    // An erased device has floor 0, so the first authentic image is written.
    try std.testing.expect(ar.needsCommit(1, ar.storedFrom(ar.Nv.erased)));
    // ... but a version-0 first image needs nothing persisted.
    try std.testing.expect(!ar.needsCommit(0, ar.storedFrom(ar.Nv.erased)));
}

test "accepted-and-not-committed is exactly the same-version re-flash" {
    // The two decisions meet on equality: the policy accepts it and the
    // counter stays put. Any version the policy denies is never offered to
    // the commit decision at all.
    var v: u32 = 0;
    while (v < 8) : (v += 1) {
        const stored: u32 = 4;
        if (ar.accepts(v, stored)) {
            try std.testing.expectEqual(v > stored, ar.needsCommit(v, stored));
        } else {
            try std.testing.expect(v < stored);
        }
    }
}

test "the stacked PC advances by the real Thumb instruction width" {
    // ARMv7-M ARM A5.1: first halfword bits [15:11] >= 0b11101 means 32-bit.
    try std.testing.expectEqual(@as(u32, 2), ar.instructionWidth(0x0000));
    try std.testing.expectEqual(@as(u32, 2), ar.instructionWidth(0x6800)); // LDR  (16-bit)
    try std.testing.expectEqual(@as(u32, 2), ar.instructionWidth(0xE7FE)); // B    (16-bit)
    try std.testing.expectEqual(@as(u32, 4), ar.instructionWidth(0xE800)); // the boundary
    try std.testing.expectEqual(@as(u32, 4), ar.instructionWidth(0xF8D0)); // LDR.W(32-bit)
    try std.testing.expectEqual(@as(u32, 4), ar.instructionWidth(0xFFFF));

    // The decision is on [15:11] alone: the low bits never move it.
    try std.testing.expectEqual(
        ar.instructionWidth(0xE800),
        ar.instructionWidth(0xE800 | 0x07FF),
    );
}
