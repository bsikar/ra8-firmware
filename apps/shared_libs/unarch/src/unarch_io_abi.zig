//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Flat-memory backing for the shared unarchiver read seam, ported from
//! src/unarch_io.c (RA8FW-1196) behind the unchanged C headers in inc/.
//!
//! One trivial ::unarch_read_fn implementation everybody shares: a
//! bounds-clamped copy out of an unarch_mem_t view. Reads never touch a byte
//! outside [base, base + len) regardless of the (untrusted) offset and
//! length arguments, so the wrapped-open paths can point it at a decompressed
//! arena without further guarding. Pure read of an immutable view, no
//! externs: the test step needs no teststub.

const std = @import("std");

/// unarch_io.h: `typedef struct { const uint8_t* base; size_t len; }
/// unarch_mem_t;` -- the mirror is `extern` so the layout stays ABI-stable
/// across the C and Zig builds.
const unarch_mem_t = extern struct {
    base: ?[*]const u8,
    len: usize,
};

/// unarch_io.h: `[[nodiscard]] size_t unarch_mem_read(void* ctx, uint64_t
/// offset, void* buf, size_t len)` -- serves [offset, offset + len) clamped
/// to the view's bounds by plain copy; reads at or past the end return 0
/// (EOF), as does a NULL context, view base, or destination.
pub export fn unarch_mem_read(
    ctx: ?*const anyopaque,
    offset: u64,
    buf: ?*anyopaque,
    len: usize,
) callconv(.c) usize {
    const m: ?*const unarch_mem_t = @ptrCast(@alignCast(ctx));
    if (m == null) {
        return 0;
    }
    if (m.?.base == null) {
        return 0;
    }
    if (buf == null) {
        return 0;
    }
    if (offset >= m.?.len) {
        return 0;
    }
    const avail: u64 = @as(u64, m.?.len) - offset;
    const n: usize = @intCast(@min(@as(u64, len), avail));
    if (n == 0) {
        return 0;
    }
    const dst: [*]u8 = @ptrCast(buf.?);
    const start: usize = @intCast(offset);
    @memcpy(dst[0..n], m.?.base.?[start..][0..n]);
    return n;
}

const null_mem = unarch_mem_t{ .base = null, .len = 4 };
var live_mem = unarch_mem_t{ .base = null, .len = 0 };

fn stub_view(base: [*]const u8, len: usize) *const anyopaque {
    live_mem = .{ .base = base, .len = len };
    return @ptrCast(&live_mem);
}

test "null context, base, or destination read zero" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = undefined;
    const view = stub_view(&payload, payload.len);
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(null, 0, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(@ptrCast(&null_mem), 0, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, 0, null, 4));
}

test "reads at or past the end read zero (EOF)" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = undefined;
    const view = stub_view(&payload, payload.len);
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, payload.len, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, payload.len + 1, &out, 4));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, std.math.maxInt(u64), &out, 4));
}

test "reads clamp to the view's bounds" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = undefined;
    const view = stub_view(&payload, payload.len);
    // Full read, then an over-long tail read clamped to what is left.
    try std.testing.expectEqual(@as(usize, 8), unarch_mem_read(view, 0, &out, 8));
    try std.testing.expectEqualSlices(u8, &payload, out[0..8]);
    try std.testing.expectEqual(@as(usize, 5), unarch_mem_read(view, 3, &out, 16));
    try std.testing.expectEqualSlices(u8, payload[3..], out[0..5]);
}

test "zero-length request reads zero without touching anything" {
    var payload: [8]u8 = .{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var out: [8]u8 = .{ 0, 0, 0, 0, 0, 0, 0, 0 };
    const view = stub_view(&payload, payload.len);
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, 0, &out, 0));
    try std.testing.expectEqual(@as(usize, 0), unarch_mem_read(view, payload.len, &out, 0));
    const zeros: [8]u8 = @splat(0);
    try std.testing.expectEqualSlices(u8, &zeros, &out);
}
