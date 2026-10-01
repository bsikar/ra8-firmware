//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The rules one set of station credentials is held to.

const std = @import("std");
const implementation = @import("implementation");

const sta_cfg = implementation.sta_cfg;

test "a terminated string measures up to its terminator" {
    try std.testing.expectEqual(@as(u8, 9), sta_cfg.length("ra8-bench\x00garbage"));
}

test "an empty string measures zero" {
    try std.testing.expectEqual(@as(u8, 0), sta_cfg.length("\x00"));
}

test "an unterminated buffer measures as the whole buffer" {
    const packed_full = [_]u8{'x'} ** 33;
    try std.testing.expectEqual(@as(u8, 33), sta_cfg.length(&packed_full));
}

test "an empty buffer measures zero" {
    const none: []const u8 = &.{};
    try std.testing.expectEqual(@as(u8, 0), sta_cfg.length(none));
}

test "an ordinary network is joinable" {
    try sta_cfg.credentialsValid(9, 12);
}

test "an open network carries no passphrase" {
    try sta_cfg.credentialsValid(9, 0);
}

test "an empty ssid names no network" {
    try std.testing.expectError(error.InvalidSize, sta_cfg.credentialsValid(0, 12));
}

test "the longest allowed ssid is accepted" {
    try sta_cfg.credentialsValid(sta_cfg.Bound.ssid_max, 0);
}

test "an ssid one octet past the bound is refused" {
    try std.testing.expectError(
        error.InvalidSize,
        sta_cfg.credentialsValid(sta_cfg.Bound.ssid_max + 1, 0),
    );
}

test "the longest allowed passphrase is accepted" {
    try sta_cfg.credentialsValid(9, sta_cfg.Bound.pass_max);
}

test "a passphrase one octet past the bound is refused" {
    try std.testing.expectError(
        error.InvalidSize,
        sta_cfg.credentialsValid(9, sta_cfg.Bound.pass_max + 1),
    );
}

test "an empty ssid is refused before an over-long passphrase" {
    try std.testing.expectError(
        error.InvalidSize,
        sta_cfg.credentialsValid(0, sta_cfg.Bound.pass_max + 1),
    );
}

test "the bounds are the ones the wire allows" {
    try std.testing.expectEqual(@as(u8, 32), sta_cfg.Bound.ssid_max);
    try std.testing.expectEqual(@as(u8, 64), sta_cfg.Bound.pass_max);
}
