//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure paint geometry shared by the concrete leaf widgets: where a string's
//! pen goes inside a rect, and how wide a proportional fill is. No callback
//! dispatch and no C types live here, so every rule below is exercised by
//! plain Zig tests.

/// Horizontal placement of a leaf widget's text within its rect.
pub const Alignment = enum(u8) {
    left = 0,
    center = 1,
    right = 2,
};

/// Axis-aligned rectangle, matching the published `ra8_ui_rect_t`.
pub const Rect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

/// A text pen position: the top-left of the first glyph.
pub const Pen = struct {
    x: i32,
    y: i32,
};

pub const geometry = struct {
    /// Filled width of a bar that is empty, or whose rect is degenerate.
    pub const empty_fill: i32 = 0;
};

/// Pen for text hugging the rect's inner inset, the placement every other
/// alignment starts from and the one used whenever the width is unmeasurable.
pub fn insetPen(rect: Rect, pad: i16) Pen {
    return .{ .x = rect.x + pad, .y = rect.y + pad };
}

/// Pen x that centres a `text_w`-wide string horizontally in `rect`.
pub fn centeredX(rect: Rect, text_w: i32) i32 {
    return rect.x + @divTrunc(rect.w - text_w, 2);
}

/// Pen x that hugs the rect's right inner inset.
pub fn rightX(rect: Rect, pad: i16, text_w: i32) i32 {
    return (rect.x + rect.w) - pad - text_w;
}

/// Pen y that centres a `text_h`-tall glyph cell vertically in `rect`.
pub fn centeredY(rect: Rect, text_h: i32) i32 {
    return rect.y + @divTrunc(rect.h - text_h, 2);
}

/// Pen for a measured string: vertically centred, horizontally per `alignment`.
/// `left` keeps the inner inset, so a measured left-aligned string is placed
/// exactly where an unmeasurable one would be on the x axis.
pub fn measuredPen(rect: Rect, pad: i16, alignment: Alignment, text_w: i32, text_h: i32) Pen {
    return .{
        .x = switch (alignment) {
            .left => rect.x + pad,
            .center => centeredX(rect, text_w),
            .right => rightX(rect, pad, text_w),
        },
        .y = centeredY(rect, text_h),
    };
}

/// Filled width in pixels for `value / total` spanning `width`, clamped to
/// `[0, width]`. A zero `total` or a non-positive `width` fills nothing; a
/// `value` at or above `total` fills the whole width.
pub fn fillFrac(value: u32, total: u32, width: i32) i32 {
    if (total == 0) return geometry.empty_fill;
    if (width <= geometry.empty_fill) return geometry.empty_fill;
    const clamped: u32 = @min(value, total);
    return @intCast((@as(u32, @intCast(width)) * clamped) / total);
}
