//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C-string half of the freestanding runtime.
//!
//! Where the C contract carries a length the functions here take a slice.
//! Where it does not, they take `[*:0]const u8`, which is Zig's way of
//! saying "a pointer whose end is a sentinel": that is what `strlen` and
//! `strcmp` are actually handed, and pretending otherwise would mean
//! inventing a length the caller never had.

/// Length up to the terminator.
pub fn length(s: [*:0]const u8) usize {
    var len: usize = 0;
    while (s[len] != 0) len += 1;
    return len;
}

/// Length up to the terminator or `max`, whichever comes first.
pub fn lengthBounded(s: [*]const u8, max: usize) usize {
    var len: usize = 0;
    while (len < max) : (len += 1) {
        if (s[len] == 0) break;
    }
    return len;
}

/// Compare to the first difference or the terminator.
///
/// Returns the difference of the two bytes, widened, which is what the C
/// returned; callers that only look at the sign are unaffected either way.
pub fn compare(a: [*:0]const u8, b: [*:0]const u8) i32 {
    var index: usize = 0;
    while (a[index] != 0) {
        if (a[index] != b[index]) break;
        index += 1;
    }
    return @as(i32, a[index]) - @as(i32, b[index]);
}

/// Compare at most `n` bytes, stopping at a shared terminator.
pub fn compareBounded(a: [*]const u8, b: [*]const u8, n: usize) i32 {
    var index: usize = 0;
    while (index < n) : (index += 1) {
        if (a[index] != b[index]) return @as(i32, a[index]) - @as(i32, b[index]);
        if (a[index] == 0) return 0;
    }
    return 0;
}

/// Offset of the first `target`, or null.
///
/// Searching for the terminator finds it, which is why this returns the
/// offset of the terminator rather than null in that case: `strchr(s, 0)`
/// is defined to point at the end of the string.
pub fn indexOfChar(s: [*:0]const u8, target: u8) ?usize {
    var index: usize = 0;
    while (s[index] != 0) : (index += 1) {
        if (s[index] == target) return index;
    }
    return if (target == 0) index else null;
}

/// Offset of the last `target`, or null. Same terminator rule as above.
pub fn lastIndexOfChar(s: [*:0]const u8, target: u8) ?usize {
    var index: usize = 0;
    var found: ?usize = null;
    while (s[index] != 0) : (index += 1) {
        if (s[index] == target) found = index;
    }
    return if (target == 0) index else found;
}

/// Offset of the first occurrence of `needle` in `haystack`, or null.
///
/// An empty needle matches at the start, which is what the C standard says
/// and what the C here did.
pub fn indexOfString(haystack: [*:0]const u8, needle: [*:0]const u8) ?usize {
    if (needle[0] == 0) return 0;
    var start: usize = 0;
    while (haystack[start] != 0) : (start += 1) {
        var offset: usize = 0;
        while (needle[offset] != 0) : (offset += 1) {
            if (haystack[start + offset] != needle[offset]) break;
        }
        if (needle[offset] == 0) return start;
    }
    return null;
}

/// Copy `src` and its terminator into `dst`.
pub fn copy(dst: [*]u8, src: [*:0]const u8) void {
    var index: usize = 0;
    while (src[index] != 0) : (index += 1) dst[index] = src[index];
    dst[index] = 0;
}

/// Copy at most `n` bytes of `src`, then pad `dst` to `n` with terminators.
///
/// Note what this does NOT do: a `src` at least `n` long leaves `dst`
/// unterminated, exactly as `strncpy` has always behaved.
pub fn copyBounded(dst: [*]u8, src: [*]const u8, n: usize) void {
    var index: usize = 0;
    while (index < n) : (index += 1) {
        if (src[index] == 0) break;
        dst[index] = src[index];
    }
    while (index < n) : (index += 1) dst[index] = 0;
}
