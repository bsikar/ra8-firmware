//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Freestanding `TRN1` manga/webtoon translation overlay renderer and English
//! text fitter over `ra8_widget` `Paint` callbacks. Performs zero heap
//! allocations, fills speech/thought bubble masks clean, fits English text
//! across the `ra8_widget` / `ra8_gfx` font ladder (including tall narrow
//! vertical-Japanese bubbles), and emits minimal per-bubble damage rects.

const std = @import("std");
const err_mod = @import("err");
const sidecar = @import("sidecar");
const overlay_mask = @import("overlay_mask");

pub const Error = err_mod.TranslateError;

pub const max_lines: usize = 64;
pub const max_line_bytes: usize = 512;
pub const default_fg: u32 = 0x000000;
pub const default_bg: u32 = 0xFFFFFF;
pub const default_border: u32 = 0x000000;

pub const fit_flag_hard_wrapped: u8 = 1 << 0;
pub const fit_flag_clipped: u8 = 1 << 1;

/// Rectangle matching `ra8_ui_rect_t`.
pub const Rect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

/// Text alignment matching `ra8_widget_align_t`.
pub const Alignment = enum(u8) {
    left = 0,
    center = 1,
    right = 2,

    pub fn fromRaw(raw: u8) ?Alignment {
        return switch (raw) {
            0 => .left,
            1 => .center,
            2 => .right,
            else => null,
        };
    }
};

/// Font family matching `ra8_widget_text_face_t`.
pub const Face = enum(u8) {
    sans = 0,
    serif = 1,

    pub fn fromRaw(raw: u8) ?Face {
        return switch (raw) {
            0 => .sans,
            1 => .serif,
            else => null,
        };
    }
};

/// Font weight matching `ra8_widget_text_weight_t`.
pub const Weight = enum(u8) {
    regular = 0,
    bold = 1,

    pub fn fromRaw(raw: u8) ?Weight {
        return switch (raw) {
            0 => .regular,
            1 => .bold,
            else => null,
        };
    }
};

/// Text size selector matching `ra8_widget_text_size_t`.
pub const TextSize = enum(u8) {
    default = 0,
    size_1 = 1,
    size_2 = 2,
    size_3 = 3,
    size_4 = 4,
    size_5 = 5,
    body_38 = 6,
    title_68 = 7,
    clock_120 = 8,
    ui_26 = 9,
    ui_30 = 10,
};

/// Pixel format matching `ra8_widget_pixel_format_t`.
pub const PixelFormat = enum(u8) {
    gray4 = 0,
    gray8 = 1,
    rgb565 = 2,
    rgb888 = 3,
    argb8888 = 4,
};

/// Per-canvas capability flags matching `ra8_widget_canvas_caps_t`.
pub const CanvasCaps = extern struct {
    pixel_format: PixelFormat = .gray8,
    color: u8 = 0,
    alpha: u8 = 0,
    gray4: u8 = 0,
    dither: u8 = 0,
    line: u8 = 0,
    blit: u8 = 0,
    clip: u8 = 0,
};

/// Drawing vtable matching `ra8_widget_paint_t` byte for byte.
pub const Paint = extern struct {
    user: ?*anyopaque,
    fill_rect: ?*const fn (user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void,
    draw_text: ?*const fn (user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, fg: u32, bg: u32) callconv(.c) void,
    text_size: ?*const fn (user: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void,
    draw_text_face: ?*const fn (user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, face: u8, fg: u32, bg: u32) callconv(.c) void = null,
    text_size_face: ?*const fn (user: ?*anyopaque, str: [*:0]const u8, face: u8, out_w: *i32, out_h: *i32) callconv(.c) void = null,
    draw_text_style: ?*const fn (user: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, face: u8, weight: u8, size: u8, fg: u32, bg: u32) callconv(.c) void = null,
    text_size_style: ?*const fn (user: ?*anyopaque, str: [*:0]const u8, face: u8, weight: u8, size: u8, out_w: *i32, out_h: *i32) callconv(.c) void = null,
    draw_line: ?*const fn (user: ?*anyopaque, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) void = null,
    blit: ?*const fn (user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, pixels: [*]const u8, stride: i32, format: u8) callconv(.c) void = null,
    push_clip: ?*const fn (user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32) callconv(.c) void = null,
    pop_clip: ?*const fn (user: ?*anyopaque) callconv(.c) void = null,
    fill_rect_alpha: ?*const fn (user: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32, alpha: u8) callconv(.c) void = null,
    caps: ?*const CanvasCaps = null,
};

/// Visual configuration matching `ra8_translate_overlay_style_t`.
pub const Style = extern struct {
    fg_color: u32 = default_fg,
    bg_color: u32 = default_bg,
    border_color: u32 = default_border,
    pad: i16 = 4,
    border_w: i16 = 0,
    line_gap: i16 = 0,
    alignment: u8 = @backingInt(Alignment.center),
    face: u8 = @backingInt(Face.sans),
    weight: u8 = @backingInt(Weight.regular),
    flags: u8 = 0,
    reserved: [2]u8 = .{ 0, 0 },
};

/// Bubble geometry matching `ra8_translate_overlay_bubble_spec_t`.
pub const BubbleSpec = extern struct {
    x: u16,
    y: u16,
    w: u16,
    h: u16,
    shape: u8 = @backingInt(sidecar.Shape.rect),
    text_orientation: u8 = @backingInt(sidecar.TextOrientation.horizontal),
    corner_radius: u16 = 0,
    reserved: u32 = 0,
};

/// Fit search result matching `ra8_translate_overlay_fit_t`.
pub const FitResult = extern struct {
    font_size_px: u16 = 0,
    line_count: u16 = 0,
    max_line_width: u16 = 0,
    total_height: u16 = 0,
    size_enum: u8 = 0,
    fit_flags: u8 = 0,
    reserved: [2]u8 = .{ 0, 0 },
};

const FontRung = struct {
    size_enum: u8,
    use_styled: bool,
    line_height: u16,
};

const WrapTrial = struct {
    line_count: u16,
    max_width: u16,
    total_height: u16,
    hard_wrapped: bool,
    fits_box: bool,
    breaks: [max_lines]u16,
};

const candidate_sizes = [_]TextSize{
    .title_68,
    .size_5,
    .size_4,
    .body_38,
    .size_2,
    .size_1,
    .ui_30,
    .ui_26,
};

/// Validate a bubble geometry descriptor cleanly before reading or drawing.
pub fn validateBubble(bubble: BubbleSpec) Error!void {
    if (bubble.w == 0 or bubble.h == 0) return Error.InvalidSize;
    if (sidecar.Shape.fromRaw(bubble.shape) == null) return Error.InvalidArg;
    if (sidecar.TextOrientation.fromRaw(bubble.text_orientation) == null) return Error.InvalidArg;
    if (bubble.reserved != 0) return Error.InvalidArg;
}

/// Validate overlay style fields cleanly.
pub fn validateStyle(style: Style) Error!void {
    if (style.pad < 0 or style.border_w < 0 or style.line_gap < 0) return Error.InvalidArg;
    if (Alignment.fromRaw(style.alignment) == null) return Error.InvalidArg;
    if (Face.fromRaw(style.face) == null) return Error.InvalidArg;
    if (Weight.fromRaw(style.weight) == null) return Error.InvalidArg;
    if (style.flags != 0 or style.reserved[0] != 0 or style.reserved[1] != 0) {
        return Error.InvalidArg;
    }
}

/// Compute the inscribed safe text box inside a bubble mask contour.
pub fn safeTextRect(bubble: BubbleSpec, style: Style) Error!Rect {
    try validateBubble(bubble);
    try validateStyle(style);

    const w: i32 = @intCast(bubble.w);
    const h: i32 = @intCast(bubble.h);
    const base_pad: i32 = @as(i32, style.pad) + @as(i32, style.border_w);
    const shape = sidecar.Shape.fromRaw(bubble.shape).?;

    var margin_x: i32 = base_pad;
    var margin_y: i32 = base_pad;
    switch (shape) {
        .rect => {},
        .rounded_rect => {
            const r = @min(@as(i32, bubble.corner_radius), @min(@divFloor(w, 2), @divFloor(h, 2)));
            const corner_inset = @divFloor(r * 76 + 255, 256) + @as(i32, style.border_w);
            margin_x = @max(base_pad, corner_inset);
            margin_y = @max(base_pad, corner_inset);
        },
        .ellipse, .cloud => {
            const ratio = shapeSafeScale(shape, w, h);
            const inner_w = @max(@divFloor(w * ratio.x_num, 256) - 2 * @as(i32, style.border_w), 1);
            const inner_h = @max(@divFloor(h * ratio.y_num, 256) - 2 * @as(i32, style.border_w), 1);
            margin_x = @max(@divFloor(w - inner_w, 2), base_pad);
            margin_y = @max(@divFloor(h - inner_h, 2), base_pad);
        },
    }

    const out_w = @max(w - 2 * margin_x, 0);
    const out_h = @max(h - 2 * margin_y, 0);
    return .{
        .x = @as(i32, bubble.x) + margin_x,
        .y = @as(i32, bubble.y) + margin_y,
        .w = out_w,
        .h = out_h,
    };
}

fn shapeSafeScale(shape: sidecar.Shape, w: i32, h: i32) struct { x_num: i32, y_num: i32 } {
    if (shape == .cloud) {
        if (h >= w * 2) return .{ .x_num = 164, .y_num = 136 };
        if (w >= h * 2) return .{ .x_num = 136, .y_num = 164 };
        return .{ .x_num = 150, .y_num = 150 };
    }
    if (h >= w * 2) return .{ .x_num = 199, .y_num = 153 };
    if (w >= h * 2) return .{ .x_num = 153, .y_num = 199 };
    return .{ .x_num = 179, .y_num = 179 };
}

fn measureSpan(
    paint: *const Paint,
    rung: FontRung,
    face: u8,
    weight: u8,
    text: []const u8,
) struct { w: u32, h: u32 } {
    if (text.len == 0) return .{ .w = 0, .h = rung.line_height };
    var total_w: u32 = 0;
    var max_h: u32 = 0;
    var off: usize = 0;
    var zbuf: [129]u8 = undefined;
    while (off < text.len) {
        const chunk_max = @min(text.len - off, 128);
        var chunk_len = chunk_max;
        if (off + chunk_len < text.len) {
            while (chunk_len > 1 and (text[off + chunk_len] & 0xC0) == 0x80) : (chunk_len -= 1) {}
        }
        @memcpy(zbuf[0..chunk_len], text[off .. off + chunk_len]);
        zbuf[chunk_len] = 0;
        const cstr: [*:0]const u8 = @ptrCast(&zbuf);
        const dim = measureCstr(paint, rung, face, weight, cstr);
        total_w +|= dim.w;
        max_h = @max(max_h, dim.h);
        off += chunk_len;
    }
    return .{ .w = total_w, .h = max_h };
}

fn measureCstr(
    paint: *const Paint,
    rung: FontRung,
    face: u8,
    weight: u8,
    cstr: [*:0]const u8,
) struct { w: u32, h: u32 } {
    var w: i32 = 0;
    var h: i32 = 0;
    if (rung.use_styled) {
        if (paint.text_size_style) |m| {
            m(paint.user, cstr, face, weight, rung.size_enum, &w, &h);
            return .{ .w = @intCast(@max(w, 0)), .h = @intCast(@max(h, 0)) };
        }
    }
    if (paint.text_size) |m| {
        m(paint.user, cstr, &w, &h);
    } else if (paint.text_size_face) |m| {
        m(paint.user, cstr, face, &w, &h);
    } else if (paint.text_size_style) |m| {
        m(paint.user, cstr, face, weight, rung.size_enum, &w, &h);
    }
    return .{ .w = @intCast(@max(w, 0)), .h = @intCast(@max(h, 0)) };
}

fn drawLineSlice(
    paint: *const Paint,
    rung: FontRung,
    face: u8,
    weight: u8,
    x: i32,
    y: i32,
    line_bytes: []const u8,
    fg: u32,
    bg: u32,
) void {
    if (line_bytes.len == 0) return;
    var buf: [max_line_bytes + 1]u8 = undefined;
    var off: usize = 0;
    var pen_x = x;
    while (off < line_bytes.len) {
        var chunk_len = @min(line_bytes.len - off, max_line_bytes);
        if (off + chunk_len < line_bytes.len) {
            while (chunk_len > 1 and (line_bytes[off + chunk_len] & 0xC0) == 0x80) : (chunk_len -= 1) {}
        }
        const chunk = line_bytes[off .. off + chunk_len];
        @memcpy(buf[0..chunk_len], chunk);
        buf[chunk_len] = 0;
        const cstr: [*:0]const u8 = @ptrCast(&buf);
        drawCstr(paint, rung, face, weight, pen_x, y, cstr, fg, bg);
        if (off + chunk_len < line_bytes.len) {
            const dim = measureCstr(paint, rung, face, weight, cstr);
            pen_x +|= @intCast(dim.w);
        }
        off += chunk_len;
    }
}

fn drawCstr(
    paint: *const Paint,
    rung: FontRung,
    face: u8,
    weight: u8,
    x: i32,
    y: i32,
    cstr: [*:0]const u8,
    fg: u32,
    bg: u32,
) void {
    if (rung.use_styled) {
        if (paint.draw_text_style) |draw| {
            draw(paint.user, x, y, cstr, face, weight, rung.size_enum, fg, bg);
            return;
        }
    }
    if (paint.draw_text) |draw| {
        draw(paint.user, x, y, cstr, fg, bg);
    } else if (paint.draw_text_face) |draw| {
        draw(paint.user, x, y, cstr, face, fg, bg);
    } else if (paint.draw_text_style) |draw| {
        draw(paint.user, x, y, cstr, face, weight, rung.size_enum, fg, bg);
    }
}

fn canMeasure(paint: *const Paint) bool {
    return paint.text_size_style != null or paint.text_size_face != null or paint.text_size != null;
}

fn canDrawText(paint: *const Paint) bool {
    return paint.draw_text_style != null or paint.draw_text_face != null or paint.draw_text != null;
}

fn collectRungs(
    paint: *const Paint,
    face: u8,
    weight: u8,
    out: *[candidate_sizes.len + 1]FontRung,
) usize {
    var count: usize = 0;
    const probe: [*:0]const u8 = "Mg";
    if (paint.text_size_style != null and paint.draw_text_style != null) {
        for (candidate_sizes) |sz| {
            const r = FontRung{ .size_enum = @backingInt(sz), .use_styled = true, .line_height = 0 };
            const h = measureCstr(paint, r, face, weight, probe).h;
            if (h == 0 or h > std.math.maxInt(u16)) continue;
            const h16: u16 = @intCast(h);
            var dup = false;
            for (out[0..count]) |prev| {
                if (prev.line_height == h16) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                out[count] = .{ .size_enum = @backingInt(sz), .use_styled = true, .line_height = h16 };
                count += 1;
            }
        }
    }
    if (paint.text_size != null and paint.draw_text != null) {
        const r = FontRung{ .size_enum = 0, .use_styled = false, .line_height = 0 };
        const h = measureCstr(paint, r, face, weight, probe).h;
        if (h > 0 and h <= std.math.maxInt(u16)) {
            const h16: u16 = @intCast(h);
            var smaller = count == 0;
            if (!smaller) {
                smaller = true;
                for (out[0..count]) |prev| {
                    if (prev.line_height <= h16) smaller = false;
                }
            }
            if (smaller) {
                out[count] = .{ .size_enum = 0, .use_styled = false, .line_height = h16 };
                count += 1;
            }
        }
    }
    if (count == 0 and canMeasure(paint) and canDrawText(paint)) {
        const fallback_styled = paint.text_size_style != null and paint.draw_text_style != null;
        const probe_rung = FontRung{
            .size_enum = if (fallback_styled) @backingInt(TextSize.size_3) else 0,
            .use_styled = fallback_styled,
            .line_height = 0,
        };
        const h = @max(measureCstr(paint, probe_rung, face, weight, probe).h, 1);
        out[0] = .{
            .size_enum = probe_rung.size_enum,
            .use_styled = fallback_styled,
            .line_height = @intCast(@min(h, std.math.maxInt(u16))),
        };
        count = 1;
    }
    sortRungsDescending(out[0..count]);
    return count;
}

fn sortRungsDescending(rungs: []FontRung) void {
    var i: usize = 1;
    while (i < rungs.len) : (i += 1) {
        const key = rungs[i];
        var j: usize = i;
        while (j > 0 and rungs[j - 1].line_height < key.line_height) : (j -= 1) {
            rungs[j] = rungs[j - 1];
        }
        rungs[j] = key;
    }
}

fn scalarLen(bytes: []const u8, off: usize) usize {
    return std.unicode.utf8ByteSequenceLength(bytes[off]) catch 1;
}

fn trimLineWhitespace(slice: []const u8) []const u8 {
    var start: usize = 0;
    while (start < slice.len) {
        const c = slice[start];
        if (c == ' ' or c == '\t' or c == '\r' or c == '\n') {
            start += 1;
        } else break;
    }
    var end = slice.len;
    while (end > start) {
        const c = slice[end - 1];
        if (c == ' ' or c == '\t' or c == '\r' or c == '\n') {
            end -= 1;
        } else break;
    }
    return slice[start..end];
}

fn tryWrapRung(
    paint: *const Paint,
    rung: FontRung,
    style: Style,
    text: []const u8,
    safe_w: u32,
    safe_h: u32,
    trial: *WrapTrial,
) void {
    trial.* = .{
        .line_count = 0,
        .max_width = 0,
        .total_height = 0,
        .hard_wrapped = false,
        .fits_box = false,
        .breaks = @splat(0),
    };
    if (safe_w == 0 or safe_h == 0) return;

    const space_w = measureSpan(paint, rung, style.face, style.weight, " ").w;
    var off: usize = 0;
    var cur_start: usize = 0;
    var cur_trim_end: usize = 0;
    var cur_w: u32 = 0;

    while (off < text.len) {
        if (text[off] == '\n') {
            const lw = measureSpan(paint, rung, style.face, style.weight, trimLineWhitespace(text[cur_start..cur_trim_end])).w;
            trial.max_width = @intCast(@min(@max(@as(u32, trial.max_width), lw), std.math.maxInt(u16)));
            if (!pushBreak(trial, off + 1)) return;
            off += 1;
            cur_start = off;
            cur_trim_end = off;
            cur_w = 0;
            continue;
        }
        if (text[off] == ' ' or text[off] == '\t' or text[off] == '\r') {
            off += 1;
            continue;
        }

        const word_start = off;
        while (off < text.len and text[off] != ' ' and text[off] != '\t' and text[off] != '\r' and text[off] != '\n') {
            off += scalarLen(text, off);
        }
        const word = text[word_start..off];
        const word_w = measureSpan(paint, rung, style.face, style.weight, word).w;

        if (cur_trim_end > cur_start) {
            const gap_cols: u32 = @intCast(word_start - cur_trim_end);
            const candidate_w = cur_w +| (space_w *| gap_cols) +| word_w;
            if (candidate_w <= safe_w) {
                cur_trim_end = off;
                cur_w = candidate_w;
                continue;
            }
            trial.max_width = @intCast(@min(@max(@as(u32, trial.max_width), cur_w), std.math.maxInt(u16)));
            if (!pushBreak(trial, word_start)) return;
            cur_start = word_start;
            cur_trim_end = word_start;
            cur_w = 0;
        }

        if (word_w <= safe_w) {
            cur_start = word_start;
            cur_trim_end = off;
            cur_w = word_w;
            continue;
        }

        trial.hard_wrapped = true;
        var pos = word_start;
        while (pos < off) {
            var seg_end = pos;
            var seg_w: u32 = 0;
            while (seg_end < off) {
                const sl = scalarLen(text, seg_end);
                const gw = measureSpan(paint, rung, style.face, style.weight, text[seg_end .. seg_end + sl]).w;
                if (seg_end > pos and seg_w +| gw > safe_w) break;
                seg_w +|= gw;
                seg_end += sl;
            }
            trial.max_width = @intCast(@min(@max(@as(u32, trial.max_width), seg_w), std.math.maxInt(u16)));
            if (seg_end == off) {
                cur_start = pos;
                cur_trim_end = off;
                cur_w = seg_w;
                break;
            }
            if (!pushBreak(trial, seg_end)) return;
            cur_start = seg_end;
            cur_trim_end = seg_end;
            cur_w = 0;
            pos = seg_end;
        }
    }

    if (trial.line_count == 0 or trial.breaks[trial.line_count - 1] < text.len) {
        const tail = trimLineWhitespace(text[cur_start..text.len]);
        const lw = measureSpan(paint, rung, style.face, style.weight, tail).w;
        trial.max_width = @intCast(@min(@max(@as(u32, trial.max_width), lw), std.math.maxInt(u16)));
        if (!pushBreak(trial, text.len)) return;
    }

    const total_h = computeBlockHeight(trial.line_count, rung.line_height, @intCast(style.line_gap));
    trial.total_height = @intCast(@min(total_h, std.math.maxInt(u16)));
    trial.fits_box = trial.max_width <= safe_w and total_h <= safe_h;
}

fn pushBreak(trial: *WrapTrial, end_off: usize) bool {
    if (trial.line_count >= max_lines or end_off > std.math.maxInt(u16)) return false;
    trial.breaks[trial.line_count] = @intCast(end_off);
    trial.line_count += 1;
    return true;
}

fn computeBlockHeight(line_count: u16, line_h: u16, line_gap: u16) u32 {
    if (line_count == 0) return 0;
    const lh: u32 = line_h;
    const lc: u32 = line_count;
    const lg: u32 = line_gap;
    return lc * lh + (lc - 1) * lg;
}

/// Find the largest font size that fits `english_utf8` inside `bubble`,
/// returning `FitResult` and cumulative `TRN1` line breaks in `out_breaks`.
pub fn fitText(
    paint: *const Paint,
    bubble: BubbleSpec,
    style: Style,
    english_utf8: []const u8,
    out_breaks: []u16,
) Error!FitResult {
    const safe = try safeTextRect(bubble, style);
    if (english_utf8.len == 0) return .{};
    if (english_utf8.len > std.math.maxInt(u16)) return Error.InvalidSize;
    if (!std.unicode.utf8ValidateSlice(english_utf8)) return Error.ValidationFailed;
    if (!canMeasure(paint) or !canDrawText(paint)) return Error.NotSupported;

    var rungs: [candidate_sizes.len + 1]FontRung = undefined;
    const rung_count = collectRungs(paint, style.face, style.weight, &rungs);
    if (rung_count == 0) return Error.NotSupported;

    const safe_w: u32 = @intCast(@max(safe.w, 0));
    const safe_h: u32 = @intCast(@max(safe.h, 0));

    var best_trial: WrapTrial = undefined;
    var best_rung: ?FontRung = null;

    for (rungs[0..rung_count]) |rung| {
        tryWrapRung(paint, rung, style, english_utf8, safe_w, safe_h, &best_trial);
        if (best_trial.line_count > 0 and best_trial.fits_box and !best_trial.hard_wrapped) {
            best_rung = rung;
            break;
        }
    }
    if (best_rung == null) {
        for (rungs[0..rung_count]) |rung| {
            tryWrapRung(paint, rung, style, english_utf8, safe_w, safe_h, &best_trial);
            if (best_trial.line_count > 0 and best_trial.fits_box) {
                best_rung = rung;
                break;
            }
        }
    }
    var flags: u8 = 0;
    if (best_rung == null) {
        const smallest = rungs[rung_count - 1];
        tryWrapRung(paint, smallest, style, english_utf8, @max(safe_w, 1), @max(safe_h, 1), &best_trial);
        if (best_trial.line_count == 0) return Error.InvalidSize;
        best_rung = smallest;
        flags |= fit_flag_clipped;
    }

    const winner = best_rung.?;
    if (best_trial.hard_wrapped) flags |= fit_flag_hard_wrapped;
    if (best_trial.line_count > out_breaks.len) return Error.InvalidSize;
    @memcpy(out_breaks[0..best_trial.line_count], best_trial.breaks[0..best_trial.line_count]);

    return .{
        .font_size_px = winner.line_height,
        .line_count = best_trial.line_count,
        .max_line_width = best_trial.max_width,
        .total_height = best_trial.total_height,
        .size_enum = winner.size_enum,
        .fit_flags = flags,
    };
}

fn selectRungForPx(paint: *const Paint, face: u8, weight: u8, target_px: u16) ?FontRung {
    var rungs: [candidate_sizes.len + 1]FontRung = undefined;
    const count = collectRungs(paint, face, weight, &rungs);
    if (count == 0) return null;
    for (rungs[0..count]) |rung| {
        if (rung.line_height == target_px) return rung;
    }
    for (rungs[0..count]) |rung| {
        if (rung.line_height <= target_px) return rung;
    }
    return rungs[count - 1];
}

fn validateRenderLines(text: []const u8, breaks: []const u16) Error!void {
    if (text.len == 0) {
        if (breaks.len != 0) return Error.InvalidArg;
        return;
    }
    if (text.len > std.math.maxInt(u16)) return Error.InvalidSize;
    if (!std.unicode.utf8ValidateSlice(text)) return Error.ValidationFailed;
    if (breaks.len == 0) return Error.InvalidArg;
    var prev: u16 = 0;
    for (breaks) |b| {
        if (b <= prev or b > text.len) return Error.InvalidArg;
        if (b < text.len and (text[b] & 0xC0) == 0x80) return Error.InvalidArg;
        prev = b;
    }
    if (prev != text.len) return Error.InvalidArg;
}

/// Fill `rect` with `shape` mask strictly bounded within `[rect.x..+rect.w, rect.y..+rect.h]`.
pub fn fillShapeMask(
    paint: *const Paint,
    rect: Rect,
    shape: sidecar.Shape,
    corner_radius: u16,
    color: u32,
) void {
    overlay_mask.fillShapeMask(
        paint.user,
        paint.fill_rect,
        rect.x,
        rect.y,
        rect.w,
        rect.h,
        shape,
        corner_radius,
        color,
    );
}

/// Fill one bubble's white mask (plus optional border) and render pre-fitted lines.
pub fn renderBubble(
    paint: *const Paint,
    bubble: BubbleSpec,
    style: Style,
    font_size_px: u16,
    english_utf8: []const u8,
    line_breaks: []const u16,
) Error!Rect {
    const safe = try safeTextRect(bubble, style);
    try validateRenderLines(english_utf8, line_breaks);
    if (paint.fill_rect == null) return Error.NotSupported;

    var selected_rung: ?FontRung = null;
    if (english_utf8.len > 0) {
        if (font_size_px == 0) return Error.InvalidArg;
        if (!canMeasure(paint) or !canDrawText(paint)) return Error.NotSupported;
        selected_rung = selectRungForPx(paint, style.face, style.weight, font_size_px) orelse return Error.NotSupported;
    }

    const damage = Rect{
        .x = @intCast(bubble.x),
        .y = @intCast(bubble.y),
        .w = @intCast(bubble.w),
        .h = @intCast(bubble.h),
    };
    const clip_bound = paint.push_clip != null and paint.pop_clip != null;
    if (clip_bound) paint.push_clip.?(paint.user, damage.x, damage.y, damage.w, damage.h);
    defer if (clip_bound) paint.pop_clip.?(paint.user);

    const shape = sidecar.Shape.fromRaw(bubble.shape).?;
    if (style.border_w > 0) {
        fillShapeMask(paint, damage, shape, bubble.corner_radius, style.border_color);
        const bw: i32 = style.border_w;
        const inner = Rect{
            .x = damage.x + bw,
            .y = damage.y + bw,
            .w = damage.w - 2 * bw,
            .h = damage.h - 2 * bw,
        };
        const inner_r: u16 = bubble.corner_radius -| @as(u16, @intCast(bw));
        if (inner.w > 0 and inner.h > 0) {
            fillShapeMask(paint, inner, shape, inner_r, style.bg_color);
        }
    } else {
        fillShapeMask(paint, damage, shape, bubble.corner_radius, style.bg_color);
    }

    if (selected_rung) |rung| {
        if (safe.w > 0 and safe.h > 0) {
            drawFittedLines(paint, rung, style, safe, english_utf8, line_breaks);
        }
    }
    return damage;
}

fn drawFittedLines(
    paint: *const Paint,
    rung: FontRung,
    style: Style,
    safe: Rect,
    english_utf8: []const u8,
    line_breaks: []const u16,
) void {
    const lh: i32 = rung.line_height;
    const lg: i32 = style.line_gap;
    const step_y = lh + lg;
    const max_vis: usize = if (safe.h < lh) 0 else @intCast(1 + @divFloor(safe.h - lh, @max(step_y, 1)));
    const vis_lines = @min(line_breaks.len, max_vis);
    if (vis_lines == 0) return;

    const block_h: i32 = @intCast(computeBlockHeight(@intCast(vis_lines), rung.line_height, @intCast(style.line_gap)));
    var y = safe.y + @divFloor(@max(safe.h - block_h, 0), 2);
    const alignment = Alignment.fromRaw(style.alignment).?;

    var line_start: usize = 0;
    for (line_breaks[0..vis_lines]) |end_u16| {
        const end: usize = @intCast(end_u16);
        const trimmed = trimLineWhitespace(english_utf8[line_start..end]);
        if (trimmed.len > 0) {
            const lw: i32 = @intCast(measureSpan(paint, rung, style.face, style.weight, trimmed).w);
            const x = switch (alignment) {
                .left => safe.x,
                .center => safe.x + @divFloor(safe.w - lw, 2),
                .right => safe.x + safe.w - lw,
            };
            drawLineSlice(paint, rung, style.face, style.weight, x, y, trimmed, style.fg_color, style.bg_color);
        }
        line_start = end;
        y += step_y;
    }
}

fn rectContainedIn(inner: Rect, outer: Rect) bool {
    return inner.x >= outer.x and
        inner.y >= outer.y and
        inner.x + inner.w <= outer.x + outer.w and
        inner.y + inner.h <= outer.y + outer.h;
}

fn clampDamageRect(bubble: sidecar.BubbleRecord, page: sidecar.PageEntry) Rect {
    const x0: i32 = @intCast(bubble.x);
    const y0: i32 = @intCast(bubble.y);
    const x1: i32 = @min(@as(i32, bubble.x) + @as(i32, bubble.w), @as(i32, page.page_width));
    const y1: i32 = @min(@as(i32, bubble.y) + @as(i32, bubble.h), @as(i32, page.page_height));
    return .{
        .x = x0,
        .y = y0,
        .w = @max(x1 - x0, 0),
        .h = @max(y1 - y0, 0),
    };
}

fn emitMinimalDamage(
    view: sidecar.SidecarView,
    page: sidecar.PageEntry,
    out_damage: ?[]Rect,
) Error!usize {
    const base: usize = @intCast(page.first_bubble);
    const count: usize = @intCast(page.bubble_count);
    var emitted: usize = 0;
    var i: usize = 0;
    while (i < count) : (i += 1) {
        const bi = try view.bubbleAt(@intCast(base + i));
        const ri = clampDamageRect(bi, page);
        if (ri.w <= 0 or ri.h <= 0) continue;
        var covered = false;
        var j: usize = 0;
        while (j < count) : (j += 1) {
            if (j == i) continue;
            const bj = try view.bubbleAt(@intCast(base + j));
            const rj = clampDamageRect(bj, page);
            if (rj.w <= 0 or rj.h <= 0) continue;
            if (rectContainedIn(ri, rj) and (j < i or !rectContainedIn(rj, ri))) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            if (out_damage) |dst| dst[emitted] = ri;
            emitted += 1;
        }
    }
    return emitted;
}

/// Render every translated bubble on `page_index` of `trn1_bytes` in reading order
/// and emit minimal non-contained bubble damage rectangles for partial panel refresh.
pub fn renderPage(
    paint: *const Paint,
    trn1_bytes: []const u8,
    page_index: u16,
    style: Style,
    out_damage: []Rect,
) Error!usize {
    try validateStyle(style);
    if (paint.fill_rect == null) return Error.NotSupported;

    const view = try sidecar.SidecarView.open(trn1_bytes);
    const page = try view.pageAt(page_index);
    const count: usize = @intCast(page.bubble_count);
    if (count == 0) return 0;

    var has_any_text = false;
    var check_i: usize = 0;
    while (check_i < count) : (check_i += 1) {
        const b = try view.bubbleAt(@intCast(page.first_bubble + @as(u32, @intCast(check_i))));
        const spec = specFromRecord(b);
        try validateBubble(spec);
        if (b.english_text_len > 0) {
            if (b.font_size_px == 0) return Error.InvalidArg;
            has_any_text = true;
        }
    }
    if (has_any_text and (!canMeasure(paint) or !canDrawText(paint))) return Error.NotSupported;

    const needed_damage = try emitMinimalDamage(view, page, null);
    if (needed_damage > out_damage.len) return Error.InvalidSize;

    var breaks_buf: [max_lines]u16 = undefined;
    var order: u16 = 0;
    while (order < page.bubble_count) : (order += 1) {
        const bubble_idx = try findBubbleByOrder(view, page, order);
        const b = try view.bubbleAt(bubble_idx);
        const eng = try view.englishText(bubble_idx);
        if (b.line_count > breaks_buf.len) return Error.InvalidSize;
        const lc = try view.lineBreaks(bubble_idx, &breaks_buf);
        _ = try renderBubble(
            paint,
            specFromRecord(b),
            style,
            b.font_size_px,
            eng,
            breaks_buf[0..lc],
        );
    }

    return try emitMinimalDamage(view, page, out_damage);
}

fn findBubbleByOrder(view: sidecar.SidecarView, page: sidecar.PageEntry, order: u16) Error!u32 {
    var i: usize = 0;
    while (i < page.bubble_count) : (i += 1) {
        const idx = page.first_bubble + @as(u32, @intCast(i));
        const b = try view.bubbleAt(idx);
        if (b.reading_order == order) return idx;
    }
    return Error.ValidationFailed;
}

fn specFromRecord(b: sidecar.BubbleRecord) BubbleSpec {
    return .{
        .x = b.x,
        .y = b.y,
        .w = b.w,
        .h = b.h,
        .shape = b.shape,
        .text_orientation = b.text_orientation,
        .corner_radius = b.corner_radius,
    };
}
