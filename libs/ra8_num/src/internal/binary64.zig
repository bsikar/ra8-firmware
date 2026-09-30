//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! IEC 60559 binary64 bit format: the parameters the conversion is defined
//! against, and the one step that turns a rounded significand and an exponent
//! into a finite value.
//!
//! Nothing here does floating-point arithmetic. The result is assembled from
//! its bit pattern, exactly as the C implementation did, so the rounding
//! decided upstream is the only rounding that happens.

const std = @import("std");

/// Format parameters, all of them derived from the same 53-bit precision.
pub const format = struct {
    pub const precision_bits = 53;
    pub const fraction_bits = 52;
    pub const sign_shift = 63;
    pub const exponent_bias = 1023;
    pub const exponent_max = 1023;
    pub const exponent_min = -1022;
    /// Scale that selects subnormal units of 2^-1074.
    pub const subnormal_scale = 1074;

    pub const hidden_bit: u64 = 1 << fraction_bits;
    pub const carry_bit: u64 = 1 << precision_bits;
};

comptime {
    std.debug.assert(@bitSizeOf(f64) == 64);
    std.debug.assert(std.math.floatMantissaBits(f64) == format.fraction_bits);
}

pub const Range = error{OutOfRange};

/// Which side of the normal/subnormal boundary a value landed on.
pub const Class = enum { normal, subnormal };

pub fn signedZero(negative: bool) f64 {
    return @bitCast(signBit(negative));
}

fn signBit(negative: bool) u64 {
    return if (negative) @as(u64, 1) << format.sign_shift else 0;
}

/// Assemble a finite binary64 from a rounded significand.
///
/// `significand` is the quotient the division produced and may carry out of
/// 53 bits when rounding rounded up; that carry is normalized here, once, and
/// can push a value that was in range over `exponent_max`.
pub fn encode(significand: u64, exponent: i32, negative: bool, class: Class) Range!f64 {
    var bits = signBit(negative);
    switch (class) {
        .normal => {
            var value = significand;
            var power = exponent;
            if (value == format.carry_bit) {
                value >>= 1;
                power += 1;
            }
            if (power > format.exponent_max or value < format.hidden_bit) return Range.OutOfRange;
            bits |= @as(u64, @intCast(power + format.exponent_bias)) << format.fraction_bits;
            bits |= value - format.hidden_bit;
        },
        .subnormal => {
            if (significand == 0 or significand > format.hidden_bit) return Range.OutOfRange;
            // A subnormal that rounded all the way up to the hidden bit is the
            // smallest normal, and its fraction field is zero either way.
            bits |= significand;
        },
    }
    return @bitCast(bits);
}
