//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the stb_truetype allocator hooks (RA8FW-1239): the static
//! arena's bump-and-drain behaviour, the zero-byte reserve, exhaustion
//! recovery, and the high-water read. Ported from
//! tests/graphics/src/test_ra8_stbtt_alloc.c; every test frees what it
//! allocates, so the vectors stay order-independent over the shared static
//! arena.

const std = @import("std");
const hooks = @import("stbtt_alloc");

test "malloc hands out aligned blocks inside the arena" {
    const block = hooks.ra8_stbtt_malloc(64);
    try std.testing.expect(block != null);
    try std.testing.expectEqual(@as(usize, 0), @intFromPtr(block.?) % hooks.align_bytes);
    hooks.ra8_stbtt_free(block);
}

test "zero request reserves its own aligned slot" {
    const first = hooks.ra8_stbtt_malloc(0);
    const second = hooks.ra8_stbtt_malloc(0);
    try std.testing.expect(first != null);
    try std.testing.expect(second != null);
    try std.testing.expect(@intFromPtr(first.?) != @intFromPtr(second.?));
    hooks.ra8_stbtt_free(first);
    hooks.ra8_stbtt_free(second);
}

test "null free is ignored" {
    hooks.ra8_stbtt_free(null);
}

test "drain rewinds: freeing every block returns the cursor to the base" {
    const a = hooks.ra8_stbtt_malloc(32);
    const b = hooks.ra8_stbtt_malloc(32);
    try std.testing.expect(a != null and b != null);
    // stb frees composite-glyph buffers out of LIFO order; drain a first.
    hooks.ra8_stbtt_free(a);
    hooks.ra8_stbtt_free(b);
    const again = hooks.ra8_stbtt_malloc(32);
    try std.testing.expect(again != null);
    try std.testing.expectEqual(@intFromPtr(a.?), @intFromPtr(again.?));
    hooks.ra8_stbtt_free(again);
}

test "exhaustion returns null and the arena recovers on drain" {
    var blocks: [hooks.arena_bytes / hooks.align_bytes + 2]?*anyopaque = undefined;
    var count: usize = 0;
    while (count < blocks.len) : (count += 1) {
        blocks[count] = hooks.ra8_stbtt_malloc(hooks.align_bytes);
        if (blocks[count] == null) break;
    }
    try std.testing.expect(count < blocks.len);
    for (blocks[0..count]) |block| hooks.ra8_stbtt_free(block);
    const back = hooks.ra8_stbtt_malloc(16);
    try std.testing.expect(back != null);
    hooks.ra8_stbtt_free(back);
}

test "high water never decreases" {
    const before = hooks.ra8_stbtt_alloc_high_water();
    const block = hooks.ra8_stbtt_malloc(hooks.align_bytes * 4);
    try std.testing.expect(block != null);
    try std.testing.expect(hooks.ra8_stbtt_alloc_high_water() >= before);
    hooks.ra8_stbtt_free(block);
}
