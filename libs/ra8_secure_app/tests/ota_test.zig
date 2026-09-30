//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The masking and the single-shot idempotency policy are what these tests
//! exist for: on silicon the writes fail closed, so the host shadow is the only
//! place the policy can be exercised.

const std = @import("std");
const ota = @import("ota");

test "reset clears a pending swap and the config shadow" {
    try std.testing.expectEqual(ota.Err.ok, ota.reset());
    try std.testing.expectEqual(ota.Err.ok, ota.swapBank(@intFromEnum(ota.Bank.b)));
    try std.testing.expectEqual(ota.Err.ok, ota.setBankConfig(0x3));

    try std.testing.expectEqual(ota.Err.ok, ota.reset());

    var target: ota.Bank = undefined;
    try std.testing.expectEqual(ota.Err.no_data, ota.pendingTarget(&target));
    try std.testing.expectEqual(@as(u32, 0), ota.bankConfig());
}

test "an armed swap reads back as the bank that was requested" {
    try std.testing.expectEqual(ota.Err.ok, ota.reset());
    try std.testing.expectEqual(ota.Err.ok, ota.swapBank(@intFromEnum(ota.Bank.b)));

    var target: ota.Bank = undefined;
    try std.testing.expectEqual(ota.Err.ok, ota.pendingTarget(&target));
    try std.testing.expectEqual(ota.Bank.b, target);
}

test "arming is single-shot" {
    try std.testing.expectEqual(ota.Err.ok, ota.reset());
    try std.testing.expectEqual(ota.Err.ok, ota.swapBank(@intFromEnum(ota.Bank.a)));
    try std.testing.expectEqual(ota.Err.invalid_state, ota.swapBank(@intFromEnum(ota.Bank.b)));

    // The second request must not have overwritten the first.
    var target: ota.Bank = undefined;
    try std.testing.expectEqual(ota.Err.ok, ota.pendingTarget(&target));
    try std.testing.expectEqual(ota.Bank.a, target);
}

test "a bank value outside the enum is refused before the state check" {
    try std.testing.expectEqual(ota.Err.ok, ota.reset());
    try std.testing.expectEqual(ota.Err.invalid_arg, ota.swapBank(2));
    try std.testing.expectEqual(ota.Err.invalid_arg, ota.swapBank(0xFF));

    var target: ota.Bank = undefined;
    try std.testing.expectEqual(ota.Err.no_data, ota.pendingTarget(&target));
}

test "nothing is pending before a swap is armed" {
    try std.testing.expectEqual(ota.Err.ok, ota.reset());
    var target: ota.Bank = undefined;
    try std.testing.expectEqual(ota.Err.no_data, ota.pendingTarget(&target));
}

test "bank config keeps only the two allowed bits" {
    try std.testing.expectEqual(ota.Err.ok, ota.reset());
    try std.testing.expectEqual(ota.Err.ok, ota.setBankConfig(0xFFFF_FFFF));
    try std.testing.expectEqual(ota.Mask.allowed, ota.bankConfig());

    try std.testing.expectEqual(ota.Err.ok, ota.setBankConfig(0xDEAD_BEEF));
    try std.testing.expectEqual(0xDEAD_BEEF & ota.Mask.allowed, ota.bankConfig());

    try std.testing.expectEqual(ota.Err.ok, ota.setBankConfig(0xFFFF_FFFC));
    try std.testing.expectEqual(@as(u32, 0), ota.bankConfig());
}

test "the allowed mask is the two-bit BANK_SEL field" {
    try std.testing.expectEqual(@as(u32, 0x3), ota.Mask.allowed);
}

test "bank raw values map only to the two declared banks" {
    try std.testing.expectEqual(ota.Bank.a, ota.Bank.fromRaw(0).?);
    try std.testing.expectEqual(ota.Bank.b, ota.Bank.fromRaw(1).?);
    try std.testing.expectEqual(@as(?ota.Bank, null), ota.Bank.fromRaw(2));
}
