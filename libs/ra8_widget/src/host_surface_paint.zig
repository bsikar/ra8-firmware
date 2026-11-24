//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The widget Paint table bound to ra8_gfx's host argb8888 surface
//! (RA8FW-950), so desktop GUI apps render the same widgets the panel does
//! in full colour: fills, blended fills, lines, blits, the clip stack and
//! text in the panel's own fonts, with glyph coverage blended over what is
//! already drawn. `table` is generic over the Paint type so each importer's
//! own copy of the ABI struct takes it.

const gfx = @import("ra8_gfx");

const Surface = gfx.Surface;
const text = gfx.text;

fn surfaceOf(user: ?*anyopaque) ?*Surface {
    return @ptrCast(@alignCast(user orelse return null));
}

fn fillRect(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.fill(.{ .x = x, .y = y, .w = w, .h = h }, color);
}

fn fillRectAlpha(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32, alpha: u8) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.fillAlpha(.{ .x = x, .y = y, .w = w, .h = h }, color, alpha);
}

fn drawLine(user: ?*anyopaque, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.line(x0, y0, x1, y1, color);
}

fn blit(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, pixels: [*]const u8, stride: i32, format: u8) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    if (w <= 0 or h <= 0 or stride <= 0) return;
    const kind = std.enums.fromInt(gfx.Format, format) orelse return;
    const rows: usize = @intCast(h);
    const pitch: usize = @intCast(stride);
    const len = (rows - 1) * pitch + @as(usize, @intCast(w)) * kind.bytes();
    s.blit(.{ .x = x, .y = y, .w = w, .h = h }, pixels[0..len], pitch, kind);
}

fn pushClip(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.pushClip(.{ .x = x, .y = y, .w = w, .h = h });
}

fn popClip(user: ?*anyopaque) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.popClip();
}

fn face(value: u8) text.Face {
    return if (value == 1) .serif else .sans;
}

fn weight(value: u8) text.Weight {
    return if (value == 1) .bold else .regular;
}

fn drawText(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, fg: u32, bg: u32) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.drawText(str, x, y, fg, bg);
}

fn drawTextFace(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, family: u8, fg: u32, bg: u32) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.drawTextFace(str, x, y, face(family), fg, bg);
}

fn drawTextStyle(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, family: u8, stroke: u8, size: u8, fg: u32, bg: u32) callconv(.c) void {
    const s = surfaceOf(user) orelse return;
    s.drawTextStyle(str, x, y, face(family), weight(stroke), size, fg, bg);
}

fn store(extent: text.Extent, out_w: *i32, out_h: *i32) void {
    out_w.* = @intCast(extent.width);
    out_h.* = @intCast(extent.height);
}

fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    store(Surface.measureText(str, .sans), out_w, out_h);
}

fn textSizeFace(_: ?*anyopaque, str: [*:0]const u8, family: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    store(Surface.measureText(str, face(family)), out_w, out_h);
}

fn textSizeStyle(_: ?*anyopaque, str: [*:0]const u8, family: u8, stroke: u8, size: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    store(Surface.measureTextStyle(str, face(family), weight(stroke), size), out_w, out_h);
}

fn capsType(comptime Paint: type) type {
    const field = @typeInfo(@FieldType(Paint, "caps")).optional.child;
    return @typeInfo(field).pointer.child;
}

fn makeDesktopCaps(comptime Caps: type) Caps {
    const surface_caps = gfx.Capabilities.desktop_argb8888;
    return .{
        .pixel_format = @fromBackingInt(@intCast(@backingInt(surface_caps.pixel_format))),
        .color = @intFromBool(surface_caps.color),
        .alpha = @intFromBool(surface_caps.alpha),
        .gray4 = @intFromBool(surface_caps.gray4),
        .dither = @intFromBool(surface_caps.dither),
        .line = @intFromBool(surface_caps.line),
        .blit = @intFromBool(surface_caps.blit),
        .clip = @intFromBool(surface_caps.clip),
    };
}

/// A Paint table of type `Paint` drawing into `surface`, every member bound.
pub fn table(comptime Paint: type, surface: *Surface) Paint {
    const Caps = capsType(Paint);
    const desktop_caps = struct {
        const value: Caps = makeDesktopCaps(Caps);
    };
    return .{
        .user = surface,
        .fill_rect = fillRect,
        .draw_text = drawText,
        .text_size = textSize,
        .draw_text_face = drawTextFace,
        .text_size_face = textSizeFace,
        .draw_text_style = drawTextStyle,
        .text_size_style = textSizeStyle,
        .draw_line = drawLine,
        .blit = blit,
        .push_clip = pushClip,
        .pop_clip = popClip,
        .fill_rect_alpha = fillRectAlpha,
        .caps = &desktop_caps.value,
    };
}

const std = @import("std");
