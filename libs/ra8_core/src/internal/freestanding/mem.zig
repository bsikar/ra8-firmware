//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The byte-wise half of the freestanding runtime: what `memset`, `memcpy`,
//! `memmove`, `memcmp` and `memchr` do, with no libc underneath.
//!
//! Every function here takes a slice rather than a pointer and a count,
//! because at this level a length is always known and a fat pointer is the
//! honest way to carry it. `freestanding_abi.zig` is where the C signatures
//! are put back on.
//!
//! Deliberately naive loops. These run before any cache or MMU setup on a
//! cold boot, so a word-at-a-time version would fault on an unaligned span;
//! the C carried a `no-tree-loop-distribute-patterns` pragma to stop GCC
//! rewriting the loops back into calls to themselves, and Zig needs no such
//! guard because it never does that.

/// Fill `dst` with `value`.
pub fn set(dst: []u8, value: u8) void {
    for (dst) |*byte| byte.* = value;
}

/// Copy `src` into `dst`, which must not overlap it.
pub fn copy(dst: []u8, src: []const u8) void {
    for (dst, src) |*out, byte| out.* = byte;
}

/// Copy `src` into `dst`, which may overlap it.
pub fn move(dst: []u8, src: []const u8) void {
    if (dst.ptr == src.ptr or dst.len == 0) return;
    if (@intFromPtr(dst.ptr) < @intFromPtr(src.ptr)) {
        for (dst, src) |*out, byte| out.* = byte;
        return;
    }
    var index = dst.len;
    while (index > 0) {
        index -= 1;
        dst[index] = src[index];
    }
}

/// Three-way compare, returning the sign only.
///
/// This is narrower than `strncmp` below on purpose: the C returned -1 or 1
/// here and the byte difference there, and callers of both are entitled to
/// the exact values they have always seen.
pub fn compare(a: []const u8, b: []const u8) i32 {
    for (a, b) |left, right| {
        if (left != right) return if (left < right) -1 else 1;
    }
    return 0;
}

/// Index of the first `value` in `haystack`, or null.
pub fn indexOf(haystack: []const u8, value: u8) ?usize {
    for (haystack, 0..) |byte, index| {
        if (byte == value) return index;
    }
    return null;
}
