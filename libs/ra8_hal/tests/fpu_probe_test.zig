//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Value tests for ra8_fpu_dp_madd, ported from the unwired C test
//! tests/misc/src/test_ra8_fpu_probe.c. The ARM lowering is checked
//! separately by tests/fpu_lowering_check.zig.

const std = @import("std");
const probe = @import("fpu_probe");

const madd = probe.ra8_fpu_dp_madd;

test "ra8_fpu_dp_madd computes a*b+c in double precision" {
    try std.testing.expectEqual(@as(f64, 7.0), madd(2.0, 3.0, 1.0));
    try std.testing.expectEqual(@as(f64, -5.5), madd(-1.5, 4.0, 0.5));
    try std.testing.expectEqual(@as(f64, 0.5), madd(0.5, 0.5, 0.25));
    try std.testing.expectEqual(@as(f64, 42.0), madd(6.0, 7.0, 0.0));
}

test "ra8_fpu_dp_madd preserves binary64 magnitude" {
    // 2^40 * 2 + 1 is exact in binary64 and not in binary32.
    try std.testing.expectEqual(@as(f64, 2199023255553.0), madd(1099511627776.0, 2.0, 1.0));
}
