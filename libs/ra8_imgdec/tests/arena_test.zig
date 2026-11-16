//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the stb_image allocator hooks (RA8FW-1238): bind routing,
//! the zero-size reserve, and the forwarding contract the scratch publishes.

const std = @import("std");
const hooks = @import("hooks");
const Scratch = hooks.Scratch;

test "bind routes malloc into the bound arena" {
    hooks.ra8_img_arena_unbind();
    defer hooks.ra8_img_arena_unbind();

    var store: [64]u8 = undefined;
    var arena = Scratch{};
    arena.base = &store;
    arena.cap = store.len;

    hooks.ra8_img_arena_bind(&arena);
    const block = hooks.ra8_img_arena_malloc(16);
    try std.testing.expect(block != null);
    const bytes: [*]u8 = @ptrCast(block.?);
    const store_addr = @intFromPtr(&store);
    try std.testing.expect(@intFromPtr(bytes) >= store_addr);
    try std.testing.expect(@intFromPtr(bytes) < store_addr + store.len);
    try std.testing.expectEqual(@as(usize, 1), arena.live);
}

test "malloc before a bind returns null" {
    hooks.ra8_img_arena_unbind();
    defer hooks.ra8_img_arena_unbind();

    try std.testing.expect(hooks.ra8_img_arena_malloc(8) == null);
    try std.testing.expect(hooks.ra8_img_arena_realloc_sized(null, 0, 8) == null);
    hooks.ra8_img_arena_free(null);
}

test "zero-size request reserves one byte" {
    hooks.ra8_img_arena_unbind();
    defer hooks.ra8_img_arena_unbind();

    var store: [16]u8 = undefined;
    var arena = Scratch{};
    arena.base = &store;
    arena.cap = store.len;

    hooks.ra8_img_arena_bind(&arena);
    const block = hooks.ra8_img_arena_malloc(0);
    try std.testing.expect(block != null);
    // The one-byte reserve still takes a whole 16-byte-aligned step.
    try std.testing.expectEqual(@as(usize, 16), arena.offset);
}

test "unbind clears the slot so nothing hands out after it" {
    hooks.ra8_img_arena_unbind();
    defer hooks.ra8_img_arena_unbind();

    var store: [64]u8 = undefined;
    var arena = Scratch{};
    arena.base = &store;
    arena.cap = store.len;

    hooks.ra8_img_arena_bind(&arena);
    hooks.ra8_img_arena_unbind();
    try std.testing.expect(hooks.ra8_img_arena_malloc(8) == null);
    try std.testing.expectEqual(@as(usize, 0), arena.offset);
}

test "realloc grows by copying then rewinds when the copy frees the last block" {
    hooks.ra8_img_arena_unbind();
    defer hooks.ra8_img_arena_unbind();

    var store: [64]u8 = undefined;
    var arena = Scratch{};
    arena.base = &store;
    arena.cap = store.len;

    hooks.ra8_img_arena_bind(&arena);
    const first = hooks.ra8_img_arena_malloc(8);
    try std.testing.expect(first != null);
    const first_bytes: [*]u8 = @ptrCast(first.?);
    @memset(first_bytes[0..8], 0xAB);

    const grown = hooks.ra8_img_arena_realloc_sized(first, 8, 32);
    try std.testing.expect(grown != null);
    const grown_bytes: [*]u8 = @ptrCast(grown.?);
    try std.testing.expectEqualSlices(u8, first_bytes[0..8], grown_bytes[0..8]);
    // The old block released, so with one live block the cursor rewinds and
    // the copy re-reserves from the top: the whole buffer still fits.
    try std.testing.expectEqual(@as(usize, 1), arena.live);

    const shrunk = hooks.ra8_img_arena_realloc_sized(grown, 32, 4);
    try std.testing.expect(shrunk != null);
    try std.testing.expectEqual(@as(usize, 1), arena.live);
}

test "realloc to zero reserves one byte instead of returning null" {
    hooks.ra8_img_arena_unbind();
    defer hooks.ra8_img_arena_unbind();

    var store: [64]u8 = undefined;
    var arena = Scratch{};
    arena.base = &store;
    arena.cap = store.len;

    hooks.ra8_img_arena_bind(&arena);
    const first = hooks.ra8_img_arena_malloc(8);
    try std.testing.expect(first != null);
    const shrunk = hooks.ra8_img_arena_realloc_sized(first, 8, 0);
    try std.testing.expect(shrunk != null);
}

test "bind of an unusable arena leaves the hooks refusing" {
    hooks.ra8_img_arena_unbind();
    defer hooks.ra8_img_arena_unbind();

    var empty = Scratch{};
    hooks.ra8_img_arena_bind(&empty);
    try std.testing.expect(hooks.ra8_img_arena_malloc(8) == null);

    var full = Scratch{ .cap = 8 };
    hooks.ra8_img_arena_bind(&full);
    try std.testing.expect(hooks.ra8_img_arena_malloc(8) == null);
}
