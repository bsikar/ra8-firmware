//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The tile geometry of `inc/ra8_tile_cache.h`: which tiles a pixel rectangle
//! covers, and which run of tiles sits one step ahead of a panning viewport.
//! Pure integer arithmetic over the tile grid, with no cache and no engine in
//! it, so the residency question and the read-ahead that acts on it are
//! answered by the same numbers and can be tested without either.

const std = @import("std");

/// `ra8_tile_rect_t`: an inclusive rectangle of tile coordinates.
pub const Rect = extern struct {
    tx0: u16 = 0,
    ty0: u16 = 0,
    tx1: u16 = 0,
    ty1: u16 = 0,
};

/// `ra8_tile_pan_dir_t`. Non-exhaustive: the C switch carried a `default`
/// arm, and a forged direction arriving over the membrane has to land there
/// rather than be illegal behaviour.
pub const PanDir = enum(u8) {
    none = 0,
    left = 1,
    right = 2,
    up = 3,
    down = 4,
    _,
};

/// `ra8_tile_prefetch_req_t`: what is visible, where it heads, how much of it
/// may be warmed.
pub const PrefetchReq = extern struct {
    image_id: u32 = 0,
    view: Rect = .{},
    tile_cols: u16 = 0,
    tile_rows: u16 = 0,
    zoom: u16 = 0,
    max_tiles: u16 = 0,
    dir: PanDir = .none,
};

/// The tile grid a pixel rectangle is resolved against. The C took these four
/// as loose parameters wedged between the four pixel ones, where a transposed
/// pair still compiles.
pub const Grid = struct {
    tile_w: u16,
    tile_h: u16,
    cols: u16,
    rows: u16,

    fn valid(self: Grid) bool {
        return self.tile_w != 0 and self.tile_h != 0 and self.cols != 0 and self.rows != 0;
    }
};

/// A tile index that ran past the grid names the last tile, so nothing
/// downstream has to re-clamp what this produced.
fn clampTile(index: u64, count: u16) u16 {
    return @intCast(@min(index, @as(u64, count) - 1));
}

/// The inclusive tile rectangle covering a pixel rectangle, or null when the
/// rectangle is empty or the grid is degenerate (`invalid_arg` at the
/// membrane). Widened to 64 bits: the C added `px + pw` in 32 bits, so a
/// rectangle near the top of the range wrapped to tile 0 instead of clamping
/// to the last tile.
pub fn rectOfPixels(px: u32, py: u32, pw: u32, ph: u32, grid: Grid) ?Rect {
    if (pw == 0 or ph == 0) return null;
    if (!grid.valid()) return null;
    const x1 = (@as(u64, px) + pw) - 1;
    const y1 = (@as(u64, py) + ph) - 1;
    return .{
        .tx0 = clampTile(px / grid.tile_w, grid.cols),
        .ty0 = clampTile(py / grid.tile_h, grid.rows),
        .tx1 = clampTile(x1 / grid.tile_w, grid.cols),
        .ty1 = clampTile(y1 / grid.tile_h, grid.rows),
    };
}

/// The lead-edge run a pan sweep walks: `count` tiles from `(x, y)`, each a
/// step of `(step_x, step_y)` on from the last (one of them 1, the other 0).
pub const PanLine = struct {
    x: u16,
    y: u16,
    step_x: u16,
    step_y: u16,
    count: u16,

    /// The tile `i` steps along the run.
    pub fn tileAt(self: PanLine, i: u16) struct { x: u16, y: u16 } {
        return .{ .x = self.x + (self.step_x * i), .y = self.y + (self.step_y * i) };
    }
};

/// The visible rectangle must be ordered and lie inside the tile grid.
pub fn viewIsSane(req: *const PrefetchReq) bool {
    const v = req.view;
    if (v.tx0 > v.tx1) return false;
    if (v.ty0 > v.ty1) return false;
    if (v.tx1 >= req.tile_cols) return false;
    if (v.ty1 >= req.tile_rows) return false;
    return true;
}

/// The run one step beyond the viewport, or null at an image edge or with no
/// pan. Call only on a request `viewIsSane` accepted.
pub fn panLine(req: *const PrefetchReq) ?PanLine {
    const v = req.view;
    const down: u16 = (v.ty1 - v.ty0) + 1;
    const across: u16 = (v.tx1 - v.tx0) + 1;
    return switch (req.dir) {
        .right => if (v.tx1 + 1 >= req.tile_cols) null else PanLine{
            .x = v.tx1 + 1,
            .y = v.ty0,
            .step_x = 0,
            .step_y = 1,
            .count = down,
        },
        .left => if (v.tx0 == 0) null else PanLine{
            .x = v.tx0 - 1,
            .y = v.ty0,
            .step_x = 0,
            .step_y = 1,
            .count = down,
        },
        .down => if (v.ty1 + 1 >= req.tile_rows) null else PanLine{
            .x = v.tx0,
            .y = v.ty1 + 1,
            .step_x = 1,
            .step_y = 0,
            .count = across,
        },
        .up => if (v.ty0 == 0) null else PanLine{
            .x = v.tx0,
            .y = v.ty0 - 1,
            .step_x = 1,
            .step_y = 0,
            .count = across,
        },
        else => null,
    };
}

comptime {
    // Spelled out rather than derived: a derivation would agree with a
    // reordered header. Both structs are caller-allocated on the C side.
    std.debug.assert(@offsetOf(Rect, "tx1") == 4);
    std.debug.assert(@sizeOf(Rect) == 8);
    std.debug.assert(@offsetOf(PrefetchReq, "view") == 4);
    std.debug.assert(@offsetOf(PrefetchReq, "tile_cols") == 12);
    std.debug.assert(@offsetOf(PrefetchReq, "tile_rows") == 14);
    std.debug.assert(@offsetOf(PrefetchReq, "zoom") == 16);
    std.debug.assert(@offsetOf(PrefetchReq, "max_tiles") == 18);
    std.debug.assert(@offsetOf(PrefetchReq, "dir") == 20);
    std.debug.assert(@sizeOf(PrefetchReq) == 24);
}
