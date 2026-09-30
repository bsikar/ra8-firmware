//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Constant-work byte comparison for security verdicts (#2908).
//!
//! A comparison that returns the moment it finds a mismatch leaks, through
//! its own timing, how many leading bytes matched. Against a MAC, an
//! authentication tag, an image digest or a key, that is a byte-at-a-time
//! forgery of the compared value. `memcmp` and `std.mem.eql` both early-out
//! and are therefore unsafe here; this one walks the whole length every time
//! and reports from an accumulator, so the work depends only on the length.

/// Whether two equal-length buffers match, in work that does not depend on
/// their contents. Two empty slices are vacuously equal, which is what the
/// `len == 0` case of the C contract promised.
pub fn equal(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    var diff: u8 = 0;
    for (a, b) |lhs, rhs| {
        diff |= lhs ^ rhs;
    }
    return diff == 0;
}
