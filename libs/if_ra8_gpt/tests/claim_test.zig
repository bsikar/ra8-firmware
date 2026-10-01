//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The channel-ownership table both GPT adapters share.

const std = @import("std");
const claim = @import("claim");

const Err = struct {
    const ok: u16 = 0;
    const invalid_arg: u16 = 0x103;
    const busy: u16 = 0x109;
};

test "a free channel is claimed once, then reads busy to either port" {
    claim.resetForTest();
    try std.testing.expectEqual(Err.ok, claim.claim(3, .timer));
    try std.testing.expectEqual(Err.busy, claim.claim(3, .timer));
    try std.testing.expectEqual(Err.busy, claim.claim(3, .pwm));
    try std.testing.expect(claim.ownedBy(3, .timer));
    try std.testing.expect(!claim.ownedBy(3, .pwm));
}

test "release frees only for the holder" {
    claim.resetForTest();
    try std.testing.expectEqual(Err.ok, claim.claim(0, .pwm));
    claim.release(0, .timer);
    try std.testing.expect(claim.ownedBy(0, .pwm));
    claim.release(0, .pwm);
    try std.testing.expect(!claim.ownedBy(0, .pwm));
    try std.testing.expectEqual(Err.ok, claim.claim(0, .timer));
}

test "out-of-range channel and the none owner are rejected" {
    claim.resetForTest();
    try std.testing.expectEqual(Err.invalid_arg, claim.claim(claim.channel_count, .timer));
    try std.testing.expectEqual(Err.invalid_arg, claim.claim(0, .none));
    try std.testing.expect(!claim.ownedBy(claim.channel_count, .timer));
    try std.testing.expect(!claim.ownedBy(0, .none));
}

test "every channel can be held at once" {
    claim.resetForTest();
    var ch: u8 = 0;
    while (ch < claim.channel_count) : (ch += 1) {
        const owner: claim.Owner = if (ch % 2 == 0) .timer else .pwm;
        try std.testing.expectEqual(Err.ok, claim.claim(ch, owner));
    }
}
