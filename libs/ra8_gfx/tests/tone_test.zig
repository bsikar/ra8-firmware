//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the per-panel tone curve: the curve contract, the
//! bracket walk, the exact round-up threshold and the prepared quantise rule.

const std = @import("std");
const impl = @import("tone");
const gfx = impl.core;

test "tone constants match the C ra8_gfx_tone_const_t" {
    try std.testing.expectEqual(@as(u16, 16), impl.tone.knots);
    try std.testing.expectEqual(@as(u16, 15), impl.tone.last_knot);
    try std.testing.expectEqual(@as(u16, 256), impl.tone.domain);
    try std.testing.expectEqual(@as(u8, 255), impl.tone.white);
    try std.testing.expectEqual(@as(u32, 256), impl.tone.scale);
}

test "tone lut and map layouts match the C structs" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(impl.Lut));
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(impl.Map, "level"));
    try std.testing.expectEqual(@as(usize, 256), @offsetOf(impl.Map, "up_threshold"));
    try std.testing.expectEqual(@as(usize, 768), @sizeOf(impl.Map));
}

test "the nominal curve is the (n << 4) | n expansion" {
    for (impl.nominal.level_gray8, 0..) |value, n| {
        const level: u8 = @intCast(n);
        try std.testing.expectEqual(@as(u8, (level << 4) | level), value);
    }
    try std.testing.expectEqual(gfx.err.ok, impl.validateLut(&impl.nominal));
}

test "bracket finds the knot at or below the sample" {
    try std.testing.expectEqual(@as(u8, 0), impl.bracket(&impl.nominal, 0));
    try std.testing.expectEqual(@as(u8, 0), impl.bracket(&impl.nominal, 16));
    try std.testing.expectEqual(@as(u8, 1), impl.bracket(&impl.nominal, 17));
    try std.testing.expectEqual(@as(u8, 1), impl.bracket(&impl.nominal, 33));
    try std.testing.expectEqual(@as(u8, 15), impl.bracket(&impl.nominal, 255));
}

test "bracket floors at black when no knot sits below the sample" {
    var lut = impl.nominal;
    lut.level_gray8[0] = 4;
    try std.testing.expectEqual(@as(u8, 0), impl.bracket(&lut, 0));
    try std.testing.expectEqual(@as(u8, 0), impl.bracket(&lut, 3));
}

test "validate rejects the endpoints the contract pins" {
    var lut = impl.nominal;
    lut.level_gray8[0] = 1;
    try std.testing.expectEqual(gfx.err.range_check_failed, impl.validateLut(&lut));

    lut = impl.nominal;
    lut.level_gray8[impl.tone.last_knot] = 254;
    try std.testing.expectEqual(gfx.err.range_check_failed, impl.validateLut(&lut));
}

test "validate rejects a curve that is not strictly increasing" {
    var flat = impl.nominal;
    flat.level_gray8[5] = flat.level_gray8[4];
    try std.testing.expectEqual(gfx.err.range_check_failed, impl.validateLut(&flat));

    var backwards = impl.nominal;
    backwards.level_gray8[9] = backwards.level_gray8[8] - 1;
    try std.testing.expectEqual(gfx.err.range_check_failed, impl.validateLut(&backwards));
}

test "upThreshold is the ceiling of rem * 256 / span" {
    try std.testing.expectEqual(@as(u16, 0), impl.upThreshold(0, 17));
    try std.testing.expectEqual(@as(u16, 16), impl.upThreshold(1, 17));
    try std.testing.expectEqual(@as(u16, 256), impl.upThreshold(17, 17));
    try std.testing.expectEqual(@as(u16, 128), impl.upThreshold(8, 16));
}

test "prepare maps every sample to its knot and round-up threshold" {
    var map: impl.Map = undefined;
    impl.prepareMap(&impl.nominal, &map);

    try std.testing.expectEqual(@as(u8, 0), map.level[0]);
    try std.testing.expectEqual(@as(u16, 0), map.up_threshold[0]);
    try std.testing.expectEqual(@as(u8, 1), map.level[17]);
    try std.testing.expectEqual(@as(u8, 0), map.level[16]);
    try std.testing.expectEqual(@as(u16, impl.upThreshold(16, 17)), map.up_threshold[16]);
    try std.testing.expectEqual(@as(u8, 15), map.level[255]);
}

test "the white knot never rounds up" {
    var map: impl.Map = undefined;
    impl.prepareMap(&impl.nominal, &map);
    try std.testing.expectEqual(@as(u16, 0), map.up_threshold[255]);
    try std.testing.expectEqual(@as(u8, 15), impl.quantise(&map, 255, 0));
}

test "quantise takes the next level exactly while thr is below the threshold" {
    var map: impl.Map = undefined;
    impl.prepareMap(&impl.nominal, &map);

    const sample: u8 = 25;
    const threshold = map.up_threshold[sample];
    const base = map.level[sample];
    try std.testing.expectEqual(base + 1, impl.quantise(&map, sample, 0));
    try std.testing.expectEqual(base + 1, impl.quantise(&map, sample, @intCast(threshold - 1)));
    try std.testing.expectEqual(base, impl.quantise(&map, sample, @intCast(threshold)));
    try std.testing.expectEqual(base, impl.quantise(&map, sample, 255));
}

test "a knot sample never rounds up under any threshold" {
    var map: impl.Map = undefined;
    impl.prepareMap(&impl.nominal, &map);
    for (impl.nominal.level_gray8, 0..) |sample, n| {
        try std.testing.expectEqual(@as(u8, @intCast(n)), impl.quantise(&map, sample, 0));
        try std.testing.expectEqual(@as(u8, @intCast(n)), impl.quantise(&map, sample, 255));
    }
}

test "a measured curve quantises against its own uneven intervals" {
    var lut = impl.Lut{ .level_gray8 = .{ 0, 4, 9, 20, 40, 62, 85, 104, 130, 151, 170, 190, 208, 228, 243, 255 } };
    try std.testing.expectEqual(gfx.err.ok, impl.validateLut(&lut));

    var map: impl.Map = undefined;
    impl.prepareMap(&lut, &map);
    try std.testing.expectEqual(@as(u8, 3), map.level[30]);
    try std.testing.expectEqual(@as(u16, impl.upThreshold(10, 20)), map.up_threshold[30]);
    try std.testing.expectEqual(@as(u8, 4), impl.quantise(&map, 30, 127));
    try std.testing.expectEqual(@as(u8, 3), impl.quantise(&map, 30, 128));
}

test "every prepared sample stays inside the panel's level range" {
    var map: impl.Map = undefined;
    impl.prepareMap(&impl.nominal, &map);
    var v: u32 = 0;
    while (v < impl.tone.domain) : (v += 1) {
        try std.testing.expect(map.level[v] <= impl.tone.last_knot);
        try std.testing.expect(map.up_threshold[v] <= impl.tone.scale);
        const high = impl.quantise(&map, @intCast(v), 0);
        try std.testing.expect(high <= impl.tone.last_knot);
    }
}
