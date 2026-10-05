//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Face-aware text metrics and rasterisation shared by the ra8_gfx entry
//! points. Sans remains the bundled fixed cell face; Literata uses the
//! checked-in 2-bit atlas generated from the licensed Latin-1 subset.

const std = @import("std");
const atlas = @import("font_literata.zig");
const bitmap = @import("font_8x16.zig");
const display_atlas = @import("font_display.zig");

/// Font family selected by a text rendering call.
pub const Face = enum(u8) {
    sans = 0,
    serif = 1,
};

/// Stroke weight selected independently from the text family.
pub const Weight = enum(u8) {
    regular = 0,
    bold = 1,
};

/// Legacy IBM font dimensions retained for sans measurement.
pub const sans = struct {
    pub const width: u8 = 8;
    pub const height: u8 = 16;
};

/// Pixel output callback used by the pure atlas renderer.
pub const PixelFn = *const fn (user: ?*anyopaque, x: i32, y: i32, color: u32) callconv(.c) void;

/// Measured dimensions of a single-line text run.
pub const Extent = struct {
    width: u32,
    height: u32,
};

const max_bytes: usize = 4096;
const replacement_codepoint: u32 = 0x3F;
const mask_byte: u32 = 0xFF;
const coverage_levels = [_]u32{ 0, 85, 170, 255 };
const rgba_shifts = [_]u5{ 16, 8, 0 };

/// Measure a NUL-terminated run. Invalid or unsupported Unicode scalars each
/// occupy the replacement glyph, matching the draw path exactly.
pub fn measure(text: [*:0]const u8, face: Face) Extent {
    return measureWeight(text, face, .regular);
}

/// Measure the visible bounds of a run at the selected weight. Bold expands
/// the final visible edge by one pixel, matching the rasterizer.
pub fn measureWeight(text: [*:0]const u8, face: Face, weight: Weight) Extent {
    if (face == .sans) {
        const bytes = byteLength(text);
        const width = @as(u32, @intCast(bytes)) * sans.width;
        return .{ .width = width + @intFromBool(weight == .bold and bytes > 0), .height = sans.height };
    }

    var extent = Extent{ .width = 0, .height = @as(u32, atlas.ascent + atlas.descent) };
    var offset: usize = 0;
    while (offset < max_bytes and text[offset] != 0) {
        const decoded = decode(text, offset);
        const glyph = lookup(decoded.codepoint);
        extent.width +|= @intCast(glyph.advance);
        offset += decoded.byte_count;
    }
    if (weight == .bold and extent.width > 0) extent.width += 1;
    return extent;
}

/// Draw the bundled 8x16 sans face into an injected pixel sink.
pub fn drawSans(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    drawSansWeight(text, x, y, fg, bg, .regular, user, put_pixel);
}

/// Draw the IBM bitmap face and expand bold strokes by one pixel rightward.
pub fn drawSansWeight(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    fg: u32,
    bg: u32,
    weight: Weight,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    var offset: usize = 0;
    while (offset < max_bytes and text[offset] != 0) : (offset += 1) {
        const glyph = bitmap.glyph(text[offset]) orelse bitmap.glyph(0x3F).?;
        for (glyph, 0..) |row_bits, row| {
            for (0..bitmap.geometry.width) |col| {
                const on = row_bits & (@as(u8, 1) << @intCast(7 - col)) != 0;
                put_pixel(user, x + @as(i32, @intCast(offset * bitmap.geometry.width + col)), y + @as(i32, @intCast(row)), if (on) fg else bg);
            }
        }
    }

    if (weight == .bold) {
        offset = 0;
        while (offset < max_bytes and text[offset] != 0) : (offset += 1) {
            const glyph = bitmap.glyph(text[offset]) orelse bitmap.glyph(0x3F).?;
            for (glyph, 0..) |row_bits, row| {
                for (0..bitmap.geometry.width) |col| {
                    const on = row_bits & (@as(u8, 1) << @intCast(7 - col)) != 0;
                    if (on) {
                        put_pixel(
                            user,
                            x + @as(i32, @intCast(offset * bitmap.geometry.width + col + 1)),
                            y + @as(i32, @intCast(row)),
                            fg,
                        );
                    }
                }
            }
        }
    }
}

/// Draw a Literata run into an injected pixel sink. Background fills the full
/// advance cell; glyph coverage blends foreground and background in 2-bit steps.
pub fn drawSerif(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    drawSerifWeight(text, x, y, fg, bg, .regular, user, put_pixel);
}

/// Draw a Literata run with a one-pixel rightward stroke expansion for bold.
pub fn drawSerifWeight(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    fg: u32,
    bg: u32,
    weight: Weight,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    const line_height: i32 = atlas.ascent + atlas.descent;
    var pen_x = x;
    var offset: usize = 0;
    while (offset < max_bytes and text[offset] != 0) {
        const decoded = decode(text, offset);
        const glyph = lookup(decoded.codepoint);
        paintBackground(pen_x, y, glyph.advance, line_height, bg, user, put_pixel);
        if (weight == .bold) paintBackground(pen_x +% glyph.advance, y, 1, line_height, bg, user, put_pixel);
        pen_x +%= glyph.advance;
        offset += decoded.byte_count;
    }

    pen_x = x;
    offset = 0;
    while (offset < max_bytes and text[offset] != 0) {
        const decoded = decode(text, offset);
        const glyph = lookup(decoded.codepoint);
        paintGlyph(pen_x, y, glyph, fg, bg, user, put_pixel, false);
        if (weight == .bold) paintGlyph(pen_x, y, glyph, fg, bg, user, put_pixel, true);
        pen_x +%= glyph.advance;
        offset += decoded.byte_count;
    }
}

/// Measure one of the five reading sizes using the matching face metrics.
pub fn measureScaled(text: [*:0]const u8, face: Face, size: u8) Extent {
    return measureScaledWeight(text, face, .regular, size);
}

/// Measure a face and stroke weight at the selected reading size.
pub fn measureScaledWeight(text: [*:0]const u8, face: Face, weight: Weight, size: u8) Extent {
    const numerator: u32 = @intCast(scaleNumerator(size));
    const base = measureWeight(text, face, weight);
    return .{
        .width = @intCast((@as(u64, base.width) * numerator + 5) / 6),
        .height = @intCast((@as(u64, base.height) * numerator + 5) / 6),
    };
}

/// Measure a complete face, weight and size selection for widget style callbacks.
pub fn measureStyle(text: [*:0]const u8, face: Face, weight: Weight, size: u8) Extent {
    if (display_atlas.get(@intFromEnum(face), @intFromEnum(weight), size)) |selected| {
        return measureDisplay(text, selected);
    }
    return measureScaledWeight(text, face, weight, size);
}

fn measureDisplay(text: [*:0]const u8, selected: display_atlas.Atlas) Extent {
    var extent = Extent{ .width = 0, .height = @intCast(selected.ascent + selected.descent) };
    var offset: usize = 0;
    while (offset < max_bytes and text[offset] != 0) {
        const decoded = decode(text, offset);
        extent.width +|= @intCast(lookupDisplay(selected, decoded.codepoint).advance);
        offset += decoded.byte_count;
    }
    return extent;
}

/// Draw face, weight and size selections without stretching the reading atlas.
pub fn drawStyle(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    face: Face,
    weight: Weight,
    size: u8,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    if (display_atlas.get(@intFromEnum(face), @intFromEnum(weight), size)) |selected| {
        drawDisplay(text, x, y, selected, fg, bg, user, put_pixel);
        return;
    }
    drawScaledWeight(text, x, y, face, weight, size, fg, bg, user, put_pixel);
}

fn drawDisplay(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    selected: display_atlas.Atlas,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    const line_height = selected.ascent + selected.descent;
    var pen_x = x;
    var offset: usize = 0;
    while (offset < max_bytes and text[offset] != 0) {
        const decoded = decode(text, offset);
        const glyph = lookupDisplay(selected, decoded.codepoint);
        paintBackground(pen_x, y, glyph.advance, line_height, bg, user, put_pixel);
        pen_x +%= glyph.advance;
        offset += decoded.byte_count;
    }

    pen_x = x;
    offset = 0;
    while (offset < max_bytes and text[offset] != 0) {
        const decoded = decode(text, offset);
        const glyph = lookupDisplay(selected, decoded.codepoint);
        paintDisplayGlyph(pen_x, y, selected, glyph, fg, bg, user, put_pixel);
        pen_x +%= glyph.advance;
        offset += decoded.byte_count;
    }
}

fn lookupDisplay(selected: display_atlas.Atlas, codepoint: u32) display_atlas.Glyph {
    return lookupDisplayExact(selected, codepoint) orelse lookupDisplayExact(selected, replacement_codepoint).?;
}

fn lookupDisplayExact(selected: display_atlas.Atlas, codepoint: u32) ?display_atlas.Glyph {
    var low: usize = 0;
    var high: usize = selected.glyph_count;
    while (low < high) {
        const middle = low + (high - low) / 2;
        const current = selected.glyphAt(middle);
        if (current.codepoint < codepoint) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < selected.glyph_count and selected.glyphAt(low).codepoint == codepoint) return selected.glyphAt(low);
    return null;
}

fn paintDisplayGlyph(
    pen_x: i32,
    line_y: i32,
    selected: display_atlas.Atlas,
    glyph: display_atlas.Glyph,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    const top = line_y +% selected.ascent +% glyph.top;
    var row: usize = 0;
    while (row < glyph.height) : (row += 1) {
        var col: usize = 0;
        while (col < glyph.width) : (col += 1) {
            const index = glyph.offset + @as(u32, @intCast(row * glyph.width + col));
            const bits = selected.coverageByte(index);
            const shift: u3 = @intCast(6 - (index % 4) * 2);
            const alpha = coverage_levels[@intCast((bits >> shift) & 3)];
            if (alpha == 0) continue;
            put_pixel(
                user,
                pen_x +% glyph.left +% @as(i32, @intCast(col)),
                top +% @as(i32, @intCast(row)),
                blend(fg, bg, alpha),
            );
        }
    }
}

const ScaledSink = struct {
    user: ?*anyopaque,
    put_pixel: PixelFn,
    origin_x: i32,
    origin_y: i32,
    numerator: i32,
};

fn scaleCoordinate(value: i32, origin: i32, numerator: i32) i32 {
    return origin +% @divFloor((value -% origin) *% numerator, 6);
}

/// Map a source pixel to all output pixels covered by its scaled cell.
fn putScaled(user: ?*anyopaque, x: i32, y: i32, color: u32) callconv(.c) void {
    const sink: *const ScaledSink = @ptrCast(@alignCast(user orelse return));
    const x0 = scaleCoordinate(x, sink.origin_x, sink.numerator);
    const x1 = scaleCoordinate(x +% 1, sink.origin_x, sink.numerator);
    const y0 = scaleCoordinate(y, sink.origin_y, sink.numerator);
    const y1 = scaleCoordinate(y +% 1, sink.origin_y, sink.numerator);
    var py = y0;
    while (py < y1) : (py += 1) {
        var px = x0;
        while (px < x1) : (px += 1) sink.put_pixel(sink.user, px, py, color);
    }
}

/// Draw sans or Literata text at one of five sizes while preserving each face's
/// glyph shapes. Size three is the original face metric.
pub fn drawScaled(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    face: Face,
    size: u8,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    drawScaledWeight(text, x, y, face, .regular, size, fg, bg, user, put_pixel);
}

/// Draw a weighted face at one of five reading sizes.
pub fn drawScaledWeight(
    text: [*:0]const u8,
    x: i32,
    y: i32,
    face: Face,
    weight: Weight,
    size: u8,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    const sink = ScaledSink{
        .user = user,
        .put_pixel = put_pixel,
        .origin_x = x,
        .origin_y = y,
        .numerator = scaleNumerator(size),
    };
    const sink_ptr: ?*anyopaque = @ptrCast(@constCast(&sink));
    if (face == .sans) {
        drawSansWeight(text, x, y, fg, bg, weight, sink_ptr, putScaled);
    } else {
        drawSerifWeight(text, x, y, fg, bg, weight, sink_ptr, putScaled);
    }
}

/// Sizes 1-5 span two-thirds through four-thirds of the base face metrics.
fn scaleNumerator(size: u8) i32 {
    return switch (size) {
        0, 3 => 6,
        1 => 4,
        2 => 5,
        4 => 7,
        5 => 8,
        else => 6,
    };
}

/// Return the byte length capped at the existing ra8_gfx text walk limit.
fn byteLength(text: [*:0]const u8) usize {
    var length: usize = 0;
    while (length < max_bytes and text[length] != 0) : (length += 1) {}
    return length;
}

/// One decoded scalar and the number of input bytes consumed.
const Decoded = struct {
    codepoint: u32,
    byte_count: usize,
};

/// Decode one UTF-8 scalar. Ill-formed sequences consume one byte so a
/// truncated sequence cannot read beyond the terminating NUL.
fn decode(text: [*:0]const u8, offset: usize) Decoded {
    const first = text[offset];
    if (first < 0x80) return .{ .codepoint = first, .byte_count = 1 };

    const length: usize = if (first >= 0xC2 and first <= 0xDF) 2 else if (first >= 0xE0 and first <= 0xEF) 3 else if (first >= 0xF0 and first <= 0xF4) 4 else return replacement();
    var sequence: [4]u8 = .{ 0, 0, 0, 0 };
    sequence[0] = first;
    var i: usize = 1;
    while (i < length) : (i += 1) {
        const byte = text[offset + i];
        if (byte == 0 or byte & 0xC0 != 0x80) return replacement();
        sequence[i] = byte;
    }
    const cp = std.unicode.utf8Decode(sequence[0..length]) catch return replacement();
    return .{ .codepoint = cp, .byte_count = length };
}

/// U+FFFD is not in this atlas, so malformed input maps directly to '?'.
fn replacement() Decoded {
    return .{ .codepoint = replacement_codepoint, .byte_count = 1 };
}

/// Find a glyph in the small sorted atlas, falling back to '?' if absent.
fn lookup(codepoint: u32) atlas.Glyph {
    return lookupExact(codepoint) orelse lookupExact(replacement_codepoint).?;
}

/// Find an exact codepoint without applying replacement fallback.
fn lookupExact(codepoint: u32) ?atlas.Glyph {
    var low: usize = 0;
    var high: usize = atlas.glyph_count;
    while (low < high) {
        const middle = low + (high - low) / 2;
        const current = atlas.glyphAt(middle);
        if (current.codepoint < codepoint) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low < atlas.glyph_count and atlas.glyphAt(low).codepoint == codepoint) return atlas.glyphAt(low);
    return null;
}

/// Fill the advance cell with the requested background color.
fn paintBackground(
    x: i32,
    y: i32,
    width: i16,
    height: i32,
    color: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
) void {
    var row: i32 = 0;
    while (row < height) : (row += 1) {
        var col: i32 = 0;
        while (col < width) : (col += 1) put_pixel(user, x +% col, y +% row, color);
    }
}

/// Blend one packed atlas glyph over its background using shared metrics.
fn paintGlyph(
    pen_x: i32,
    line_y: i32,
    glyph: atlas.Glyph,
    fg: u32,
    bg: u32,
    user: ?*anyopaque,
    put_pixel: PixelFn,
    bold_expand: bool,
) void {
    const top = line_y +% atlas.ascent +% glyph.top;
    const output_width = @as(usize, glyph.width) + @intFromBool(bold_expand);
    var row: usize = 0;
    while (row < glyph.height) : (row += 1) {
        var col: usize = 0;
        while (col < output_width) : (col += 1) {
            const current = coverage(glyph, row, col);
            const previous = if (bold_expand and col > 0) coverage(glyph, row, col - 1) else 0;
            const level = @max(current, previous);
            if (level == 0) continue;
            const px = pen_x +% glyph.left +% @as(i32, @intCast(col));
            const py = top +% @as(i32, @intCast(row));
            put_pixel(user, px, py, blend(fg, bg, level));
        }
    }
}

/// Two-bit coverage at one glyph-local pixel, or zero outside its bitmap.
fn coverage(glyph: atlas.Glyph, row: usize, col: usize) u32 {
    if (col >= glyph.width) return 0;
    const index = glyph.offset + @as(u32, @intCast((row * glyph.width) + col));
    const packed_byte = atlas.coverageByte(index);
    const shift: u3 = @intCast(6 - (index % 4) * 2);
    return coverage_levels[@intCast((packed_byte >> shift) & 3)];
}

/// Interpolate 24-bit RGB channels for one 2-bit atlas coverage level.
fn blend(fg: u32, bg: u32, alpha: u32) u32 {
    var color: u32 = 0;
    for (rgba_shifts) |shift| {
        const foreground = (fg >> shift) & mask_byte;
        const background = (bg >> shift) & mask_byte;
        const channel = (foreground * alpha + background * (mask_byte - alpha) + 127) / mask_byte;
        color |= channel << shift;
    }
    return color;
}
