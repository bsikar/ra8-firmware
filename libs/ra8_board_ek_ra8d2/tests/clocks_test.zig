//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Board clock bring-up against a fake CGC. Two things worth holding down:
//! the published rates are the board's constants rather than anything the CGC
//! hands back, and a failed bring-up leaves the caller's record untouched.

const std = @import("std");
const clocks = @import("clocks");

var cgc_result: u32 = 0;
var cgc_calls: usize = 0;

export fn ra8_cgc_init() u32 {
    cgc_calls += 1;
    return cgc_result;
}

fn reset() void {
    cgc_result = 0;
    cgc_calls = 0;
}

const Rates = extern struct { cpuclk0_hz: u32, pclka_hz: u32 };

test "null output is rejected before the clock tree is touched" {
    reset();
    try std.testing.expectEqual(@as(u32, 0x103), clocks.init(null));
    try std.testing.expectEqual(@as(usize, 0), cgc_calls);
}

test "a successful bring-up publishes the board rates" {
    reset();
    var rates: Rates = .{ .cpuclk0_hz = 0, .pclka_hz = 0 };
    try std.testing.expectEqual(@as(u32, 0), clocks.init(@ptrCast(&rates)));
    try std.testing.expectEqual(@as(usize, 1), cgc_calls);
    try std.testing.expectEqual(clocks.Rates.cpuclk0_hz, rates.cpuclk0_hz);
    try std.testing.expectEqual(clocks.Rates.pclka_hz, rates.pclka_hz);
}

test "a CGC failure propagates and leaves the record untouched" {
    reset();
    cgc_result = 0x10F;
    var rates: Rates = .{ .cpuclk0_hz = 0xDEAD, .pclka_hz = 0xBEEF };
    try std.testing.expectEqual(@as(u32, 0x10F), clocks.init(@ptrCast(&rates)));
    try std.testing.expectEqual(@as(u32, 0xDEAD), rates.cpuclk0_hz);
    try std.testing.expectEqual(@as(u32, 0xBEEF), rates.pclka_hz);
}

test "the published rates are the standard PLL1 tree" {
    try std.testing.expectEqual(@as(u32, 1_000_000_000), clocks.Rates.cpuclk0_hz);
    try std.testing.expectEqual(@as(u32, 125_000_000), clocks.Rates.pclka_hz);
}
