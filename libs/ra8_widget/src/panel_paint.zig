//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Generic Paint binder shared by `ra8_gfx` surfaces (`Surface` on desktop,
//! `EinkSurface` on chip/panel). Maps fills, lines, blits, clips, text draw/size
//! callbacks, and per-canvas `Capabilities` cleanly with zero duplication.

const std = @import("std");
const gfx = @import("ra8_gfx");

const text = gfx.text;

fn face(value: u8) text.Face {
    return if (value == 1) .serif else .sans;
}

fn weight(value: u8) text.Weight {
    return if (value == 1) .bold else .regular;
}

fn capsType(comptime Paint: type) type {
    const field = @typeInfo(@FieldType(Paint, "caps")).optional.child;
    return @typeInfo(field).pointer.child;
}

fn makeCaps(comptime Caps: type, comptime source: gfx.Capabilities) Caps {
    return .{
        .pixel_format = @fromBackingInt(@intCast(@backingInt(source.pixel_format))),
        .color = @intFromBool(source.color),
        .alpha = @intFromBool(source.alpha),
        .gray4 = @intFromBool(source.gray4),
        .dither = @intFromBool(source.dither),
        .line = @intFromBool(source.line),
        .blit = @intFromBool(source.blit),
        .clip = @intFromBool(source.clip),
    };
}

/// Bind a `Paint` vtable of type `Paint` directly to `target` (`*gfx.Surface`
/// or `*gfx.EinkSurface`) with its declared capabilities.
pub fn table(comptime Target: type, comptime Paint: type, target: *Target, comptime declared_caps: gfx.Capabilities) Paint {
    const callbacks = struct {
        fn targetOf(user: ?*anyopaque) ?*Target {
            return @ptrCast(@alignCast(user orelse return null));
        }

        fn fillRect(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.fill(.{ .x = x, .y = y, .w = w, .h = h }, color);
        }

        fn fillRectAlpha(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32, alpha: u8) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.fillAlpha(.{ .x = x, .y = y, .w = w, .h = h }, color, alpha);
        }

        fn drawLine(user: ?*anyopaque, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.line(x0, y0, x1, y1, color);
        }

        fn blit(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, pixels: [*]const u8, stride: i32, format: u8) callconv(.c) void {
            const c = targetOf(user) orelse return;
            if (w <= 0 or h <= 0 or stride <= 0) return;
            const rows: usize = @intCast(h);
            const pitch: usize = @intCast(stride);
            if (format == 0) {
                if (comptime @hasDecl(Target, "blitGray4")) {
                    const row_bytes = (@as(usize, @intCast(w)) + 1) / 2;
                    const len = (rows - 1) * pitch + row_bytes;
                    c.blitGray4(.{ .x = x, .y = y, .w = w, .h = h }, pixels[0..len], pitch);
                }
                return;
            }
            const kind = std.enums.fromInt(gfx.Format, format) orelse return;
            const len = (rows - 1) * pitch + @as(usize, @intCast(w)) * kind.bytes();
            c.blit(.{ .x = x, .y = y, .w = w, .h = h }, pixels[0..len], pitch, kind);
        }

        fn pushClip(user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.pushClip(.{ .x = x, .y = y, .w = w, .h = h });
        }

        fn popClip(user: ?*anyopaque) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.popClip();
        }

        fn drawText(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, fg: u32, bg: u32) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.drawText(str, x, y, fg, bg);
        }

        fn drawTextFace(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, family: u8, fg: u32, bg: u32) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.drawTextFace(str, x, y, face(family), fg, bg);
        }

        fn drawTextStyle(user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, family: u8, stroke: u8, size: u8, fg: u32, bg: u32) callconv(.c) void {
            const c = targetOf(user) orelse return;
            c.drawTextStyle(str, x, y, face(family), weight(stroke), size, fg, bg);
        }

        fn store(extent: text.Extent, out_w: *i32, out_h: *i32) void {
            out_w.* = @intCast(extent.width);
            out_h.* = @intCast(extent.height);
        }

        fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
            store(Target.measureText(str, .sans), out_w, out_h);
        }

        fn textSizeFace(_: ?*anyopaque, str: [*:0]const u8, family: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
            store(Target.measureText(str, face(family)), out_w, out_h);
        }

        fn textSizeStyle(_: ?*anyopaque, str: [*:0]const u8, family: u8, stroke: u8, size: u8, out_w: *i32, out_h: *i32) callconv(.c) void {
            store(Target.measureTextStyle(str, face(family), weight(stroke), size), out_w, out_h);
        }
    };

    const Caps = capsType(Paint);
    const caps_holder = struct {
        const value: Caps = makeCaps(Caps, declared_caps);
    };

    return .{
        .user = target,
        .fill_rect = callbacks.fillRect,
        .draw_text = callbacks.drawText,
        .text_size = callbacks.textSize,
        .draw_text_face = callbacks.drawTextFace,
        .text_size_face = callbacks.textSizeFace,
        .draw_text_style = callbacks.drawTextStyle,
        .text_size_style = callbacks.textSizeStyle,
        .draw_line = if (declared_caps.line) callbacks.drawLine else null,
        .blit = if (declared_caps.blit) callbacks.blit else null,
        .push_clip = if (declared_caps.clip) callbacks.pushClip else null,
        .pop_clip = if (declared_caps.clip) callbacks.popClip else null,
        .fill_rect_alpha = if (declared_caps.alpha) callbacks.fillRectAlpha else null,
        .caps = &caps_holder.value,
    };
}
