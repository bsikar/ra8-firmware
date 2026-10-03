//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The four memory routines gcc expects of any freestanding program, for a
//! ThreadX module built from Zig through C (RA8FW-539).
//!
//! A module links no C library, and Zig's own versions live in a runtime
//! that is not part of the C the backend emits. So the module carries
//! these. They go through the same route as the module's code: emitted as C
//! and compiled by gcc with the module flags, plus one more that stops gcc
//! turning each loop back into a call to the routine it is the body of.

/// The extra gcc flag this unit needs, and no other unit does.
pub const gcc_flags = [_][]const u8{"-fno-tree-loop-distribute-patterns"};

/// A `void *` as the bytes it points at.
fn bytes(pointer: ?*anyopaque) [*]u8 {
    return @ptrCast(pointer.?);
}

fn constBytes(pointer: ?*const anyopaque) [*]const u8 {
    return @ptrCast(pointer.?);
}

export fn memcpy(
    noalias dest: ?*anyopaque,
    noalias src: ?*const anyopaque,
    len: usize,
) callconv(.c) ?*anyopaque {
    for (0..len) |index| bytes(dest)[index] = constBytes(src)[index];
    return dest;
}

export fn memmove(dest: ?*anyopaque, src: ?*const anyopaque, len: usize) callconv(.c) ?*anyopaque {
    if (@intFromPtr(dest) <= @intFromPtr(src)) {
        for (0..len) |index| bytes(dest)[index] = constBytes(src)[index];
    } else {
        var index = len;
        while (index != 0) {
            index -= 1;
            bytes(dest)[index] = constBytes(src)[index];
        }
    }
    return dest;
}

export fn memset(dest: ?*anyopaque, value: c_int, len: usize) callconv(.c) ?*anyopaque {
    for (0..len) |index| bytes(dest)[index] = @truncate(@as(c_uint, @bitCast(value)));
    return dest;
}

export fn memcmp(left: ?*const anyopaque, right: ?*const anyopaque, len: usize) callconv(.c) c_int {
    for (0..len) |index| {
        const a = constBytes(left)[index];
        const b = constBytes(right)[index];
        if (a != b) return @as(c_int, a) - b;
    }
    return 0;
}
