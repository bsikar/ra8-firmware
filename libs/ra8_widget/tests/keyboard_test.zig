//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the keyboard membrane: the descriptor and key-info layouts, the
//! bind guards, what one key draws (face, then glyph or label), and what a tap
//! does (hit, apply, invalidate, commit edge). The engine seam is a recording
//! mock, so the key grid here is whatever the test says it is.

const std = @import("std");
const abi = @import("abi");

/// Link-time substitutes for the two C symbols the library leaves undefined:
/// the logger and `ra8_widget_invalidate` from the still-C `ra8_widget.c`.
var last_message: ?[*:0]const u8 = null;
var invalidations: u32 = 0;
var last_refresh: u8 = 0xFF;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}

export fn ra8_widget_invalidate(w: *abi.Widget, refresh: u8) callconv(.c) u16 {
    invalidations += 1;
    last_refresh = refresh;
    w.dirty = true;
    w.refresh = refresh;
    return abi.err.ok;
}

const Fill = struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
    color: u32,
};

/// The drawn text is copied, not aliased: a character key's glyph buffer is a
/// transient local in the draw helper (as it is in the C), so keeping the
/// pointer would read a dead stack frame.
const Draw = struct {
    text: [16]u8,
    len: usize,
    x: i32,
    y: i32,
    fg: u32,
    bg: u32,

    fn str(self: *const Draw) []const u8 {
        return self.text[0..self.len];
    }
};

/// Recording paint backend.
const Recorder = struct {
    var fills: std.BoundedArray(Fill, 16) = .{};
    var draws: std.BoundedArray(Draw, 16) = .{};

    fn fillRect(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
        fills.append(.{ .x = x, .y = y, .w = w, .h = h, .color = color }) catch unreachable;
    }

    fn drawText(
        _: ?*anyopaque,
        x: i32,
        y: i32,
        str: [*:0]const u8,
        fg: u32,
        bg: u32,
    ) callconv(.c) void {
        var record: Draw = .{ .text = undefined, .len = 0, .x = x, .y = y, .fg = fg, .bg = bg };
        const seen = std.mem.span(str);
        record.len = @min(seen.len, record.text.len);
        @memcpy(record.text[0..record.len], seen[0..record.len]);
        draws.append(record) catch unreachable;
    }

    /// Fixed-width measurement so centring has something to halve.
    fn textSize(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
        out_w.* = @intCast(std.mem.span(str).len * 6);
        out_h.* = 12;
    }
};

/// Recording keyboard-engine seam: a flat key list the test supplies.
const Engine = struct {
    var keys: std.BoundedArray(abi.KeyInfo, 8) = .{};
    var hit_answer: u8 = abi.key.no_hit;
    var hit_calls: u32 = 0;
    var last_hit_x: i32 = 0;
    var last_hit_y: i32 = 0;
    var applied: std.BoundedArray(u8, 8) = .{};
    var commit_answer: bool = false;
    var info_calls: u32 = 0;

    fn count(_: ?*anyopaque) callconv(.c) u8 {
        return @intCast(keys.len);
    }

    fn keyInfo(_: ?*anyopaque, idx: u8, out: *abi.KeyInfo) callconv(.c) void {
        info_calls += 1;
        out.* = keys.get(idx);
    }

    fn hit(_: ?*anyopaque, x: i32, y: i32) callconv(.c) u8 {
        hit_calls += 1;
        last_hit_x = x;
        last_hit_y = y;
        return hit_answer;
    }

    fn apply(_: ?*anyopaque, idx: u8) callconv(.c) bool {
        applied.append(idx) catch unreachable;
        return commit_answer;
    }
};

var commits: u32 = 0;
var field_submits: u32 = 0;

fn noteCommit(_: *abi.Widget) callconv(.c) void {
    commits += 1;
}

fn noteFieldSubmit(_: *abi.Widget) callconv(.c) void {
    field_submits += 1;
}

fn reset() void {
    Recorder.fills = .{};
    Recorder.draws = .{};
    Engine.keys = .{};
    Engine.hit_answer = abi.key.no_hit;
    Engine.hit_calls = 0;
    Engine.applied = .{};
    Engine.commit_answer = false;
    Engine.info_calls = 0;
    last_message = null;
    invalidations = 0;
    last_refresh = 0xFF;
    commits = 0;
    field_submits = 0;
}

const full_backend: abi.Paint = .{
    .user = null,
    .fill_rect = Recorder.fillRect,
    .draw_text = Recorder.drawText,
    .text_size = Recorder.textSize,
};

const fill_only_backend: abi.Paint = .{
    .user = null,
    .fill_rect = Recorder.fillRect,
    .draw_text = null,
    .text_size = null,
};

const full_ops: abi.Ops = .{
    .user = null,
    .count = Engine.count,
    .key_info = Engine.keyInfo,
    .hit = Engine.hit,
    .apply = Engine.apply,
};

const bg_color: u32 = 0x00202020;
const face_color: u32 = 0x00404040;
const key_border_color: u32 = 0x00101010;
const key_fg_color: u32 = 0x00FFFFFF;

fn charKey(rect: abi.Rect, glyph: u8) abi.KeyInfo {
    return .{ .rect = rect, .label = null, .glyph = glyph, .pad0 = 0, .pad1 = 0, .action = .character };
}

fn labelKey(rect: abi.Rect, label: [*:0]const u8) abi.KeyInfo {
    return .{ .rect = rect, .label = label, .glyph = abi.key.no_glyph, .pad0 = 0, .pad1 = 0 };
}

fn keyboardAt(paint: ?*const abi.Paint, ops: ?*const abi.Ops) abi.Keyboard {
    return .{
        .paint = paint,
        .ops = ops,
        .on_commit = noteCommit,
        .bg = bg_color,
        .key_face = face_color,
        .key_border = key_border_color,
        .key_fg = key_fg_color,
        .border_w = 1,
        .reserved = 0,
    };
}

fn widgetAt(rect: abi.Rect) abi.Widget {
    return .{
        .vt = null,
        .ctx = null,
        .rect = rect,
        .fixed = 0,
        .flex = 0,
        .action_id = 0,
        .refresh = 0,
        .visible = false,
        .dirty = false,
    };
}

const band: abi.Rect = .{ .x = 0, .y = 200, .w = 240, .h = 120 };

fn touchAt(x: i32, y: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = y };
}

test "the descriptor mirrors ra8_widget_keyboard_t" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(0, @offsetOf(abi.Keyboard, "paint"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.Keyboard, "ops"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.Keyboard, "on_commit"));
    try std.testing.expectEqual(3 * ptr + 16, @offsetOf(abi.Keyboard, "border_w"));
    try std.testing.expectEqual(3 * ptr + 18, @offsetOf(abi.Keyboard, "reserved"));
}

test "the key info and seam mirror their C structs" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(0, @offsetOf(abi.KeyInfo, "rect"));
    try std.testing.expectEqual(16, @offsetOf(abi.KeyInfo, "label"));
    try std.testing.expectEqual(16 + ptr, @offsetOf(abi.KeyInfo, "glyph"));
    try std.testing.expectEqual(5 * ptr, @sizeOf(abi.Ops));
    try std.testing.expectEqual(4 * ptr, @offsetOf(abi.Ops, "apply"));
}

test "the no-hit sentinel is the C one" {
    try std.testing.expectEqual(255, abi.key.no_hit);
    try std.testing.expectEqual(0, abi.key.no_glyph);
}

test "init binds the vtable, the context and visibility" {
    reset();
    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);

    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_keyboard_init(&w, &kbd));
    try std.testing.expectEqual(abi.ra8_widget_keyboard_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(*anyopaque, @ptrCast(&kbd)), w.ctx.?);
    try std.testing.expect(w.visible);
}

test "init refuses a null widget or descriptor and leaves nothing bound" {
    reset();
    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_keyboard_init(null, &kbd));
    try std.testing.expect(last_message != null);

    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_keyboard_init(&w, null));
    try std.testing.expectEqual(null, w.vt);
    try std.testing.expect(!w.visible);
}

test "the vtable measures nothing and is shared by every keyboard" {
    const vt = abi.ra8_widget_keyboard_vtable();
    try std.testing.expectEqual(null, vt.measure);
    try std.testing.expect(vt.render != null);
    try std.testing.expect(vt.on_input != null);
    try std.testing.expectEqual(vt, abi.ra8_widget_keyboard_vtable());
}

test "render fills the band, then one framed face per key" {
    reset();
    Engine.keys.append(charKey(.{ .x = 4, .y = 204, .w = 20, .h = 20 }, 'a')) catch unreachable;
    Engine.keys.append(charKey(.{ .x = 28, .y = 204, .w = 20, .h = 20 }, 'b')) catch unreachable;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);
    abi.ra8_widget_keyboard_vtable().render.?(&w);

    // band, then border+face for each of the two keys.
    try std.testing.expectEqual(5, Recorder.fills.len);
    try std.testing.expectEqual(bg_color, Recorder.fills.get(0).color);
    try std.testing.expectEqual(band.w, Recorder.fills.get(0).w);
    try std.testing.expectEqual(key_border_color, Recorder.fills.get(1).color);
    try std.testing.expectEqual(face_color, Recorder.fills.get(2).color);
    // The face is inset by border_w on every edge.
    try std.testing.expectEqual(5, Recorder.fills.get(2).x);
    try std.testing.expectEqual(18, Recorder.fills.get(2).w);
    try std.testing.expectEqual(2, Engine.info_calls);
}

test "a character key draws its glyph centred, a label key its label" {
    reset();
    Engine.keys.append(charKey(.{ .x = 0, .y = 200, .w = 24, .h = 24 }, 'q')) catch unreachable;
    Engine.keys.append(labelKey(.{ .x = 24, .y = 200, .w = 60, .h = 24 }, "space")) catch unreachable;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);
    abi.ra8_widget_keyboard_vtable().render.?(&w);

    try std.testing.expectEqual(2, Recorder.draws.len);
    try std.testing.expectEqualStrings("q", Recorder.draws.get(0).str());
    try std.testing.expectEqualStrings("space", Recorder.draws.get(1).str());
    // 24 wide, one 6px glyph: (24 - 6) / 2. Vertically (24 - 12) / 2 below the top.
    try std.testing.expectEqual(9, Recorder.draws.get(0).x);
    try std.testing.expectEqual(206, Recorder.draws.get(0).y);
    // 60 wide, five glyphs at 6px: (60 - 30) / 2 from x = 24.
    try std.testing.expectEqual(39, Recorder.draws.get(1).x);
    try std.testing.expectEqual(key_fg_color, Recorder.draws.get(0).fg);
    try std.testing.expectEqual(face_color, Recorder.draws.get(0).bg);
}

test "a glyph wins over a label on the same key" {
    reset();
    var both = labelKey(.{ .x = 0, .y = 200, .w = 24, .h = 24 }, "shift");
    both.glyph = 'z';
    Engine.keys.append(both) catch unreachable;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);
    abi.ra8_widget_keyboard_vtable().render.?(&w);

    try std.testing.expectEqual(1, Recorder.draws.len);
    try std.testing.expectEqualStrings("z", Recorder.draws.get(0).str());
}

test "a key with neither glyph nor label draws only its face" {
    reset();
    Engine.keys.append(.{
        .rect = .{ .x = 0, .y = 200, .w = 24, .h = 24 },
        .label = null,
        .glyph = abi.key.no_glyph,
        .pad0 = 0,
        .pad1 = 0,
    }) catch unreachable;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);
    abi.ra8_widget_keyboard_vtable().render.?(&w);

    try std.testing.expectEqual(3, Recorder.fills.len);
    try std.testing.expectEqual(0, Recorder.draws.len);
}

test "a backend with no draw_text still paints the grid" {
    reset();
    Engine.keys.append(charKey(.{ .x = 0, .y = 200, .w = 24, .h = 24 }, 'a')) catch unreachable;

    var kbd = keyboardAt(&fill_only_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);
    abi.ra8_widget_keyboard_vtable().render.?(&w);

    try std.testing.expectEqual(3, Recorder.fills.len);
    try std.testing.expectEqual(0, Recorder.draws.len);
}

test "render draws nothing at all without a paint backend" {
    reset();
    Engine.keys.append(charKey(.{ .x = 0, .y = 200, .w = 24, .h = 24 }, 'a')) catch unreachable;

    var kbd = keyboardAt(null, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);
    abi.ra8_widget_keyboard_vtable().render.?(&w);

    try std.testing.expectEqual(0, Recorder.fills.len);
    try std.testing.expectEqual(0, Engine.info_calls);
}

test "an inert seam still fills the band" {
    reset();
    var no_ops = keyboardAt(&full_backend, null);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &no_ops);
    abi.ra8_widget_keyboard_vtable().render.?(&w);

    try std.testing.expectEqual(1, Recorder.fills.len);
    try std.testing.expectEqual(bg_color, Recorder.fills.get(0).color);

    reset();
    const partial: abi.Ops = .{
        .user = null,
        .count = Engine.count,
        .key_info = null,
        .hit = Engine.hit,
        .apply = Engine.apply,
    };
    var half = keyboardAt(&full_backend, &partial);
    var w2 = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w2, &half);
    abi.ra8_widget_keyboard_vtable().render.?(&w2);

    try std.testing.expectEqual(1, Recorder.fills.len);
}

test "a tap on a key applies it and invalidates for a quality refresh" {
    reset();
    Engine.keys.append(charKey(.{ .x = 0, .y = 200, .w = 24, .h = 24 }, 'a')) catch unreachable;
    Engine.hit_answer = 0;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);

    const event = touchAt(12, 212);
    try std.testing.expect(abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(1, Engine.hit_calls);
    try std.testing.expectEqual(12, Engine.last_hit_x);
    try std.testing.expectEqual(212, Engine.last_hit_y);
    try std.testing.expectEqual(1, Engine.applied.len);
    try std.testing.expectEqual(0, Engine.applied.get(0));
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(@intFromEnum(abi.Refresh.quality), last_refresh);
    try std.testing.expect(w.dirty);
    try std.testing.expectEqual(0, commits);
}

test "on_commit fires exactly on the commit edge" {
    reset();
    Engine.hit_answer = 3;
    Engine.commit_answer = true;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);

    const event = touchAt(1, 201);
    try std.testing.expect(abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(3, Engine.applied.get(0));
    try std.testing.expectEqual(1, commits);

    Engine.commit_answer = false;
    try std.testing.expect(abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(1, commits);
    try std.testing.expectEqual(2, invalidations);
}

test "a commit with no callback bound is still applied" {
    reset();
    Engine.hit_answer = 1;
    Engine.commit_answer = true;

    var kbd = keyboardAt(&full_backend, &full_ops);
    kbd.on_commit = null;
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);

    const event = touchAt(1, 201);
    try std.testing.expect(abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(1, Engine.applied.len);
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(0, commits);
}

test "a tap on a gap is consumed but changes nothing" {
    reset();
    Engine.hit_answer = abi.key.no_hit;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);

    const event = touchAt(5, 205);
    try std.testing.expect(abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(1, Engine.hit_calls);
    try std.testing.expectEqual(0, Engine.applied.len);
    try std.testing.expectEqual(0, invalidations);
    try std.testing.expect(!w.dirty);
}

test "a button event is declined so it keeps travelling" {
    reset();
    Engine.hit_answer = 0;

    var kbd = keyboardAt(&full_backend, &full_ops);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);

    const event: abi.Event = .{ .kind = .button, .reserved = 0, .button_id = 7, .x = 0, .y = 0 };
    try std.testing.expect(!abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(0, Engine.hit_calls);
    try std.testing.expectEqual(0, invalidations);
}

test "an inert seam consumes the touch without hitting anything" {
    reset();
    var kbd = keyboardAt(&full_backend, null);
    var w = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&w, &kbd);

    const event = touchAt(5, 205);
    try std.testing.expect(abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    try std.testing.expectEqual(0, Engine.hit_calls);
    try std.testing.expectEqual(0, invalidations);
}

test "a widget with no context declines input and renders nothing" {
    reset();
    var w = widgetAt(band);
    const event = touchAt(5, 205);
    try std.testing.expect(!abi.ra8_widget_keyboard_vtable().on_input.?(&w, &event));
    abi.ra8_widget_keyboard_vtable().render.?(&w);
    try std.testing.expectEqual(0, Recorder.fills.len);
}

test "keyboard keys update the focused fixed-buffer field and report bounded damage" {
    reset();
    Engine.keys.append(charKey(.{ .x = 4, .y = 204, .w = 20, .h = 20 }, 'r')) catch unreachable;
    Engine.keys.append(charKey(.{ .x = 28, .y = 204, .w = 20, .h = 20 }, 'e')) catch unreachable;
    Engine.keys.append(charKey(.{ .x = 52, .y = 204, .w = 20, .h = 20 }, 'a')) catch unreachable;
    Engine.keys.append(charKey(.{ .x = 76, .y = 204, .w = 20, .h = 20 }, 'd')) catch unreachable;
    Engine.keys.append(charKey(.{ .x = 100, .y = 204, .w = 20, .h = 20 }, 'x')) catch unreachable;
    var backspace = labelKey(.{ .x = 124, .y = 204, .w = 28, .h = 20 }, "delete");
    backspace.action = .backspace;
    Engine.keys.append(backspace) catch unreachable;
    var enter = labelKey(.{ .x = 156, .y = 204, .w = 28, .h = 20 }, "enter");
    enter.action = .enter;
    Engine.keys.append(enter) catch unreachable;

    var buffer = [_]u8{0} ** 5;
    var field = abi.text_field.TextField{
        .paint = &full_backend,
        .buffer = &buffer,
        .capacity = @intCast(buffer.len),
        .len = 0,
        .placeholder = "Search",
        .fg = 0,
        .bg = 0xffffff,
        .caret = 0,
        .pad = 8,
        .face = .sans,
        .focused = false,
        .on_submit = noteFieldSubmit,
    };
    var field_widget = widgetAt(.{ .x = 20, .y = 30, .w = 120, .h = 40 });
    try std.testing.expectEqual(abi.err.ok, abi.text_field.ra8_widget_text_field_init(&field_widget, &field));
    var kbd = keyboardAt(&full_backend, &full_ops);
    kbd.focused_field = &field_widget;
    var keyboard_widget = widgetAt(band);
    _ = abi.ra8_widget_keyboard_init(&keyboard_widget, &kbd);

    Engine.hit_answer = 0;
    const ignored_event = touchAt(5, 205);
    _ = abi.ra8_widget_keyboard_vtable().on_input.?(&keyboard_widget, &ignored_event);
    try std.testing.expectEqual(@as(u16, 0), field.len);
    const field_tap = touchAt(30, 40);
    try std.testing.expect(abi.text_field.ra8_widget_text_field_vtable().on_input.?(&field_widget, &field_tap));

    for (0..5) |index| {
        Engine.hit_answer = @intCast(index);
        const event = touchAt(5, 205);
        try std.testing.expect(abi.ra8_widget_keyboard_vtable().on_input.?(&keyboard_widget, &event));
    }
    try std.testing.expectEqualStrings("read", buffer[0..4]);
    try std.testing.expectEqual(@as(u16, 4), field.len);
    try std.testing.expectEqual(field_widget.rect.x + 8, field.damage.x);
    try std.testing.expectEqual(field_widget.rect.w - 16, field.damage.w);
    try std.testing.expectEqual(Engine.keys.get(4).rect, kbd.damage);

    Engine.hit_answer = 5;
    const backspace_event = touchAt(130, 205);
    _ = abi.ra8_widget_keyboard_vtable().on_input.?(&keyboard_widget, &backspace_event);
    try std.testing.expectEqualStrings("rea", buffer[0..3]);
    try std.testing.expectEqual(@as(u8, 0), buffer[3]);

    Engine.hit_answer = 6;
    Engine.commit_answer = true;
    const enter_event = touchAt(160, 205);
    _ = abi.ra8_widget_keyboard_vtable().on_input.?(&keyboard_widget, &enter_event);
    try std.testing.expect(field.submitted);
    try std.testing.expectEqual(@as(u32, 1), field_submits);
}
