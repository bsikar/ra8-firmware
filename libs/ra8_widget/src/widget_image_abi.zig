//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI widget adapter for the shared greyscale image paint routine.

const std = @import("std");
const types = @import("widget_abi_types.zig");
const image = @import("widget_image.zig");

/// Scaling policy for an image widget.
pub const Scale = enum(u8) { fit = 0, fill = 1 };
/// Rectangle, paint and widget types shared with the tree ABI.
pub const Rect = types.Rect;
pub const Paint = types.Paint;
pub const Widget = types.Widget;
pub const Vtable = types.Vtable;
pub const err = types.err;

/// Caller-owned image descriptor for .
pub const ImageWidget = extern struct {
    paint: ?*const Paint,
    pixels: ?[*]const u8,
    width: u32,
    height: u32,
    scale: Scale,
    placeholder_fill: u8,
    placeholder_border: u8,
    reserved: u8,
    placeholder_border_width: i32,
};

const tag: [*:0]const u8 = "ra8_widget_image";

fn render(widget: *Widget) callconv(.c) void {
    const descriptor: *const ImageWidget = @ptrCast(@alignCast(widget.ctx orelse return));
    const backend = descriptor.paint orelse return;
    const pixels = descriptor.pixels;
    const pixel_count = std.math.mul(usize, descriptor.width, descriptor.height) catch 0;
    const bitmap: ?image.Bitmap = if (pixels) |data|
        .{ .pixels = data[0..pixel_count], .width = descriptor.width, .height = descriptor.height }
    else
        null;
    const scale: image.Scale = if (descriptor.scale == .fill) .fill else .fit;
    image.render(backend, widget.rect, bitmap, scale, .{
        .fill = descriptor.placeholder_fill,
        .border = descriptor.placeholder_border,
        .border_width = descriptor.placeholder_border_width,
    });
}

const vtable: Vtable = .{ .measure = null, .render = render, .on_input = null };

/// Shared vtable backing every image widget.
pub export fn ra8_widget_image_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

/// Bind  to a decoded greyscale bitmap or placeholder.
pub export fn ra8_widget_image_init(widget: ?*Widget, descriptor: ?*ImageWidget) callconv(.c) u16 {
    const instance = widget orelse return types.refuseNull(tag, "widget must not be nullptr");
    const image_descriptor = descriptor orelse return types.refuseNull(tag, "image must not be nullptr");
    if (image_descriptor.scale != .fit and image_descriptor.scale != .fill) return types.err.invalid_arg;
    instance.vt = &vtable;
    instance.ctx = image_descriptor;
    instance.visible = true;
    return types.err.ok;
}

comptime {
    const ptr = @sizeOf(usize);
    if (@offsetOf(ImageWidget, "paint") != 0) @compileError("ra8_widget_image_t paint offset");
    if (@offsetOf(ImageWidget, "pixels") != ptr) @compileError("ra8_widget_image_t pixels offset");
    if (@offsetOf(ImageWidget, "width") != 2 * ptr) @compileError("ra8_widget_image_t width offset");
    if (@offsetOf(ImageWidget, "height") != 2 * ptr + 4) @compileError("ra8_widget_image_t height offset");
    if (@offsetOf(ImageWidget, "scale") != 2 * ptr + 8) @compileError("ra8_widget_image_t scale offset");
    if (@offsetOf(ImageWidget, "placeholder_fill") != 2 * ptr + 9) @compileError("ra8_widget_image_t placeholder fill offset");
    if (@offsetOf(ImageWidget, "placeholder_border") != 2 * ptr + 10) @compileError("ra8_widget_image_t placeholder border offset");
    if (@offsetOf(ImageWidget, "reserved") != 2 * ptr + 11) @compileError("ra8_widget_image_t reserved offset");
    if (@offsetOf(ImageWidget, "placeholder_border_width") != 2 * ptr + 12) @compileError("ra8_widget_image_t border width offset");
    if (@sizeOf(Scale) != 1) @compileError("ra8_widget_image_scale_t width");
}
