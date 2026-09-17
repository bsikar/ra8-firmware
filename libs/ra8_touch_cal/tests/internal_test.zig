//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure calibration math: the least-squares fit, the
//! Cramer solve, the pixel mapping and the `'TCAL'` storage codec. The C suite
//! in `tests/graphics/` remains the behavioural contract and runs unchanged
//! against this archive; these cases reach the internals it can only observe
//! through the public API.

const std = @import("std");
const implementation = @import("implementation");

const Point = implementation.Point;
const Matrix = implementation.Matrix;

/// Map a raw sample through a known-good transform, the way the C suite's
/// synthetic ground truth does.
fn truthMap(raw: Point, m: Matrix) Point {
    const xf: f32 = @floatFromInt(raw.x);
    const yf: f32 = @floatFromInt(raw.y);
    const u = (m.a * xf) + (m.b * yf) + m.c;
    const v = (m.d * xf) + (m.e * yf) + m.f;
    return .{
        .x = @intFromFloat(u + implementation.round_bias),
        .y = @intFromFloat(v + implementation.round_bias),
    };
}

test "crc32 matches the IEEE 802.3 check vector" {
    try std.testing.expectEqual(@as(u32, 0xCBF43926), implementation.crc32("123456789"));
    try std.testing.expectEqual(@as(u32, 0), implementation.crc32(""));
}

test "crc32 changes when any single byte flips" {
    var data = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
    const base = implementation.crc32(&data);
    for (&data) |*byte| {
        byte.* ^= 0xFF;
        try std.testing.expect(implementation.crc32(&data) != base);
        byte.* ^= 0xFF;
    }
    try std.testing.expectEqual(base, implementation.crc32(&data));
}

test "little-endian packers round-trip" {
    var buf: [4]u8 = @splat(0);
    implementation.packLe32(&buf, 0x12345678);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0x78, 0x56, 0x34, 0x12 }, &buf);
    try std.testing.expectEqual(@as(u32, 0x12345678), implementation.unpackLe32(&buf));
}

test "accumulateSums totals every term" {
    const raw = [_]Point{ .{ .x = 1, .y = 2 }, .{ .x = 3, .y = 4 } };
    const screen = [_]Point{ .{ .x = 10, .y = 20 }, .{ .x = 30, .y = 40 } };
    const s = implementation.accumulateSums(&raw, &screen);
    try std.testing.expectEqual(@as(f32, 4), s.sx);
    try std.testing.expectEqual(@as(f32, 6), s.sy);
    try std.testing.expectEqual(@as(f32, 10), s.sxx);
    try std.testing.expectEqual(@as(f32, 20), s.syy);
    try std.testing.expectEqual(@as(f32, 14), s.sxy);
    try std.testing.expectEqual(@as(f32, 40), s.su);
    try std.testing.expectEqual(@as(f32, 60), s.sv);
    try std.testing.expectEqual(@as(f32, 100), s.sxu);
    try std.testing.expectEqual(@as(f32, 140), s.syu);
    try std.testing.expectEqual(@as(f32, 140), s.sxv);
    try std.testing.expectEqual(@as(f32, 200), s.syv);
}

test "accumulateSums is bounded by the shorter span" {
    const raw = [_]Point{ .{ .x = 1, .y = 1 }, .{ .x = 5, .y = 5 } };
    const screen = [_]Point{.{ .x = 2, .y = 2 }};
    const s = implementation.accumulateSums(&raw, &screen);
    try std.testing.expectEqual(@as(f32, 1), s.sx);
}

test "det3 of the identity is one" {
    const identity = [9]f32{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    try std.testing.expectEqual(@as(f32, 1), implementation.det3(identity));
}

test "solve3 recovers a known solution" {
    const identity = [9]f32{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const solution = implementation.solve3(identity, .{ 3, -4, 5 }).?;
    try std.testing.expectEqual(@as(f32, 3), solution[0]);
    try std.testing.expectEqual(@as(f32, -4), solution[1]);
    try std.testing.expectEqual(@as(f32, 5), solution[2]);
}

test "solve3 refuses a singular system" {
    const singular = [9]f32{ 1, 2, 3, 2, 4, 6, 3, 6, 9 };
    try std.testing.expect(implementation.solve3(singular, .{ 1, 2, 3 }) == null);
}

test "solve3 refuses a determinant just under the floor and accepts one just over" {
    const under = [9]f32{ implementation.min_det * 0.5, 0, 0, 0, 1, 0, 0, 0, 1 };
    try std.testing.expect(implementation.solve3(under, .{ 1, 1, 1 }) == null);
    const over = [9]f32{ implementation.min_det * 2.0, 0, 0, 0, 1, 0, 0, 0, 1 };
    try std.testing.expect(implementation.solve3(over, .{ 1, 1, 1 }) != null);
}

test "solve3 handles a negative determinant" {
    const flipped = [9]f32{ 0, 1, 0, 1, 0, 0, 0, 0, 1 };
    try std.testing.expectEqual(@as(f32, -1), implementation.det3(flipped));
    try std.testing.expect(implementation.solve3(flipped, .{ 2, 3, 4 }) != null);
}

test "compute rejects a sample count outside the accepted range" {
    const pts = [_]Point{.{}} ** 6;
    try std.testing.expectError(error.SampleCountOutOfRange, implementation.compute(&pts, &pts, 2));
    try std.testing.expectError(error.SampleCountOutOfRange, implementation.compute(&pts, &pts, 6));
    try std.testing.expectError(error.SampleCountOutOfRange, implementation.compute(&pts, &pts, 0));
}

test "compute rejects collinear targets as singular" {
    const raw = [_]Point{ .{ .x = 0, .y = 0 }, .{ .x = 100, .y = 100 }, .{ .x = 200, .y = 200 } };
    const screen = [_]Point{ .{ .x = 0, .y = 0 }, .{ .x = 10, .y = 10 }, .{ .x = 20, .y = 20 } };
    try std.testing.expectError(error.SingularSystem, implementation.compute(&raw, &screen, 3));
}

test "compute recovers a three-point exact fit" {
    const truth: Matrix = .{ .a = 0.25, .b = 0, .c = -25, .d = 0, .e = 0.20, .f = -10 };
    const raw = [_]Point{
        .{ .x = 100, .y = 100 },
        .{ .x = 3900, .y = 100 },
        .{ .x = 2000, .y = 3900 },
    };
    var screen: [3]Point = @splat(.{});
    for (raw, 0..) |point, i| screen[i] = truthMap(point, truth);

    const fitted = try implementation.compute(&raw, &screen, 3);
    try std.testing.expectApproxEqAbs(truth.a, fitted.a, 1.0e-3);
    try std.testing.expectApproxEqAbs(truth.e, fitted.e, 1.0e-3);
    try std.testing.expectApproxEqAbs(truth.c, fitted.c, 1.0e-1);
    try std.testing.expectApproxEqAbs(truth.f, fitted.f, 1.0e-1);
}

test "compute recovers a five-point least-squares fit that round-trips" {
    const truth: Matrix = .{ .a = 0.25, .b = 0.005, .c = 10, .d = 0.005, .e = 0.14, .f = 20 };
    const raw = [_]Point{
        .{ .x = 100, .y = 100 },
        .{ .x = 3800, .y = 200 },
        .{ .x = 3700, .y = 3700 },
        .{ .x = 300, .y = 3650 },
        .{ .x = 2000, .y = 1800 },
    };
    var screen: [5]Point = @splat(.{});
    for (raw, 0..) |point, i| screen[i] = truthMap(point, truth);

    const fitted = try implementation.compute(&raw, &screen, 5);
    for (raw, 0..) |point, i| {
        const mapped = implementation.applyMatrix(point, fitted, 1024, 600);
        try std.testing.expect(@abs(mapped.x - screen[i].x) <= 5);
        try std.testing.expect(@abs(mapped.y - screen[i].y) <= 5);
    }
}

test "clip32 holds the bounds" {
    try std.testing.expectEqual(@as(i32, 0), implementation.clip32(-5, 0, 99));
    try std.testing.expectEqual(@as(i32, 99), implementation.clip32(1000, 0, 99));
    try std.testing.expectEqual(@as(i32, 42), implementation.clip32(42, 0, 99));
    try std.testing.expectEqual(@as(i32, 0), implementation.clip32(0, 0, 99));
    try std.testing.expectEqual(@as(i32, 99), implementation.clip32(99, 0, 99));
}

test "roundToPixel rounds away from zero" {
    try std.testing.expectEqual(@as(i32, 1), implementation.roundToPixel(0.5));
    try std.testing.expectEqual(@as(i32, 0), implementation.roundToPixel(0.49));
    try std.testing.expectEqual(@as(i32, -1), implementation.roundToPixel(-0.5));
    try std.testing.expectEqual(@as(i32, 0), implementation.roundToPixel(-0.49));
    try std.testing.expectEqual(@as(i32, 3), implementation.roundToPixel(2.6));
}

test "roundToPixel saturates rather than overflowing" {
    try std.testing.expectEqual(std.math.maxInt(i32), implementation.roundToPixel(1.0e30));
    try std.testing.expectEqual(std.math.minInt(i32), implementation.roundToPixel(-1.0e30));
}

test "applyMatrix clips an identity transform to the panel" {
    const identity: Matrix = .{ .a = 1, .b = 0, .c = 0, .d = 0, .e = 1, .f = 0 };
    const low = implementation.applyMatrix(.{ .x = -50, .y = -50 }, identity, 100, 100);
    try std.testing.expectEqual(@as(i32, 0), low.x);
    try std.testing.expectEqual(@as(i32, 0), low.y);
    const high = implementation.applyMatrix(.{ .x = 9999, .y = 9999 }, identity, 100, 100);
    try std.testing.expectEqual(@as(i32, 99), high.x);
    try std.testing.expectEqual(@as(i32, 99), high.y);
    const mid = implementation.applyMatrix(.{ .x = 42, .y = 7 }, identity, 100, 100);
    try std.testing.expectEqual(@as(i32, 42), mid.x);
    try std.testing.expectEqual(@as(i32, 7), mid.y);
}

test "applyMatrix carries the cross terms" {
    const skewed: Matrix = .{ .a = 1, .b = 2, .c = 3, .d = 4, .e = 5, .f = 6 };
    const mapped = implementation.applyMatrix(.{ .x = 10, .y = 20 }, skewed, 1000, 1000);
    try std.testing.expectEqual(@as(i32, 53), mapped.x);
    try std.testing.expectEqual(@as(i32, 146), mapped.y);
}

test "targetsFor paints the corners then the centre" {
    const targets = implementation.targetsFor(1024, 600, 32);
    try std.testing.expectEqual(@as(usize, 5), targets.len);
    try std.testing.expectEqual(Point{ .x = 32, .y = 32 }, targets[0]);
    try std.testing.expectEqual(Point{ .x = 991, .y = 32 }, targets[1]);
    try std.testing.expectEqual(Point{ .x = 991, .y = 567 }, targets[2]);
    try std.testing.expectEqual(Point{ .x = 32, .y = 567 }, targets[3]);
    try std.testing.expectEqual(Point{ .x = 512, .y = 300 }, targets[4]);
}

test "insetFits rejects an inset that collapses either axis" {
    try std.testing.expect(implementation.insetFits(200, 200, 10));
    try std.testing.expect(!implementation.insetFits(100, 100, 60));
    try std.testing.expect(!implementation.insetFits(200, 100, 60));
    try std.testing.expect(!implementation.insetFits(100, 200, 60));
    // Exactly half the panel leaves the two targets on top of each other.
    try std.testing.expect(!implementation.insetFits(100, 100, 50));
    try std.testing.expect(implementation.insetFits(100, 100, 49));
}

test "coefficient helpers round-trip a matrix" {
    const m: Matrix = .{ .a = 1, .b = 2, .c = 3, .d = 4, .e = 5, .f = 6 };
    const coeffs = implementation.coeffsOf(m);
    try std.testing.expectEqualSlices(f32, &[_]f32{ 1, 2, 3, 4, 5, 6 }, &coeffs);
    try std.testing.expectEqual(m, implementation.matrixFrom(coeffs));
}

test "serialize writes the documented header and a checked trailer" {
    const m: Matrix = .{ .a = 0.25, .b = 0.001, .c = 5.5, .d = -0.01, .e = 0.19, .f = -3.25 };
    var blob: [implementation.blob_size]u8 = @splat(0xAA);
    try implementation.serialize(m, &blob);

    try std.testing.expectEqualSlices(u8, "TCAL", blob[0..4]);
    try std.testing.expectEqual(implementation.storage_version, blob[4]);
    try std.testing.expectEqualSlices(u8, &[_]u8{ 0, 0, 0 }, blob[5..8]);
    const trailer = implementation.unpackLe32(blob[implementation.offset.crc32..][0..4]);
    try std.testing.expectEqual(implementation.crc32(blob[0..implementation.offset.crc32]), trailer);
}

test "serialize refuses a short destination" {
    var small: [implementation.blob_size - 1]u8 = @splat(0);
    try std.testing.expectError(error.ShortBuffer, implementation.serialize(.{}, &small));
}

test "serialize then deserialize is bit-identical" {
    const m: Matrix = .{ .a = 0.25, .b = 0.001, .c = 5.5, .d = -0.01, .e = 0.19, .f = -3.25 };
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(m, &blob);
    const decoded = try implementation.deserialize(&blob);
    try std.testing.expectEqual(m, decoded);
}

test "deserialize refuses a short source" {
    var small: [implementation.blob_size - 1]u8 = @splat(0);
    try std.testing.expectError(error.ShortBuffer, implementation.deserialize(&small));
}

test "deserialize rejects every magic byte independently" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .a = 1, .e = 1 }, &blob);
    for (0..implementation.magic.len) |i| {
        var bad = blob;
        bad[implementation.offset.magic + i] ^= 0xFF;
        try std.testing.expectError(error.BadHeader, implementation.deserialize(&bad));
    }
}

test "deserialize rejects a wrong version and every non-zero reserved byte" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .a = 1, .e = 1 }, &blob);

    var bad_version = blob;
    bad_version[implementation.offset.version] = implementation.storage_version + 1;
    try std.testing.expectError(error.BadHeader, implementation.deserialize(&bad_version));

    for (0..implementation.reserved_len) |i| {
        var bad = blob;
        bad[implementation.offset.reserved + i] = 0x42;
        try std.testing.expectError(error.BadHeader, implementation.deserialize(&bad));
    }
}

test "deserialize rejects a damaged coefficient as a CRC mismatch" {
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(.{ .a = 1, .e = 1 }, &blob);
    var bad = blob;
    bad[implementation.offset.coeffs] ^= 0xFF;
    try std.testing.expectError(error.CrcMismatch, implementation.deserialize(&bad));
}

test "a header check precedes the checksum check" {
    // A blob with both a bad magic and a bad CRC reports the header, because a
    // blank flash page must read as 'never provisioned' rather than 'corrupt'.
    var blob: [implementation.blob_size]u8 = @splat(0);
    try std.testing.expectError(error.BadHeader, implementation.deserialize(&blob));
}

test "a full calibration survives a storage round-trip" {
    const truth: Matrix = .{ .a = 0.25, .b = 0, .c = 0, .d = 0, .e = 0.20, .f = 0 };
    const targets = implementation.targetsFor(1024, 600, 32);
    var raw: [implementation.n_targets]Point = @splat(.{});
    for (targets, 0..) |target, i| {
        raw[i] = .{
            .x = @intFromFloat(@as(f32, @floatFromInt(target.x)) / truth.a),
            .y = @intFromFloat(@as(f32, @floatFromInt(target.y)) / truth.e),
        };
    }

    const fitted = try implementation.compute(&raw, &targets, implementation.n_targets);
    var blob: [implementation.blob_size]u8 = @splat(0);
    try implementation.serialize(fitted, &blob);
    const reloaded = try implementation.deserialize(&blob);

    for (raw, 0..) |point, i| {
        const mapped = implementation.applyMatrix(point, reloaded, 1024, 600);
        try std.testing.expect(@abs(mapped.x - targets[i].x) <= 5);
        try std.testing.expect(@abs(mapped.y - targets[i].y) <= 5);
    }
}
