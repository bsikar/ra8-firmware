//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the SysTick reload arithmetic.
//!
//! Only the arithmetic is covered here. The register writes are MMIO against
//! the System Control Space, and the project already drives those from
//! `tests/hal/src/test_ra8_systick.c` through the fake MMIO map, which is a
//! better place for them than a Zig test that would have to fake the window
//! itself.

const std = @import("std");
const reload = @import("systick_reload");

test "reloadFor computes clock-driven reloads" {
    try std.testing.expectEqual(@as(u32, 999_999), try reload.reloadFor(1_000_000_000, 1000));
    try std.testing.expectEqual(@as(u32, 239_999), try reload.reloadFor(240_000_000, 1000));
    try std.testing.expectEqual(@as(u32, 0), try reload.reloadFor(1000, 1000));
}

test "reloadFor rejects a zero core clock" {
    try std.testing.expectError(error.ZeroRate, reload.reloadFor(0, 1000));
}

test "reloadFor rejects a zero tick rate" {
    try std.testing.expectError(error.ZeroRate, reload.reloadFor(1_000_000, 0));
}

test "reloadFor rejects both rates zero" {
    try std.testing.expectError(error.ZeroRate, reload.reloadFor(0, 0));
}

test "reloadFor rejects a core slower than one tick period" {
    try std.testing.expectError(error.ClockBelowTick, reload.reloadFor(999, 1000));
    try std.testing.expectError(error.ClockBelowTick, reload.reloadFor(1, 1000));
}

test "reloadFor rejects a reload wider than the 24-bit field" {
    // 0x1000000 ticks -> reload 0x00FFFFFF, the largest that fits.
    try std.testing.expectEqual(reload.limits.rvr_max, try reload.reloadFor(0x100_0000, 1));
    // One tick more overflows the field.
    try std.testing.expectError(error.OutOfRange, reload.reloadFor(0x100_0001, 1));
}

test "fits tracks the 24-bit field bound" {
    try std.testing.expect(reload.fits(0));
    try std.testing.expect(reload.fits(reload.limits.rvr_max));
    try std.testing.expect(!reload.fits(reload.limits.rvr_max + 1));
    try std.testing.expect(!reload.fits(0xFFFF_FFFF));
}

test "rvr_max is the 24-bit mask" {
    try std.testing.expectEqual(@as(u32, 0x00FF_FFFF), reload.limits.rvr_max);
}
