//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_miniz_alloc_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_miniz_alloc_abi");

const epub_miniz_alloc = sut.epub_miniz_alloc;
const epub_miniz_arena_deinit = sut.epub_miniz_arena_deinit;
const epub_miniz_arena_init = sut.epub_miniz_arena_init;
const epub_miniz_arena_t = sut.epub_miniz_arena_t;
const epub_miniz_free = sut.epub_miniz_free;
const epub_miniz_realloc = sut.epub_miniz_realloc;
const internal_cell_at = sut.internal_cell_at;
const internal_header = sut.internal_header;
const internal_in_pool = sut.internal_in_pool;
const internal_ready = sut.internal_ready;
const k_epub_miniz_pool_bytes = sut.k_epub_miniz_pool_bytes;
const k_priv_align = sut.k_priv_align;
const k_priv_blk_free = sut.k_priv_blk_free;
const k_priv_blk_used = sut.k_priv_blk_used;
const k_priv_hdr_bytes = sut.k_priv_hdr_bytes;
const k_ra8_err_invalid_arg = sut.k_ra8_err_invalid_arg;
const k_ra8_err_invalid_size = sut.k_ra8_err_invalid_size;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_ok = sut.k_ra8_ok;

var t_workspace: [k_epub_miniz_pool_bytes]u8 align(16) = undefined;

var t_arena: epub_miniz_arena_t = undefined;

fn t_init() !void {
    const err = epub_miniz_arena_init(&t_arena, &t_workspace, t_workspace.len);
    try std.testing.expectEqual(k_ra8_ok, err);
}

test "init resets the workspace to one free block; deinit zeroes the descriptor" {
    defer _ = &t_workspace;
    try t_init();
    defer epub_miniz_arena_deinit(&t_arena);
    try std.testing.expectEqual(k_epub_miniz_pool_bytes, t_arena.capacity);
    try std.testing.expectEqual(@as(u8, 1), t_arena.initialized);
    const head = internal_cell_at(&t_arena, 0);
    try std.testing.expectEqual(k_epub_miniz_pool_bytes - k_priv_hdr_bytes, head.size);
    try std.testing.expectEqual(k_priv_blk_free, head.is_free);
    epub_miniz_arena_deinit(&t_arena);
    try std.testing.expectEqual(@as(usize, 0), t_arena.capacity);
    try std.testing.expectEqual(@as(u8, 0), t_arena.initialized);
    try std.testing.expect(t_arena.base == null);
    try std.testing.expect(!internal_ready(&t_arena));
}

test "init rejects null, short, and misaligned workspaces without touching them" {
    var arena: epub_miniz_arena_t = undefined;
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_miniz_arena_init(null, &t_workspace, t_workspace.len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_miniz_arena_init(&arena, null, t_workspace.len));
    try std.testing.expectEqual(k_ra8_err_invalid_size, epub_miniz_arena_init(&arena, &t_workspace, t_workspace.len - 1));
    const mis: *[k_epub_miniz_pool_bytes]u8 = @ptrFromInt((@intFromPtr(&t_workspace)) + 1);
    try std.testing.expectEqual(k_ra8_err_invalid_arg, epub_miniz_arena_init(&arena, mis, mis.len));
}

test "alloc/free round trip reuses the first block" {
    try t_init();
    defer epub_miniz_arena_deinit(&t_arena);
    const p = epub_miniz_alloc(&t_arena, 1, 1).?;
    try std.testing.expect((@intFromPtr(p) % k_priv_align) == 0);
    try std.testing.expect(internal_in_pool(&t_arena, p));
    const head = internal_cell_at(&t_arena, 0);
    try std.testing.expectEqual(k_priv_blk_used, head.is_free);
    const q = epub_miniz_alloc(&t_arena, 0, 1).?;
    try std.testing.expect(@intFromPtr(q) > @intFromPtr(p));
    epub_miniz_free(&t_arena, p);
    const r = epub_miniz_alloc(&t_arena, 1, 1).?;
    try std.testing.expectEqual(@intFromPtr(p), @intFromPtr(r));
    epub_miniz_free(&t_arena, q);
    epub_miniz_free(&t_arena, r);
    const whole = internal_cell_at(&t_arena, 0);
    try std.testing.expectEqual(k_epub_miniz_pool_bytes - k_priv_hdr_bytes, whole.size);
}

test "first-fit splits and coalesces adjacent free blocks" {
    try t_init();
    defer epub_miniz_arena_deinit(&t_arena);
    const a = epub_miniz_alloc(&t_arena, 64, 1).?;
    const b = epub_miniz_alloc(&t_arena, 64, 1).?;
    const c = epub_miniz_alloc(&t_arena, 64, 1).?;
    const ha = internal_header(&t_arena, a);
    try std.testing.expectEqual(@as(usize, 64), ha.size);
    epub_miniz_free(&t_arena, a);
    try std.testing.expectEqual(k_priv_blk_free, ha.is_free);
    // a's hole is the first fit; its 64-byte payload is reused whole.
    const again = epub_miniz_alloc(&t_arena, 64, 1).?;
    try std.testing.expectEqual(@intFromPtr(a), @intFromPtr(again));
    // free everything; coalescing must rebuild one whole-pool block.
    epub_miniz_free(&t_arena, b);
    epub_miniz_free(&t_arena, c);
    epub_miniz_free(&t_arena, again);
    const head = internal_cell_at(&t_arena, 0);
    try std.testing.expectEqual(k_epub_miniz_pool_bytes - k_priv_hdr_bytes, head.size);
}

test "realloc grow moves and preserves the payload, then frees the old block" {
    try t_init();
    defer epub_miniz_arena_deinit(&t_arena);
    const p = epub_miniz_alloc(&t_arena, 32, 1).?;
    const old: [*]u8 = @ptrCast(p);
    for (0..32) |i| old[i] = @intCast(i & 0xFF);
    const q = epub_miniz_realloc(&t_arena, p, 128, 1).?;
    try std.testing.expect(@intFromPtr(q) != @intFromPtr(p));
    const moved: [*]const u8 = @ptrCast(q);
    for (0..32) |i| try std.testing.expectEqual(@as(u8, @intCast(i & 0xFF)), moved[i]);
    // The old block was freed and coalesced, so first-fit hands it back.
    const back = epub_miniz_alloc(&t_arena, 32, 1).?;
    try std.testing.expectEqual(@intFromPtr(p), @intFromPtr(back));
}

test "realloc shrinks in place and treats a zero-byte request as free" {
    try t_init();
    defer epub_miniz_arena_deinit(&t_arena);
    const p = epub_miniz_alloc(&t_arena, 64, 1).?;
    try std.testing.expectEqual(@intFromPtr(p), @intFromPtr(epub_miniz_realloc(&t_arena, p, 32, 1).?));
    // items*size == 0: the C TU frees the block and returns NULL.
    try std.testing.expect(epub_miniz_realloc(&t_arena, p, 0, 1) == null);
    const head = internal_cell_at(&t_arena, 0);
    try std.testing.expectEqual(k_priv_blk_free, head.is_free);
    // NULL address: realloc allocates.
    const q = epub_miniz_realloc(&t_arena, null, 16, 1);
    try std.testing.expect(q != null);
    epub_miniz_free(&t_arena, q);
}

test "bad requests are rejected and foreign pointers are ignored" {
    try t_init();
    defer epub_miniz_arena_deinit(&t_arena);
    // Overflow of items*size.
    try std.testing.expect(epub_miniz_alloc(&t_arena, std.math.maxInt(usize), 2) == null);
    // Request larger than the whole pool.
    try std.testing.expect(epub_miniz_alloc(&t_arena, 1, k_epub_miniz_pool_bytes + 1) == null);
    // Free of a foreign pointer or null changes nothing.
    epub_miniz_free(&t_arena, @as(?*anyopaque, @ptrCast(&t_arena)));
    epub_miniz_free(&t_arena, null);
    const head = internal_cell_at(&t_arena, 0);
    try std.testing.expectEqual(k_epub_miniz_pool_bytes - k_priv_hdr_bytes, head.size);
    // Realloc on a foreign pointer fails.
    try std.testing.expect(epub_miniz_realloc(&t_arena, @as(?*anyopaque, @ptrCast(&t_arena)), 8, 1) == null);
    // Dead arena rejects everything.
    var dead: epub_miniz_arena_t = undefined;
    epub_miniz_arena_deinit(&dead);
    try std.testing.expect(epub_miniz_alloc(&dead, 1, 1) == null);
}

test "exhaustion then full recovery" {
    try t_init();
    defer epub_miniz_arena_deinit(&t_arena);
    var blocks: [256]?*anyopaque = @splat(null);
    var count: usize = 0;
    while (count < blocks.len) : (count += 1) {
        const p = epub_miniz_alloc(&t_arena, 4096, 1) orelse break;
        blocks[count] = p;
    }
    try std.testing.expect(count > 0);
    // The pool is not an exact multiple of the 4096-byte stride, so the fill
    // loop leaves a live tail block: drain it before true exhaustion.
    while (count < blocks.len) {
        const p = epub_miniz_alloc(&t_arena, 1, 1) orelse break;
        blocks[count] = p;
        count += 1;
    }
    try std.testing.expect(epub_miniz_alloc(&t_arena, 1, 1) == null);
    for (blocks[0..count]) |p| epub_miniz_free(&t_arena, p);
    // One whole-pool block survives the coalesce after the drain.
    const whole = epub_miniz_alloc(&t_arena, 1, k_epub_miniz_pool_bytes - k_priv_hdr_bytes);
    try std.testing.expect(whole != null);
    epub_miniz_free(&t_arena, whole.?);
}
