//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Affine tensor quantization for the Ethos-U55 runtime (inc/ra8_npu_quant.h,
//! RA8FW-543): real = scale * (q - zero_point) and
//! q = clamp(round(real / scale) + zero_point, qmin, qmax). Pure arithmetic,
//! no registers and no libm.

const std = @import("std");

pub const i8_min: i32 = -128;
pub const i8_max: i32 = 127;
pub const u8_min: i32 = 0;
pub const u8_max: i32 = 255;

/// The C caps the rounded quotient at +/-1e6 before the integer cast.
pub const round_cap: f32 = 1.0e6;

pub const Error = error{ScaleNotPositive};

/// Round half away from zero, cap, add the zero point, clamp. A NaN rounds to
/// 0 (what the M85's VCVT gives; the C cast is undefined there) and the
/// zero-point add saturates instead of overflowing.
pub fn roundClamp(scaled: f32, zero_point: i32, qmin: i32, qmax: i32) i32 {
    var rounded = if (scaled >= 0.0) scaled + 0.5 else scaled - 0.5;
    if (rounded > round_cap) rounded = round_cap;
    if (rounded < -round_cap) rounded = -round_cap;
    const whole: i32 = if (std.math.isNan(rounded)) 0 else @intFromFloat(rounded);
    return std.math.clamp(whole +| zero_point, qmin, qmax);
}

fn bounds(comptime T: type) [2]i32 {
    return switch (T) {
        i8 => .{ i8_min, i8_max },
        u8 => .{ u8_min, u8_max },
        else => @compileError("quantize supports i8 and u8"),
    };
}

/// Quantize `in` into `out` (same length). `scale` must be > 0.
pub fn quantize(comptime T: type, in: []const f32, out: []T, scale: f32, zero_point: i32) Error!void {
    if (scale <= 0.0) return error.ScaleNotPositive;
    const range = bounds(T);
    for (in, out) |real, *q| {
        q.* = @intCast(roundClamp(real / scale, zero_point, range[0], range[1]));
    }
}

/// Dequantize `in` into `out` (same length): scale * (q - zero_point).
pub fn dequantize(comptime T: type, in: []const T, out: []f32, scale: f32, zero_point: i32) void {
    _ = bounds(T);
    for (in, out) |q, *real| {
        const centred = @as(i32, q) -% zero_point;
        real.* = scale * @as(f32, @floatFromInt(centred));
    }
}
