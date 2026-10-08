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
const rng = @import("freestanding_rand");
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

fn srand(value: c_uint) callconv(.c) void {
    rng.seed(@intCast(value));
}

fn rand() callconv(.c) c_int {
    return @intCast(rng.next());
}

fn errnoLocation() callconv(.c) *i32 {
    return &math.errno_slot;
}

// -- ARM EABI memory helpers ----------------------------------------------
//
// GCC and LLVM lower some copies and clears on ARM to these run-time ABI
// entry points (RTABI 4.3.4). Newlib's libc would supply them, but images
// link no libc, and the Zig archives no longer carry compiler_rt into a
// cortex-m link (RA8FW-943), so the firmware answers to them here. The
// aligned variants only promise alignment; plain byte copies satisfy them.

fn aeabiMemcpy(noalias dst: ?*anyopaque, noalias src: ?*const anyopaque, n: usize) callconv(.c) void {
    _ = memcpy(dst, src, n);
}

fn aeabiMemmove(dst: ?*anyopaque, src: ?*const anyopaque, n: usize) callconv(.c) void {
    _ = memmove(dst, src, n);
}

/// The EABI order is (dest, n, c), not memset's (dest, c, n).
fn aeabiMemset(dst: ?*anyopaque, n: usize, value: c_int) callconv(.c) void {
    _ = memset(dst, value, n);
}

fn aeabiMemclr(dst: ?*anyopaque, n: usize) callconv(.c) void {
    _ = memset(dst, 0, n);
}

/// ARM EHABI's _URC_FAILURE: "unwinding cannot proceed".
const urc_failure: c_int = 9;

/// Zig's cortex-m objects carry .ARM.exidx entries that name the EHABI
/// personality routines. Nothing in an image unwinds (C has no exceptions
/// and a Zig panic halts), so the routines only have to exist. Without
/// these, libgcc's real unwinder would be pulled in, and it wants abort and
/// __exidx_start, which an image doesn't have. Used to come from the
/// bundled compiler_rt (RA8FW-943).
fn aeabiUnwindPersonality() callconv(.c) c_int {
    return urc_failure;
}

/// Every EABI helper name, each variant mapped to its plain form.
const aeabi_surface = .{
    .{ "__aeabi_memcpy", &aeabiMemcpy },
    .{ "__aeabi_memcpy4", &aeabiMemcpy },
    .{ "__aeabi_memcpy8", &aeabiMemcpy },
    .{ "__aeabi_memmove", &aeabiMemmove },
    .{ "__aeabi_memmove4", &aeabiMemmove },
    .{ "__aeabi_memmove8", &aeabiMemmove },
    .{ "__aeabi_memset", &aeabiMemset },
    .{ "__aeabi_memset4", &aeabiMemset },
    .{ "__aeabi_memset8", &aeabiMemset },
    .{ "__aeabi_memclr", &aeabiMemclr },
    .{ "__aeabi_memclr4", &aeabiMemclr },
    .{ "__aeabi_memclr8", &aeabiMemclr },
    .{ "__aeabi_unwind_cpp_pr0", &aeabiUnwindPersonality },
    .{ "__aeabi_unwind_cpp_pr1", &aeabiUnwindPersonality },
    .{ "__aeabi_unwind_cpp_pr2", &aeabiUnwindPersonality },
};

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
    .{ "srand", &srand },
    .{ "rand", &rand },
};

comptime {
    for (surface) |entry| {
        @export(entry[1], .{ .name = options.abi_prefix ++ entry[0], .linkage = .strong });
    }
    // Only the ARM EABI's libm asks for this, and only an ARM image links it.
    if (builtin.target.cpu.arch.isArm() or builtin.target.cpu.arch.isThumb()) {
        @export(&errnoLocation, .{ .name = options.abi_prefix ++ "__errno", .linkage = .strong });
        for (aeabi_surface) |entry| {
            @export(entry[1], .{ .name = options.abi_prefix ++ entry[0], .linkage = .strong });
        }
    }
}
