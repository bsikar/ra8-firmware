//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the text-label leaf widget: what used to be
//! `src/ra8_widget_label.c`. It owns the label descriptor, the one immutable
//! label vtable, and the two published entry points. The widget tree's shared
//! types live in `widget_abi_types.zig` and the geometry in `internal/paint.zig`,
//! so this file is binding plus dispatch and nothing else.
//!
//! Guard order, log lines and error codes are the C's, byte for byte.

const std = @import("std");
const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");
const text_layout = @import("internal/text_layout.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = types.Rect;
/// Alignment selector of the published ABI (`ra8_widget_align_t`).
pub const Alignment = types.Alignment;
/// Draw backend of the published ABI (`ra8_widget_paint_t`).
pub const Paint = types.Paint;
/// Widget instance of the published ABI (`ra8_widget_t`).
pub const Widget = types.Widget;
/// Behaviour table of the published ABI (`ra8_widget_vtable_t`).
pub const Vtable = types.Vtable;
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;
/// Label overflow behavior. Zero preserves the original one-line rendering.
pub const WrapMode = text_layout.Mode;

/// A plain label paints exactly one fill, so it asks the shared box helper for
/// no frame at all. The bordered face belongs to the button, not here.
pub const geometry = struct {
    pub const no_border: i16 = 0;
};

/// The C names this literal `s_tag`.
const tag: [*:0]const u8 = "ra8_widget_label";

/// Label descriptor of the published ABI (`ra8_widget_label_t`). Caller-owned
/// plain data: a null `paint` draws nothing and a null `text` fills only.
pub const Label = extern struct {
    paint: ?*const Paint,
    text: ?[*:0]const u8,
    fg: u32,
    bg: u32,
    pad: i16,
    alignment: Alignment,
    face: paint_abi.Face,
    weight: paint_abi.Weight = .regular,
    size: paint_abi.TextSize = .default,
    wrap: WrapMode = .none,
};

comptime {
    const ptr = @sizeOf(usize);

    if (@alignOf(Label) != @alignOf(usize)) @compileError("ra8_widget_label_t alignment");
    if (@offsetOf(Label, "paint") != 0) @compileError("ra8_widget_label_t paint offset");
    if (@offsetOf(Label, "text") != ptr) @compileError("ra8_widget_label_t text offset");
    if (@offsetOf(Label, "fg") != 2 * ptr) @compileError("ra8_widget_label_t fg offset");
    if (@offsetOf(Label, "bg") != 2 * ptr + 4) @compileError("ra8_widget_label_t bg offset");
    if (@offsetOf(Label, "pad") != 2 * ptr + 8) @compileError("ra8_widget_label_t pad offset");
    if (@offsetOf(Label, "alignment") != 2 * ptr + 10) @compileError("ra8_widget_label_t align offset");
    if (@offsetOf(Label, "face") != 2 * ptr + 11) @compileError("ra8_widget_label_t face offset");
    if (@offsetOf(Label, "weight") != 2 * ptr + 12) @compileError("ra8_widget_label_t weight offset");
    if (@offsetOf(Label, "size") != 2 * ptr + 13) @compileError("ra8_widget_label_t size offset");
    if (@offsetOf(Label, "wrap") != 2 * ptr + 14) @compileError("ra8_widget_label_t wrap offset");
}

/// Vtable `render`: fill the background, then draw the aligned text.
///
/// Every guard is a single condition, so a label with no descriptor, no paint
/// backend, no text or no `draw_text` is a no-op in exactly the place the C
/// returned, and the background fill still lands whenever there is a backend.
fn renderLabel(w: *Widget) callconv(.c) void {
    const label: *const Label = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = label.paint orelse return;

    paint_abi.priv_widget_fill_box(backend, &w.rect, label.bg, label.bg, geometry.no_border);

    const text = label.text orelse return;
    const weight_draw = backend.draw_text_style;
    const styled_draw = backend.draw_text_face;
    const legacy_draw = backend.draw_text;
    if (weight_draw == null and styled_draw == null and legacy_draw == null) return;

    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    const size = normalizedSize(label.size);
    if (label.wrap != .none) {
        renderWrapped(backend, &w.rect, text, label, size);
        return;
    }
    paint_abi.priv_widget_text_pos(
        backend,
        &w.rect,
        text,
        label.pad,
        label.alignment,
        label.face,
        label.weight,
        size,
        weight_draw != null or styled_draw != null,
        &pen_x,
        &pen_y,
    );
    if (weight_draw) |draw| {
        draw(backend.user, pen_x, pen_y, text, @intFromEnum(label.face), @intFromEnum(label.weight), @intFromEnum(size), label.fg, label.bg);
    } else if (styled_draw) |draw| {
        draw(backend.user, pen_x, pen_y, text, @intFromEnum(label.face), label.fg, label.bg);
    } else if (legacy_draw) |draw| {
        draw(backend.user, pen_x, pen_y, text, label.fg, label.bg);
    }
}

const MeasureContext = struct {
    backend: *const Paint,
    label: *const Label,
    size: paint_abi.TextSize,
};

fn renderWrapped(backend: *const Paint, rect: *const Rect, text: [*:0]const u8, label: *const Label, size: paint_abi.TextSize) void {
    const bytes = std.mem.span(text);
    if (bytes.len == 0) return;
    const inner_width = @max(rect.w - 2 * @as(i32, label.pad), 0);
    const inner_height = @max(rect.h - 2 * @as(i32, label.pad), 0);
    if (inner_width == 0 or inner_height == 0) return;
    const context = MeasureContext{ .backend = backend, .label = label, .size = size };
    const line_height = @max(measureBytes(context, "M").height, 1);
    const max_width: u32 = @intCast(inner_width);
    const max_lines: usize = @intCast(@divTrunc(inner_height, @as(i32, @intCast(line_height))));
    if (max_lines == 0) return;
    const line_count = countLines(bytes, max_width, label.wrap, context);
    const visible = @min(max_lines, line_count);
    const block_height: i32 = @intCast(visible * line_height);
    var y = rect.y + label.pad + @divTrunc(inner_height - block_height, 2);
    var line_buffer: [4097]u8 = undefined;
    var offset: usize = 0;
    var index: usize = 0;
    while (offset < bytes.len and index < visible) : (index += 1) {
        const line = text_layout.nextLine(bytes, offset, max_width, label.wrap, context, measureScalar);
        const width = line.width;
        var add_vertical_ellipsis = label.wrap == .clip and index + 1 == visible and line.next < bytes.len and !line.ellipsis;
        const ellipsis_width = measureBytes(context, "\xe2\x80\xa6").width;
        if (add_vertical_ellipsis and width +| ellipsis_width > max_width) add_vertical_ellipsis = false;
        const draw_width = width +| @as(u32, if (add_vertical_ellipsis) ellipsis_width else 0);
        const x = switch (label.alignment) {
            .left => rect.x + label.pad,
            .center => rect.x + @divTrunc(rect.w - @as(i32, @intCast(draw_width)), 2),
            .right => rect.x + rect.w - label.pad - @as(i32, @intCast(draw_width)),
        };
        drawLine(context, bytes, line, x, y, &line_buffer);
        if (add_vertical_ellipsis) drawScalar(backend, label, size, x + @as(i32, @intCast(width)), y, "\xe2\x80\xa6");
        y += @intCast(line_height);
        offset = line.next;
        if (offset <= line.start) break;
    }
}

fn countLines(bytes: []const u8, max_width: u32, mode: WrapMode, context: MeasureContext) usize {
    var offset: usize = 0;
    var count: usize = 0;
    while (offset < bytes.len) {
        const line = text_layout.nextLine(bytes, offset, max_width, mode, context, measureScalar);
        count += 1;
        if (line.next <= offset) break;
        offset = line.next;
    }
    return count;
}

fn measureScalar(context: MeasureContext, bytes: []const u8) u32 {
    return measureBytes(context, bytes).width;
}

const Extent = struct { width: u32, height: u32 };

fn measureBytes(context: MeasureContext, bytes: []const u8) Extent {
    var terminated: [5:0]u8 = undefined;
    @memcpy(terminated[0..bytes.len], bytes);
    terminated[bytes.len] = 0;
    const text: [*:0]const u8 = @ptrCast(&terminated);
    var width: i32 = 0;
    var height: i32 = 0;
    const backend = context.backend;
    const label = context.label;
    if (backend.text_size_style) |measure| {
        measure(backend.user, text, @intFromEnum(label.face), @intFromEnum(label.weight), @intFromEnum(context.size), &width, &height);
    } else if (backend.text_size_face) |measure| {
        measure(backend.user, text, @intFromEnum(label.face), &width, &height);
    } else if (backend.text_size) |measure| {
        measure(backend.user, text, &width, &height);
    } else {
        width = @intCast(bytes.len * 8);
        height = 16;
    }
    return .{ .width = @intCast(@max(width, 0)), .height = @intCast(@max(height, 0)) };
}

fn drawLine(context: MeasureContext, bytes: []const u8, line: text_layout.Line, x: i32, y: i32, storage: *[4097]u8) void {
    var length = @min(line.end - line.start, storage.len - 1);
    @memcpy(storage[0..length], bytes[line.start .. line.start + length]);
    if (line.ellipsis and length + 3 < storage.len) {
        @memcpy(storage[length .. length + 3], "\xe2\x80\xa6");
        length += 3;
    }
    storage[length] = 0;
    drawRun(context.backend, context.label, context.size, x, y, @ptrCast(storage));
}

fn drawRun(backend: *const Paint, label: *const Label, size: paint_abi.TextSize, x: i32, y: i32, text: [*:0]const u8) void {
    if (backend.draw_text_style) |draw| {
        draw(backend.user, x, y, text, @intFromEnum(label.face), @intFromEnum(label.weight), @intFromEnum(size), label.fg, label.bg);
    } else if (backend.draw_text_face) |draw| {
        draw(backend.user, x, y, text, @intFromEnum(label.face), label.fg, label.bg);
    } else if (backend.draw_text) |draw| {
        draw(backend.user, x, y, text, label.fg, label.bg);
    }
}

fn drawScalar(backend: *const Paint, label: *const Label, size: paint_abi.TextSize, x: i32, y: i32, bytes: []const u8) void {
    var terminated: [5:0]u8 = undefined;
    @memcpy(terminated[0..bytes.len], bytes);
    terminated[bytes.len] = 0;
    drawRun(backend, label, size, x, y, @ptrCast(&terminated));
}

fn normalizedSize(size: paint_abi.TextSize) paint_abi.TextSize {
    return switch (@intFromEnum(size)) {
        0 => .size_3,
        1 => .size_1,
        2 => .size_2,
        3 => .size_3,
        4 => .size_4,
        5 => .size_5,
        6 => .body_38,
        7 => .title_68,
        8 => .clock_120,
        9 => .ui_26,
        10 => .ui_30,
        else => .size_3,
    };
}

/// The single immutable vtable shared by every text label: display only, so
/// it measures nothing and never consumes a touch.
const label_vtable: Vtable = .{
    .measure = null,
    .render = renderLabel,
    .on_input = null,
};

/// `ra8_widget_label_vtable`: the shared label vtable, in static storage.
pub export fn ra8_widget_label_vtable() callconv(.c) *const Vtable {
    return &label_vtable;
}

/// `ra8_widget_label_init`: bind `w` to render `label`. The caller still sets
/// `w`'s `fixed` / `flex` for its parent's layout.
pub export fn ra8_widget_label_init(w: ?*Widget, label: ?*Label) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = label orelse return types.refuseNull(tag, "label must not be nullptr");

    widget.vt = ra8_widget_label_vtable();
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}
