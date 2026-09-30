//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the freestanding runtime primitives declared in
//! `libs/ra8_core/inc/ra8_freestanding.h`: the subset of libc the firmware
//! provides for itself because it links no libc at all.
//!
//! The logic lives in the three modules behind this file. This one only puts
//! the C signatures back on: raw pointers in, raw pointers out, and the exact
//! return values the C produced.
//!
//! The exported names carry a prefix from the build (`-Dabi-prefix`). An
//! image wants the bare standard names, because the toolchain emits calls to
//! `memcpy` and `memset` from ordinary struct assignment and array
//! initialisation and the definitions have to answer to those symbols. A host
//! test wants `ra8_`-prefixed ones instead, so the suite can exercise these
//! implementations without colliding with the host's own libc. The C did the
//! same thing with a block of `#define memset ra8_memset` in the header,
//! guarded by `RA8_TEST_FREESTANDING`.

const builtin = @import("builtin");
const options = @import("build_options");
const math = @import("freestanding_math");
const mem = @import("freestanding_mem");
const str = @import("freestanding_str");

// -- memory ----------------------------------------------------------------

fn memset(dst: ?*anyopaque, value: c_int, n: usize) callconv(.c) ?*anyopaque {
    const bytes: [*]u8 = @ptrCast(dst orelse return dst);
    mem.set(bytes[0..n], @truncate(@as(c_uint, @bitCast(value))));
    return dst;
}

fn memcpy(noalias dst: ?*anyopaque, noalias src: ?*const anyopaque, n: usize) callconv(.c) ?*anyopaque {
    if (n != 0) {
        const out: [*]u8 = @ptrCast(dst.?);
        const in: [*]const u8 = @ptrCast(src.?);
        mem.copy(out[0..n], in[0..n]);
    }
    return dst;
}

fn memmove(dst: ?*anyopaque, src: ?*const anyopaque, n: usize) callconv(.c) ?*anyopaque {
    if (n != 0) {
        const out: [*]u8 = @ptrCast(dst.?);
        const in: [*]const u8 = @ptrCast(src.?);
        mem.move(out[0..n], in[0..n]);
    }
    return dst;
}

fn memcmp(a: ?*const anyopaque, b: ?*const anyopaque, n: usize) callconv(.c) c_int {
    if (n == 0) return 0;
    const left: [*]const u8 = @ptrCast(a.?);
    const right: [*]const u8 = @ptrCast(b.?);
    return mem.compare(left[0..n], right[0..n]);
}

fn memchr(s: ?*const anyopaque, c: c_int, n: usize) callconv(.c) ?*anyopaque {
    const bytes: [*]const u8 = @ptrCast(s orelse return null);
    const target: u8 = @truncate(@as(c_uint, @bitCast(c)));
    const found = mem.indexOf(bytes[0..n], target) orelse return null;
    return @ptrCast(@constCast(bytes + found));
}

// -- strings ---------------------------------------------------------------

fn strlen(s: [*:0]const u8) callconv(.c) usize {
    return str.length(s);
}

fn strnlen(s: [*]const u8, maxlen: usize) callconv(.c) usize {
    return str.lengthBounded(s, maxlen);
}

fn strcmp(a: [*:0]const u8, b: [*:0]const u8) callconv(.c) c_int {
    return str.compare(a, b);
}

fn strncmp(a: [*]const u8, b: [*]const u8, n: usize) callconv(.c) c_int {
    if (n == 0) return 0;
    return str.compareBounded(a, b, n);
}

fn strchr(s: [*:0]const u8, c: c_int) callconv(.c) ?[*]u8 {
    const target: u8 = @truncate(@as(c_uint, @bitCast(c)));
    const found = str.indexOfChar(s, target) orelse return null;
    return @constCast(s + found);
}

fn strrchr(s: [*:0]const u8, c: c_int) callconv(.c) ?[*]u8 {
    const target: u8 = @truncate(@as(c_uint, @bitCast(c)));
    const found = str.lastIndexOfChar(s, target) orelse return null;
    return @constCast(s + found);
}

fn strstr(haystack: [*:0]const u8, needle: [*:0]const u8) callconv(.c) ?[*]u8 {
    const found = str.indexOfString(haystack, needle) orelse return null;
    return @constCast(haystack + found);
}

fn strcpy(noalias dst: [*]u8, noalias src: [*:0]const u8) callconv(.c) [*]u8 {
    str.copy(dst, src);
    return dst;
}

fn strncpy(noalias dst: [*]u8, noalias src: [*]const u8, n: usize) callconv(.c) [*]u8 {
    str.copyBounded(dst, src, n);
    return dst;
}

// -- runtime odds and ends -------------------------------------------------

fn abs(j: c_int) callconv(.c) c_int {
    return math.absolute(j);
}

fn errnoLocation() callconv(.c) *i32 {
    return &math.errno_slot;
}

// -- the exported surface --------------------------------------------------

/// Every symbol this archive answers to, in header order.
const surface = .{
    .{ "memset", &memset },
    .{ "memcpy", &memcpy },
    .{ "memmove", &memmove },
    .{ "memcmp", &memcmp },
    .{ "memchr", &memchr },
    .{ "strlen", &strlen },
    .{ "strnlen", &strnlen },
    .{ "strcmp", &strcmp },
    .{ "strncmp", &strncmp },
    .{ "strchr", &strchr },
    .{ "strrchr", &strrchr },
    .{ "strstr", &strstr },
    .{ "strcpy", &strcpy },
    .{ "strncpy", &strncpy },
    .{ "abs", &abs },
};

comptime {
    for (surface) |entry| {
        @export(entry[1], .{ .name = options.abi_prefix ++ entry[0], .linkage = .strong });
    }
    // Only the ARM EABI's libm asks for this, and only an ARM image links it.
    if (builtin.target.cpu.arch.isArm() or builtin.target.cpu.arch.isThumb()) {
        @export(&errnoLocation, .{ .name = options.abi_prefix ++ "__errno", .linkage = .strong });
    }
}
