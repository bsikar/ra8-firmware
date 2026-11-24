// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the pagination cache serialiser/loader (`src/reflow_cache.zig`).

const std = @import("std");
const cache = @import("reflow").cache;

const testing = std.testing;
const ReflowEngine = cache.ReflowEngine;
const ReflowGlyph = cache.ReflowGlyph;
const ReflowPage = cache.ReflowPage;

fn makeTestEngine(alloc: std.mem.Allocator, glyphs: []const ReflowGlyph, pages: []const ReflowPage) !*ReflowEngine {
    const eng = try alloc.create(ReflowEngine);
    eng.* = std.mem.zeroes(ReflowEngine);
    eng.in_use = 1;
    eng.viewport_w = 320;
    eng.viewport_h = 480;
    eng.font_px = 16;
    eng.body_color = 0x00FF00;
    eng.link_color = 0x0000FF;
    for (glyphs, 0..) |g, i| {
        eng.glyphs[i] = g;
    }
    eng.glyph_count = @intCast(glyphs.len);
    for (pages, 0..) |p, i| {
        eng.pages[i] = p;
    }
    eng.page_count = @intCast(pages.len);
    return eng;
}

test "cache serialize/load round-trip restores glyphs and pages byte-exactly" {
    const a = testing.allocator;
    const glyphs = [_]ReflowGlyph{
        .{ .x = 10, .y = -4, .cp = 'A', .color = 0xDEADBEEF, .font_px = 16, .style = 3, .reserved = 7 },
        .{ .x = 26, .y = 0, .cp = 0x1F600, .color = 0x12345678, .font_px = 9, .style = 0, .reserved = 0 },
    };
    const pages = [_]ReflowPage{
        .{ .glyph_first = 0, .glyph_count = 2 },
        .{ .glyph_first = 2, .glyph_count = 0 },
    };
    const eng = try makeTestEngine(a, &glyphs, &pages);
    defer a.destroy(eng);

    var blob: [512]u8 = undefined;
    var out_len: usize = 0;
    const content = "chapter one";
    try testing.expectEqual(cache.k_ra8_ok, cache.reflow_cache_serialize(eng, content.ptr, content.len, &blob, blob.len, &out_len));
    try testing.expectEqual(cache.k_reflow_cache_header_bytes + glyphs.len * cache.k_reflow_cache_glyph_bytes + pages.len * cache.k_reflow_cache_page_bytes, out_len);

    const restored = try makeTestEngine(a, &.{}, &.{});
    defer a.destroy(restored);
    try testing.expectEqual(cache.k_ra8_ok, cache.reflow_cache_load(restored, content.ptr, content.len, &blob, out_len));
    try testing.expectEqual(eng.glyph_count, restored.glyph_count);
    try testing.expectEqual(eng.page_count, restored.page_count);
    try testing.expectEqual(eng.glyphs[0], restored.glyphs[0]);
    try testing.expectEqual(eng.glyphs[1], restored.glyphs[1]);
    try testing.expectEqual(eng.pages[0], restored.pages[0]);
    try testing.expectEqual(eng.pages[1], restored.pages[1]);
    try testing.expectEqual(content.len, restored.xhtml_len);
    try testing.expectEqual(@as(usize, @intFromPtr(content.ptr)), @as(usize, @intFromPtr(restored.xhtml_buf.?)));
}

test "cache blob header is byte-exact on the wire" {
    const a = testing.allocator;
    const eng = try makeTestEngine(a, &.{}, &.{});
    defer a.destroy(eng);
    var blob: [128]u8 = undefined;
    var out_len: usize = 0;
    try testing.expectEqual(cache.k_ra8_ok, cache.reflow_cache_serialize(eng, null, 0, &blob, blob.len, &out_len));
    try testing.expectEqual(cache.k_reflow_cache_header_bytes, out_len);
    try testing.expectEqualSlices(u8, &.{ 0x31, 0x43, 0x46, 0x52 }, blob[0..4]);
    try testing.expectEqualSlices(u8, &.{ 0x02, 0x00, 0x00, 0x00 }, blob[4..8]);
    try testing.expectEqualSlices(u8, &.{ 0x40, 0x01 }, blob[8..10]);
    try testing.expectEqualSlices(u8, &.{ 0xE0, 0x01 }, blob[10..12]);
    try testing.expectEqualSlices(u8, &.{ 0x10, 0x00 }, blob[12..14]);
    try testing.expectEqualSlices(u8, &.{ 0x00, 0x00 }, blob[14..16]);
    try testing.expectEqual(cache.k_priv_fnv_offset, cache.internal_fnv1a(null, 0));
}

test "cache load rejects a foreign magic, a stale key and a bad version" {
    const a = testing.allocator;
    const glyphs = [_]ReflowGlyph{.{ .x = 3, .y = 4, .cp = 'Z', .color = 9, .font_px = 16, .style = 1, .reserved = 2 }};
    const eng = try makeTestEngine(a, &glyphs, &.{});
    defer a.destroy(eng);
    var blob: [128]u8 = undefined;
    var out_len: usize = 0;
    const content = "abc";
    try testing.expectEqual(cache.k_ra8_ok, cache.reflow_cache_serialize(eng, content.ptr, content.len, &blob, blob.len, &out_len));

    var bad = blob;
    bad[0] ^= 0xFF;
    try testing.expectEqual(cache.k_ra8_err_not_found, cache.reflow_cache_load(eng, content.ptr, content.len, &bad, out_len));

    bad = blob;
    bad[4] = 0x63;
    try testing.expectEqual(cache.k_ra8_err_invalid_state, cache.reflow_cache_load(eng, content.ptr, content.len, &bad, out_len));

    try testing.expectEqual(cache.k_ra8_err_invalid_state, cache.reflow_cache_load(eng, "xyz".ptr, 3, &blob, out_len));
    try testing.expectEqual(cache.k_ra8_err_invalid_size, cache.reflow_cache_load(eng, content.ptr, content.len, &blob, cache.k_reflow_cache_header_bytes - 1));

    bad = blob;
    bad[cache.k_reflow_cache_header_bytes] ^= 0x55;
    try testing.expectEqual(cache.k_ra8_err_checksum_mismatch, cache.reflow_cache_load(eng, content.ptr, content.len, &bad, out_len));

    eng.in_use = 0;
    try testing.expectEqual(cache.k_ra8_err_not_initialized, cache.reflow_cache_load(eng, content.ptr, content.len, &blob, out_len));
    eng.in_use = 1;

    try testing.expectEqual(cache.k_ra8_err_invalid_arg, cache.reflow_cache_serialize(eng, null, 4, &blob, blob.len, &out_len));
}

test "cache serialize rejects a short destination buffer" {
    const a = testing.allocator;
    const glyphs = [_]ReflowGlyph{.{ .x = 1, .y = 2, .cp = 'A', .color = 1, .font_px = 16, .style = 0, .reserved = 0 }};
    const eng = try makeTestEngine(a, &glyphs, &.{});
    defer a.destroy(eng);
    var blob: [cache.k_reflow_cache_header_bytes]u8 = undefined;
    var out_len: usize = 0;
    try testing.expectEqual(cache.k_ra8_err_invalid_size, cache.reflow_cache_serialize(eng, null, 0, &blob, blob.len, &out_len));
}

test "cache size reports the header for an empty layout" {
    const a = testing.allocator;
    const eng = try makeTestEngine(a, &.{}, &.{});
    defer a.destroy(eng);
    var bytes: usize = 0;
    try testing.expectEqual(cache.k_ra8_ok, cache.reflow_cache_size(eng, &bytes));
    try testing.expectEqual(cache.k_reflow_cache_header_bytes, bytes);
    try testing.expectEqual(cache.k_ra8_err_null_ptr, cache.reflow_cache_size(null, &bytes));
}
