//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ASCII byte helpers the URL and address parsers share. Everything here folds
//! ASCII only, so no locale can reach the policy.

/// Byte at `i`, or 0 past the end.
///
/// The C read these strings through a NUL terminator, so one past the last
/// byte reads as 0 and the parsers lean on that. Slices carry their length
/// instead, and this keeps the same reading without a terminator.
pub fn byteAt(s: []const u8, i: usize) u8 {
    return if (i < s.len) s[i] else 0;
}

/// Fold one ASCII byte to lower case, leaving every other byte alone.
pub fn lower(c: u8) u8 {
    return if ((c >= 'A') and (c <= 'Z')) c + ('a' - 'A') else c;
}

/// Value of one hexadecimal digit in either case, or null when `c` is not one.
pub fn hexValue(c: u8) ?u8 {
    if ((c >= '0') and (c <= '9')) return c - '0';
    if ((c >= 'a') and (c <= 'f')) return (c - 'a') + 10;
    if ((c >= 'A') and (c <= 'F')) return (c - 'A') + 10;
    return null;
}

/// Whether `s` begins with `prefix`, ignoring ASCII case.
pub fn startsWithCi(s: []const u8, prefix: []const u8) bool {
    if (s.len < prefix.len) return false;
    for (prefix, 0..) |want, i| {
        if (lower(s[i]) != want) return false;
    }
    return true;
}
