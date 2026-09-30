//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `ra8_imgdec` backend's own decisions, driven directly. No JPEG and no
//! codec call is needed: every branch here is a function of the geometry and
//! the caller's destination description.

const std = @import("std");
const policy = @import("policy");

test "row stride and surface bytes are packed RGB888" {
    try std.testing.expectEqual(@as(u32, 3), policy.rowStride(1));
    try std.testing.expectEqual(@as(u32, 48), policy.rowStride(16));
    try std.testing.expectEqual(@as(u32, 0), policy.rowStride(0));
    try std.testing.expectEqual(@as(u32, 768), policy.surfaceBytes(16, 16));
    try std.testing.expectEqual(@as(u32, 0), policy.surfaceBytes(16, 0));
}

test "dim_max admits the ceiling and refuses one past it" {
    const ceiling: u16 = @intCast(policy.limits.dim_max);
    try std.testing.expect(policy.withinDimMax(1, 1));
    try std.testing.expect(policy.withinDimMax(ceiling, ceiling));
    try std.testing.expect(!policy.withinDimMax(ceiling + 1, 1));
    try std.testing.expect(!policy.withinDimMax(1, ceiling + 1));
    try std.testing.expect(!policy.withinDimMax(ceiling + 1, ceiling + 1));
}

test "a zero stride means packed and asks nothing of the codec" {
    const stride = policy.rowStride(16);
    const need = policy.surfaceBytes(16, 16);
    try std.testing.expectEqual(policy.Destination.ok, policy.destination(0, need, stride, need));
    try std.testing.expectEqual(
        policy.Destination.ok,
        policy.destination(0, need + 4096, stride, need),
    );
}

test "a destination smaller than the surface is refused as too small" {
    const stride = policy.rowStride(16);
    const need = policy.surfaceBytes(16, 16);
    try std.testing.expectEqual(
        policy.Destination.too_small,
        policy.destination(0, need - 1, stride, need),
    );
    try std.testing.expectEqual(policy.Destination.too_small, policy.destination(0, 0, stride, need));
}

test "a stride under one packed row is too small, not padded" {
    const stride = policy.rowStride(16);
    const need = policy.surfaceBytes(16, 16);
    try std.testing.expectEqual(
        policy.Destination.too_small,
        policy.destination(stride - 1, need, stride, need),
    );
    try std.testing.expectEqual(
        policy.Destination.too_small,
        policy.destination(1, need, stride, need),
    );
}

test "a stride over one packed row is the padded refusal, kept distinct" {
    const stride = policy.rowStride(16);
    const need = policy.surfaceBytes(16, 16);
    try std.testing.expectEqual(
        policy.Destination.padded,
        policy.destination(stride + 1, need, stride, need),
    );
    // Padding is decided before capacity, so a huge padded destination is
    // still the padded refusal rather than an accept.
    try std.testing.expectEqual(
        policy.Destination.padded,
        policy.destination(stride * 2, need * 4, stride, need),
    );
}

test "an exact matching stride is accepted" {
    const stride = policy.rowStride(640);
    const need = policy.surfaceBytes(640, 480);
    try std.testing.expectEqual(policy.Destination.ok, policy.destination(stride, need, stride, need));
    // ... and the same stride with a short buffer is still too small.
    try std.testing.expectEqual(
        policy.Destination.too_small,
        policy.destination(stride, need - 1, stride, need),
    );
}

test "a dim_max square surface fits a 32-bit byte count" {
    const ceiling: u16 = @intCast(policy.limits.dim_max);
    const need = policy.surfaceBytes(ceiling, ceiling);
    try std.testing.expectEqual(@as(u32, 16384 * 16384 * 3), need);
    try std.testing.expectEqual(policy.Destination.ok, policy.destination(0, need, policy.rowStride(ceiling), need));
}
