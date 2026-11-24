// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Unit tests for the fallback render face picker (`src/reflow_render_face.zig`).

const std = @import("std");
const face = @import("reflow").render_face;

test "face picker keeps the layout pick when fallback is disabled" {
    try std.testing.expectEqual(@as(u8, 2), face.priv_reflow_render_pick_face(null, null, 3, 2, 65));
    try std.testing.expectEqual(@as(u8, 0), face.priv_reflow_render_pick_face(&face.faceHasGlyph, null, 1, 0, 65));
    try std.testing.expectEqual(@as(u8, 5), face.priv_reflow_render_pick_face(&face.faceHasGlyph, null, 3, 5, 65));
}

test "face picker falls back by coverage" {
    const harness = struct {
        var covered: [4]bool = .{ false, false, false, false };

        fn probe(ctx: ?*const anyopaque, idx: u8, cp: c_int) callconv(.c) bool {
            _ = ctx;
            _ = cp;
            return covered[idx];
        }
    };
    const ctx: ?*const anyopaque = null;

    harness.covered = .{ false, true, false, false };
    try std.testing.expectEqual(@as(u8, 1), face.priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));

    harness.covered = .{ true, false, false, false };
    try std.testing.expectEqual(@as(u8, 0), face.priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));

    harness.covered = .{ false, false, true, false };
    try std.testing.expectEqual(@as(u8, 2), face.priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));

    harness.covered = .{ false, false, false, false };
    try std.testing.expectEqual(@as(u8, 1), face.priv_reflow_render_pick_face(&harness.probe, ctx, 3, 1, 65));
}

test "stbtt_fontinfo mirror matches the C struct size" {
    try std.testing.expectEqual(@as(usize, 168), @sizeOf(face.StbttFontinfo));
}
