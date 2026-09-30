//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `inc/ra8_num.h`.
//!
//! The header declares exactly one function, so this file exports exactly one
//! symbol. Its whole job is the shape change: a Zig error union becomes the
//! header's `bool`, and every refusal leaves `out` untouched, because a caller
//! that ignored the return value must not find a number it did not earn.

const decimal = @import("internal/decimal.zig");

/// See `ra8_num_decimal_to_binary64` in `inc/ra8_num.h`.
pub export fn ra8_num_decimal_to_binary64(
    mantissa: u64,
    decimal_scale: i32,
    negative: bool,
    out: ?*f64,
) callconv(.c) bool {
    // The null check comes first and on its own: a null destination is a
    // refusal regardless of how well-formed the value is.
    const destination = out orelse return false;
    const value = decimal.toBinary64(mantissa, decimal_scale, negative) catch return false;
    destination.* = value;
    return true;
}
