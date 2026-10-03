//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const npu_quant = @import("npu_quant");

test "roundClamp rounds half away from zero and adds the zero point" {
    try std.testing.expectEqual(@as(i32, 3), npu_quant.roundClamp(2.5, 0, -128, 127));
    try std.testing.expectEqual(@as(i32, -3), npu_quant.roundClamp(-2.5, 0, -128, 127));
    try std.testing.expectEqual(@as(i32, 2), npu_quant.roundClamp(2.49, 0, -128, 127));
    try std.testing.expectEqual(@as(i32, 13), npu_quant.roundClamp(2.6, 10, -128, 127));
}

test "roundClamp clamps to the range and caps huge or infinite quotients" {
    try std.testing.expectEqual(@as(i32, 127), npu_quant.roundClamp(1000.0, 0, -128, 127));
    try std.testing.expectEqual(@as(i32, -128), npu_quant.roundClamp(-1000.0, 0, -128, 127));
    try std.testing.expectEqual(@as(i32, 0), npu_quant.roundClamp(std.math.inf(f32), -1_000_000, -128, 127));
    try std.testing.expectEqual(@as(i32, 255), npu_quant.roundClamp(-std.math.inf(f32), 1_000_255, 0, 255));
}

test "roundClamp maps NaN to the zero point and saturates the zero-point add" {
    try std.testing.expectEqual(@as(i32, 7), npu_quant.roundClamp(std.math.nan(f32), 7, -128, 127));
    try std.testing.expectEqual(@as(i32, 255), npu_quant.roundClamp(1.0e6, std.math.maxInt(i32), 0, 255));
}

test "quantize i8 divides by scale and saturates" {
    const in = [_]f32{ 0.0, 0.25, -0.25, 100.0, -100.0 };
    var out: [5]i8 = undefined;
    try npu_quant.quantize(i8, &in, &out, 0.5, 1);
    try std.testing.expectEqualSlices(i8, &.{ 1, 2, 0, 127, -128 }, &out);
}

test "quantize u8 clamps at 0 and 255" {
    const in = [_]f32{ -5.0, 1.0, 300.0 };
    var out: [3]u8 = undefined;
    try npu_quant.quantize(u8, &in, &out, 1.0, 128);
    try std.testing.expectEqualSlices(u8, &.{ 123, 129, 255 }, &out);
}

test "quantize refuses a scale that is zero or negative and leaves out alone" {
    const in = [_]f32{1.0};
    var out = [_]i8{42};
    try std.testing.expectError(error.ScaleNotPositive, npu_quant.quantize(i8, &in, &out, 0.0, 0));
    try std.testing.expectError(error.ScaleNotPositive, npu_quant.quantize(i8, &in, &out, -1.0, 0));
    try std.testing.expectEqual(@as(i8, 42), out[0]);
}

test "dequantize is scale * (q - zero_point) for i8 and u8" {
    var real: [3]f32 = undefined;
    npu_quant.dequantize(i8, &.{ -128, 0, 127 }, &real, 0.5, -1);
    try std.testing.expectEqualSlices(f32, &.{ -63.5, 0.5, 64.0 }, &real);
    npu_quant.dequantize(u8, &.{ 0, 128, 255 }, &real, 2.0, 128);
    try std.testing.expectEqualSlices(f32, &.{ -256.0, 0.0, 254.0 }, &real);
}

test "quantize then dequantize round-trips on the grid" {
    const in = [_]f32{ -1.5, -0.5, 0.0, 0.5, 1.5 };
    var q: [5]i8 = undefined;
    var back: [5]f32 = undefined;
    try npu_quant.quantize(i8, &in, &q, 0.5, 3);
    npu_quant.dequantize(i8, &q, &back, 0.5, 3);
    try std.testing.expectEqualSlices(f32, &in, &back);
}
