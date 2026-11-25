//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Integer scanline mask rasterizers for translated speech and thought bubble
//! backgrounds (`rect`, `rounded_rect`, `ellipse`, and `cloud`) using caller
//! `fill_rect` primitives with zero heap allocation or floating-point math.

const std = @import("std");
const sidecar = @import("sidecar");

/// Rectangle callback matching `ra8_widget_paint_t.fill_rect`.
pub const FillFn = *const fn (?*anyopaque, i32, i32, i32, i32, u32) callconv(.c) void;

const MaskBox = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

/// Fill `shape` mask strictly bounded within `[x..x+w, y..y+h]`.
pub fn fillShapeMask(
    user: ?*anyopaque,
    fill_opt: ?FillFn,
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    shape: sidecar.Shape,
    corner_radius: u16,
    color: u32,
) void {
    const fill = fill_opt orelse return;
    if (w <= 0 or h <= 0) return;
    const box = MaskBox{ .x = x, .y = y, .w = w, .h = h };
    switch (shape) {
        .rect => fill(user, box.x, box.y, box.w, box.h, color),
        .rounded_rect => fillRoundedRect(user, fill, box, corner_radius, color),
        .ellipse => fillEllipse(user, fill, box, color),
        .cloud => fillCloud(user, fill, box, color),
    }
}

fn fillRoundedRect(
    user: ?*anyopaque,
    fill: FillFn,
    rect: MaskBox,
    corner_radius: u16,
    color: u32,
) void {
    const r = @min(@as(i32, corner_radius), @min(@divFloor(rect.w, 2), @divFloor(rect.h, 2)));
    if (r <= 0) {
        fill(user, rect.x, rect.y, rect.w, rect.h, color);
        return;
    }
    const mid_h = rect.h - 2 * r;
    if (mid_h > 0) fill(user, rect.x, rect.y + r, rect.w, mid_h, color);

    const diam: u64 = @intCast(r * 2);
    const limit: u64 = diam * diam;
    var row: i32 = 0;
    while (row < r) : (row += 1) {
        const dy: u64 = @intCast((r - row) * 2 - 1);
        const rem = limit -| (dy * dy);
        const half2: i32 = @intCast(std.math.sqrt(rem));
        const inset = @max(@divFloor(r * 2 - 1 - half2 + 1, 2), 0);
        const span_w = rect.w - 2 * inset;
        if (span_w > 0) {
            fill(user, rect.x + inset, rect.y + row, span_w, 1, color);
            fill(user, rect.x + inset, rect.y + rect.h - 1 - row, span_w, 1, color);
        }
    }
}

fn fillEllipse(
    user: ?*anyopaque,
    fill: FillFn,
    rect: MaskBox,
    color: u32,
) void {
    const w64: u64 = @intCast(rect.w);
    const h64: u64 = @intCast(rect.h);
    const w2: u64 = w64 * w64;
    const h2: u64 = h64 * h64;
    const limit: u64 = w2 * h2;
    var row: i32 = 0;
    while (row < rect.h) : (row += 1) {
        const y_signed: i64 = @as(i64, row) * 2 + 1 - @as(i64, rect.h);
        const y2: u64 = @intCast(y_signed * y_signed);
        const term = y2 * w2;
        if (term > limit) continue;
        const max_x_term = (limit - term) / h2;
        const half2: u64 = std.math.sqrt(max_x_term);
        const x0 = @as(i64, rect.w) - 1 - @as(i64, @intCast(half2));
        const left: i32 = @intCast(@max(@divFloor(x0 + 1, 2), 0));
        const right: i32 = @min(rect.w - left, rect.w);
        if (right > left) fill(user, rect.x + left, rect.y + row, right - left, 1, color);
    }
}

fn fillCloud(
    user: ?*anyopaque,
    fill: FillFn,
    rect: MaskBox,
    color: u32,
) void {
    const w: i64 = rect.w;
    const h: i64 = rect.h;
    const core = MaskBox{
        .x = rect.x + @as(i32, @intCast(@divFloor(w, 10))),
        .y = rect.y + @as(i32, @intCast(@divFloor(h, 10))),
        .w = @max(rect.w - 2 * @as(i32, @intCast(@divFloor(w, 10))), 1),
        .h = @max(rect.h - 2 * @as(i32, @intCast(@divFloor(h, 10))), 1),
    };
    fillEllipse(user, fill, core, color);

    const lobe_rx = @max(@divFloor(rect.w, 6), 1);
    const lobe_ry = @max(@divFloor(rect.h, 6), 1);
    const anchors = [_]struct { x: i32, y: i32 }{
        .{ .x = @divFloor(rect.w, 4), .y = lobe_ry },
        .{ .x = @divFloor(rect.w * 3, 4), .y = lobe_ry },
        .{ .x = lobe_rx, .y = @divFloor(rect.h, 2) },
        .{ .x = rect.w - lobe_rx, .y = @divFloor(rect.h, 2) },
        .{ .x = @divFloor(rect.w, 4), .y = rect.h - lobe_ry },
        .{ .x = @divFloor(rect.w * 3, 4), .y = rect.h - lobe_ry },
    };
    for (anchors) |a| {
        const lw = @min(lobe_rx * 2, rect.w);
        const lh = @min(lobe_ry * 2, rect.h);
        const lx = std.math.clamp(rect.x + a.x - @divFloor(lw, 2), rect.x, rect.x + rect.w - lw);
        const ly = std.math.clamp(rect.y + a.y - @divFloor(lh, 2), rect.y, rect.y + rect.h - lh);
        fillEllipse(user, fill, .{ .x = lx, .y = ly, .w = lw, .h = lh }, color);
    }
}
