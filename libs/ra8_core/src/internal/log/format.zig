//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Decimal formatting for the log backend.
//!
//! The C pushed digits straight at the ITM port from a scratch buffer. Here
//! the digits land in a caller buffer and come back as a slice, so the
//! arithmetic is testable without a transport and the caller decides where
//! the bytes go.

/// Buffer sizes callers need, from the widest value of each type.
pub const limits = struct {
    /// `4294967295` is 10 digits.
    pub const u32_digits: usize = 10;
    /// `-2147483648` is 11 characters.
    pub const i32_chars: usize = 11;
};

const decimal_base: u32 = 10;

/// Write `value` as unsigned decimal into `buf`, returning the written slice.
pub fn unsigned(buf: *[limits.u32_digits]u8, value: u32) []const u8 {
    if (value == 0) {
        buf[0] = '0';
        return buf[0..1];
    }

    // Digits fall out least-significant first, so fill from the back.
    var rest = value;
    var first = buf.len;
    while (rest != 0) {
        first -= 1;
        buf[first] = '0' + @as(u8, @intCast(rest % decimal_base));
        rest /= decimal_base;
    }
    return buf[first..];
}

/// Write `value` as signed decimal into `buf`, returning the written slice.
pub fn signed(buf: *[limits.i32_chars]u8, value: i32) []const u8 {
    if (value >= 0) {
        var digits: [limits.u32_digits]u8 = undefined;
        const written = unsigned(&digits, @intCast(value));
        @memcpy(buf[0..written.len], written);
        return buf[0..written.len];
    }

    buf[0] = '-';
    var digits: [limits.u32_digits]u8 = undefined;
    // Negating i32's most negative value overflows it, so widen first. This
    // is the same reason the C went through int64_t here.
    const magnitude: u32 = @intCast(-@as(i64, value));
    const written = unsigned(&digits, magnitude);
    @memcpy(buf[1 .. 1 + written.len], written);
    return buf[0 .. 1 + written.len];
}
