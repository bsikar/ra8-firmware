//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exact decimal to binary64, correctly rounded to nearest with ties to even.
//!
//! The value `mantissa * 10^scale` is held as a rational with the even factor
//! taken out: 10^scale becomes 5^scale in the numerator or denominator, and
//! the matching 2^scale stays an explicit shift. Long division then emits at
//! most 53 significant bits and the remainder decides the rounding, so no
//! floating-point arithmetic happens anywhere on the way.

const std = @import("std");
const Big = @import("big.zig");
const binary64 = @import("binary64.zig");

/// Bounds the conversion accepts, mirroring the public header.
pub const bounds = struct {
    pub const scale_max = 400;
    pub const digits_max = 17;
    /// Odd factor of the decimal radix; the even factor is the explicit shift.
    pub const decimal_prime = 5;
    /// Width the bounded quotient may occupy.
    pub const quotient_bits = 64;
};

pub const Refusal = error{
    /// The scale is outside the accepted range.
    ScaleOutOfRange,
    /// An intermediate outgrew the fixed capacity.
    Capacity,
    /// The exact value overflows or underflows binary64.
    OutOfRange,
};

/// The rational `mantissa * 5^scale`, with `2^scale` still to be applied.
const Rational = struct {
    numerator: Big,
    denominator: Big,

    fn build(mantissa: u64, scale: i32) error{Capacity}!Rational {
        var self = Rational{
            .numerator = Big.fromU64(mantissa),
            .denominator = Big.fromU64(1),
        };
        const count: u32 = @abs(scale);
        const factor = if (scale < 0) &self.denominator else &self.numerator;
        for (0..count) |_| try factor.mulSmall(bounds.decimal_prime);
        return self;
    }

    /// Order this rational, scaled by `2^binary_scale`, against `2^exponent`.
    ///
    /// A shift too wide for the fixed capacity is not an ambiguity: the side
    /// that could not be built is the larger one, so the ordering is forced.
    fn orderAgainstPower(self: *const Rational, binary_scale: i32, exponent: i32) std.math.Order {
        const shift = binary_scale - exponent;
        if (shift >= 0) {
            const shifted = self.numerator.shiftLeft(@intCast(shift)) catch return .gt;
            return shifted.order(&self.denominator);
        }
        const shifted = self.denominator.shiftLeft(@intCast(-shift)) catch return .lt;
        return self.numerator.order(&shifted);
    }

    /// Divide, shifted by `binary_shift`, rounding to nearest with ties to even.
    fn divideRounded(self: *const Rational, binary_shift: i32) Refusal!u64 {
        var work: Big = undefined;
        var divisor: Big = undefined;
        if (binary_shift >= 0) {
            work = self.numerator.shiftLeft(@intCast(binary_shift)) catch return Refusal.Capacity;
            divisor = self.denominator;
        } else {
            work = self.numerator;
            divisor = self.denominator.shiftLeft(@intCast(-binary_shift)) catch return Refusal.Capacity;
        }

        const top = @as(i32, work.bitLength()) - @as(i32, divisor.bitLength());
        if (top >= bounds.quotient_bits) return Refusal.Capacity;

        var quotient: u64 = 0;
        var bit = top;
        while (bit >= 0) : (bit -= 1) {
            const trial = divisor.shiftLeft(@intCast(bit)) catch return Refusal.Capacity;
            if (work.order(&trial) != .lt) {
                work.subtract(&trial);
                quotient |= @as(u64, 1) << @intCast(bit);
            }
        }

        // Twice the remainder against the divisor is the round-to-nearest
        // test; equality is the tie, broken towards an even quotient.
        const twice = work.shiftLeft(1) catch return Refusal.Capacity;
        const comparison = twice.order(&divisor);
        if (comparison == .gt or (comparison == .eq and (quotient & 1) != 0)) quotient += 1;
        return quotient;
    }
};

/// The binary64 nearest to `mantissa * 10^scale`, ties to even.
pub fn toBinary64(mantissa: u64, scale: i32, negative: bool) Refusal!f64 {
    if (scale < -bounds.scale_max or scale > bounds.scale_max) return Refusal.ScaleOutOfRange;
    if (mantissa == 0) return binary64.signedZero(negative);

    const rational = Rational.build(mantissa, scale) catch return Refusal.Capacity;

    // The bit lengths give the exponent to within one; the comparison against
    // 2^exponent settles which side of the power of two the value sits on.
    var exponent = @as(i32, rational.numerator.bitLength()) -
        @as(i32, rational.denominator.bitLength()) + scale;
    if (rational.orderAgainstPower(scale, exponent) == .lt) exponent -= 1;
    if (exponent > binary64.format.exponent_max) return Refusal.OutOfRange;

    const class: binary64.Class =
        if (exponent >= binary64.format.exponent_min) .normal else .subnormal;
    const shift = switch (class) {
        .normal => scale + binary64.format.fraction_bits - exponent,
        .subnormal => scale + binary64.format.subnormal_scale,
    };

    const quotient = try rational.divideRounded(shift);
    return binary64.encode(quotient, exponent, negative, class) catch Refusal.OutOfRange;
}
