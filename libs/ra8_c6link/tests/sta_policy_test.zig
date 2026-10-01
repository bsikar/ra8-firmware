//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the station association policy.

const std = @import("std");
const implementation = @import("implementation");
const sta_policy = implementation.sta_policy;
const field_copy = implementation.field_copy;

const Wire = sta_policy.Wire;

test "the interface selector is WIFI_IF_STA" {
    try std.testing.expectEqual(@as(i32, 0), Wire.iface_sta);
}

test "the scan selectors are the ones the bench network wants" {
    try std.testing.expectEqual(@as(i32, 0), Wire.scan_fast);
    try std.testing.expectEqual(@as(i32, 0), Wire.sort_signal);
}

test "the auth threshold imposes no minimum" {
    try std.testing.expectEqual(@as(i32, 0), Wire.auth_open);
}

test "protected management frames are offered, not demanded" {
    try std.testing.expectEqual(@as(i32, 1), Wire.pmf_capable);
}

test "the policy carries every selector the request needs" {
    const p = sta_policy.policy();
    try std.testing.expectEqual(Wire.iface_sta, p.iface);
    try std.testing.expectEqual(Wire.scan_fast, p.scan_method);
    try std.testing.expectEqual(Wire.sort_signal, p.sort_method);
    try std.testing.expectEqual(Wire.auth_open, p.auth_threshold);
    try std.testing.expectEqual(Wire.pmf_capable, p.pmf_capable);
}

test "the policy is the same object every time it is asked for" {
    try std.testing.expectEqual(sta_policy.policy(), sta_policy.policy());
}

test "the shape C reads has no interior padding" {
    const P = sta_policy.Policy;
    try std.testing.expectEqual(@as(usize, 4), @alignOf(P));
    try std.testing.expectEqual(@as(usize, 20), @sizeOf(P));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(P, "iface"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(P, "scan_method"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(P, "sort_method"));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(P, "auth_threshold"));
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(P, "pmf_capable"));
}

test "a pinned BSSID travels at its full length" {
    try std.testing.expectEqual(field_copy.Bound.mac_octets, sta_policy.bssidLen(true));
}

test "an unpinned BSSID travels as an absent field" {
    try std.testing.expectEqual(@as(usize, 0), sta_policy.bssidLen(false));
}

test "the pinned length is the address length, not a restated six" {
    try std.testing.expectEqual(@as(usize, 6), field_copy.Bound.mac_octets);
    try std.testing.expectEqual(field_copy.Bound.mac_octets, sta_policy.bssidLen(true));
}

test "pinning is the only thing that puts octets on the wire" {
    try std.testing.expect(sta_policy.bssidLen(true) > sta_policy.bssidLen(false));
}

test "the four wire selectors agree today and are still four decisions" {
    // Every one of these is zero in a different ESP-IDF enumeration. The test
    // exists so that a renumbering on the far side shows up here rather than
    // as a station that associates with the wrong interface or scan order.
    const all = [_]i32{ Wire.iface_sta, Wire.scan_fast, Wire.sort_signal, Wire.auth_open };
    for (all) |v| try std.testing.expectEqual(@as(i32, 0), v);
}
