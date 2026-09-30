//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the tile geometry: which tiles a pixel rectangle covers,
//! and which run of tiles a pan sweep should walk. No cache and no engine is
//! involved, which is the point of the file being on its own.

const std = @import("std");
const geometry = @import("tile_geometry");

const Grid = geometry.Grid;
const PrefetchReq = geometry.PrefetchReq;
const Rect = geometry.Rect;

/// A 10x8 grid of 64x64 tiles, the shape a comic page takes in the viewer.
const page: Grid = .{ .tile_w = 64, .tile_h = 64, .cols = 10, .rows = 8 };

test "a rectangle inside one tile names that one tile" {
    const rect = geometry.rectOfPixels(10, 10, 20, 20, page).?;
    try std.testing.expectEqual(Rect{ .tx0 = 0, .ty0 = 0, .tx1 = 0, .ty1 = 0 }, rect);
}

test "a rectangle straddling a tile boundary names both tiles" {
    const rect = geometry.rectOfPixels(60, 60, 10, 10, page).?;
    try std.testing.expectEqual(Rect{ .tx0 = 0, .ty0 = 0, .tx1 = 1, .ty1 = 1 }, rect);
}

test "a rectangle ending exactly on a boundary does not claim the next tile" {
    // The last pixel is 63, still tile 0: the inclusive end is what decides.
    const rect = geometry.rectOfPixels(0, 0, 64, 64, page).?;
    try std.testing.expectEqual(Rect{ .tx0 = 0, .ty0 = 0, .tx1 = 0, .ty1 = 0 }, rect);
}

test "a rectangle running past the image clamps to the last tile" {
    const rect = geometry.rectOfPixels(0, 0, 100_000, 100_000, page).?;
    try std.testing.expectEqual(@as(u16, 9), rect.tx1);
    try std.testing.expectEqual(@as(u16, 7), rect.ty1);
}

test "a rectangle near the top of the range clamps instead of wrapping" {
    // The C added `px + pw` in 32 bits, so this pair wrapped and the inclusive
    // end landed back at tile 0, understating residency to a single tile.
    const rect = geometry.rectOfPixels(0xFFFF_FF00, 1, 0x200, 1, page).?;
    try std.testing.expectEqual(@as(u16, 9), rect.tx0);
    try std.testing.expectEqual(@as(u16, 9), rect.tx1);
}

test "an empty rectangle names no tiles" {
    try std.testing.expect(geometry.rectOfPixels(0, 0, 0, 10, page) == null);
    try std.testing.expect(geometry.rectOfPixels(0, 0, 10, 0, page) == null);
}

test "a degenerate grid is rejected on every axis" {
    const axes = [_]Grid{
        .{ .tile_w = 0, .tile_h = 64, .cols = 10, .rows = 8 },
        .{ .tile_w = 64, .tile_h = 0, .cols = 10, .rows = 8 },
        .{ .tile_w = 64, .tile_h = 64, .cols = 0, .rows = 8 },
        .{ .tile_w = 64, .tile_h = 64, .cols = 10, .rows = 0 },
    };
    for (axes) |grid| {
        try std.testing.expect(geometry.rectOfPixels(0, 0, 10, 10, grid) == null);
    }
}

/// A viewport of 2 columns by 3 rows, comfortably inside a 10x8 grid.
fn request(dir: geometry.PanDir) PrefetchReq {
    return .{
        .image_id = 7,
        .view = .{ .tx0 = 2, .ty0 = 2, .tx1 = 3, .ty1 = 4 },
        .tile_cols = 10,
        .tile_rows = 8,
        .zoom = 1,
        .max_tiles = 16,
        .dir = dir,
    };
}

test "an unordered or off-grid view is not sane" {
    var req = request(.right);
    req.view = .{ .tx0 = 4, .ty0 = 2, .tx1 = 3, .ty1 = 4 };
    try std.testing.expect(!geometry.viewIsSane(&req));

    req = request(.right);
    req.view = .{ .tx0 = 2, .ty0 = 5, .tx1 = 3, .ty1 = 4 };
    try std.testing.expect(!geometry.viewIsSane(&req));

    req = request(.right);
    req.tile_cols = 3;
    try std.testing.expect(!geometry.viewIsSane(&req));

    req = request(.right);
    req.tile_rows = 4;
    try std.testing.expect(!geometry.viewIsSane(&req));
}

test "panning right names the column past the viewport, one tile per visible row" {
    const req = request(.right);
    const line = geometry.panLine(&req).?;
    try std.testing.expectEqual(@as(u16, 4), line.x);
    try std.testing.expectEqual(@as(u16, 2), line.y);
    try std.testing.expectEqual(@as(u16, 3), line.count);
    try std.testing.expectEqual(@as(u16, 4), line.tileAt(2).x);
    try std.testing.expectEqual(@as(u16, 4), line.tileAt(2).y);
}

test "panning left names the column before the viewport" {
    const req = request(.left);
    const line = geometry.panLine(&req).?;
    try std.testing.expectEqual(@as(u16, 1), line.x);
    try std.testing.expectEqual(@as(u16, 3), line.count);
}

test "panning down names the row below, one tile per visible column" {
    const req = request(.down);
    const line = geometry.panLine(&req).?;
    try std.testing.expectEqual(@as(u16, 2), line.x);
    try std.testing.expectEqual(@as(u16, 5), line.y);
    try std.testing.expectEqual(@as(u16, 2), line.count);
    try std.testing.expectEqual(@as(u16, 3), line.tileAt(1).x);
    try std.testing.expectEqual(@as(u16, 5), line.tileAt(1).y);
}

test "panning up names the row above" {
    const req = request(.up);
    const line = geometry.panLine(&req).?;
    try std.testing.expectEqual(@as(u16, 1), line.y);
    try std.testing.expectEqual(@as(u16, 2), line.count);
}

test "a pan already against the image edge has no lead edge" {
    var req = request(.right);
    req.view = .{ .tx0 = 8, .ty0 = 2, .tx1 = 9, .ty1 = 4 };
    try std.testing.expect(geometry.panLine(&req) == null);

    req = request(.left);
    req.view = .{ .tx0 = 0, .ty0 = 2, .tx1 = 1, .ty1 = 4 };
    try std.testing.expect(geometry.panLine(&req) == null);

    req = request(.down);
    req.view = .{ .tx0 = 2, .ty0 = 6, .tx1 = 3, .ty1 = 7 };
    try std.testing.expect(geometry.panLine(&req) == null);

    req = request(.up);
    req.view = .{ .tx0 = 2, .ty0 = 0, .tx1 = 3, .ty1 = 1 };
    try std.testing.expect(geometry.panLine(&req) == null);
}

test "a still viewport and a forged direction both warm nothing" {
    const still = request(.none);
    try std.testing.expect(geometry.panLine(&still) == null);

    // The enum is non-exhaustive on purpose: a byte the membrane never
    // defined must reach the default arm rather than be illegal behaviour.
    const forged = request(@enumFromInt(200));
    try std.testing.expect(geometry.panLine(&forged) == null);
}
