// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the text rasterisation helpers and ABI mirrors (`src/reflow_render.zig`).

const std = @import("std");
const render = @import("reflow").render;

const TofuRect = render.TofuRect;
const GlyphKey = render.GlyphKey;
const GlyphView = render.GlyphView;
const KeycacheCell = render.KeycacheCell;
const AtlasStorage = render.AtlasStorage;

test "blank code points draw nothing" {
    try std.testing.expect(render.priv_reflow_render_is_blank_cp(0x20));
    try std.testing.expect(render.priv_reflow_render_is_blank_cp(0xA0));
    try std.testing.expect(render.priv_reflow_render_is_blank_cp(0xFEFF));
    try std.testing.expect(!render.priv_reflow_render_is_blank_cp(0x41));
    try std.testing.expect(!render.priv_reflow_render_is_blank_cp(0xFFFF));
}

test "tofu decision mirrors the cmap probe" {
    try std.testing.expect(render.priv_reflow_render_needs_tofu(0, 0x41));
    try std.testing.expect(!render.priv_reflow_render_needs_tofu(0, 0x20));
    try std.testing.expect(!render.priv_reflow_render_needs_tofu(7, 0x41));
}

test "tofu rect geometry" {
    var rect: TofuRect = undefined;
    try std.testing.expect(render.priv_reflow_render_tofu_rect(10, 12, 12, &rect));
    try std.testing.expectEqual(@as(i32, 1), rect.x_off);
    try std.testing.expectEqual(@as(i32, -9), rect.y_off);
    try std.testing.expectEqual(@as(i32, 8), rect.w);
    try std.testing.expectEqual(@as(i32, 9), rect.h);

    try std.testing.expect(render.priv_reflow_render_tofu_rect(16, 16, 12, &rect));
    try std.testing.expectEqual(@as(i32, 12), rect.h);

    try std.testing.expect(render.priv_reflow_render_tofu_rect(0, 0, 12, &rect));
    try std.testing.expectEqual(@as(i32, 6), rect.w);

    try std.testing.expect(render.priv_reflow_render_tofu_rect(1, 0, 12, &rect));
    try std.testing.expectEqual(@as(i32, 3), rect.w);
    try std.testing.expectEqual(@as(i32, 0), rect.x_off);

    try std.testing.expect(!render.priv_reflow_render_tofu_rect(10, 12, 12, null));
    try std.testing.expect(!render.priv_reflow_render_tofu_rect(10, 12, 0, &rect));
}

test "ABI mirrors match the C layout" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(GlyphKey));
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(GlyphView));
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(KeycacheCell));
    try std.testing.expectEqual(@as(usize, 48), @offsetOf(AtlasStorage, "bucket_count"));
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(TofuRect));
}
