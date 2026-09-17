//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the exported C ABI. This file supplies the two symbols the
//! library leaves undefined, `ra8_log_emit_error` and `ra8_ui_rect_contains`,
//! the same link-time substitution the real build performs against
//! `libs/ra8_core` and the `ra8_ui` archive, so the guard order, the log line
//! each guard emits, and every MC/DC vector the C suite drives are all covered
//! through the public entry points.

const std = @import("std");
const abi = @import("abi");

var log_calls: usize = 0;
var last_message: [*:0]const u8 = "";
var contains_calls: usize = 0;

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    log_calls += 1;
    last_message = message;
}

export fn ra8_ui_rect_contains(r: ?*const abi.Rect, px: i32, py: i32) callconv(.c) bool {
    contains_calls += 1;
    const rect = r orelse return false;
    const left: i64 = rect.x;
    const top: i64 = rect.y;
    const right: i64 = left + @as(i64, rect.w);
    const bottom: i64 = top + @as(i64, rect.h);
    const point_x: i64 = px;
    const point_y: i64 = py;
    return (point_x >= left) and (point_x < right) and (point_y >= top) and (point_y < bottom);
}

const ok: u16 = 0;
const invalid_arg: u16 = 0x103;
const null_ptr: u16 = 0x504;

const frame_x: i32 = 0;
const frame_y: i32 = 600;
const frame_w: i32 = 1024;
const frame_h: i32 = 360;

fn reset() void {
    log_calls = 0;
    contains_calls = 0;
    last_message = "";
}

fn testFrame() abi.Rect {
    return .{ .x = frame_x, .y = frame_y, .w = frame_w, .h = frame_h };
}

fn laidOut() abi.Layout {
    var kb = std.mem.zeroes(abi.Layout);
    const frame = testFrame();
    std.debug.assert(abi.ra8_kbd_layout_init(&kb, &frame) == ok);
    return kb;
}

fn expectMessage(expected: []const u8) !void {
    try std.testing.expectEqualStrings(expected, std.mem.span(last_message));
}

fn indexOfChar(kb: *const abi.Layout, ch: u8) u8 {
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        const key = kb.keys[index];
        if ((key.kind == @intFromEnum(abi.KeyKind.char)) and (key.ch_lower == ch)) return index;
    }
    return abi.no_hit;
}

fn indexOfKind(kb: *const abi.Layout, kind: abi.KeyKind) u8 {
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        if (kb.keys[index].kind == @intFromEnum(kind)) return index;
    }
    return abi.no_hit;
}

fn indexOfLayerKey(kb: *const abi.Layout, aux: abi.Layer) u8 {
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        const key = kb.keys[index];
        if ((key.kind == @intFromEnum(abi.KeyKind.layer)) and (key.aux == @intFromEnum(aux))) {
            return index;
        }
    }
    return abi.no_hit;
}

fn typed(t: *const abi.Text) []const u8 {
    return std.mem.sliceTo(&t.buf, 0);
}

test "the exported symbols keep the header's signatures" {
    try std.testing.expectEqual(u16, @typeInfo(@TypeOf(abi.ra8_kbd_layout_init)).@"fn".return_type.?);
    try std.testing.expectEqual(u8, @typeInfo(@TypeOf(abi.ra8_kbd_hit)).@"fn".return_type.?);
    try std.testing.expectEqual(u8, @typeInfo(@TypeOf(abi.ra8_kbd_key_glyph)).@"fn".return_type.?);
    try std.testing.expectEqual(u16, @typeInfo(@TypeOf(abi.ra8_kbd_text_init)).@"fn".return_type.?);
    try std.testing.expectEqual(u16, @typeInfo(@TypeOf(abi.ra8_kbd_apply)).@"fn".return_type.?);
}

test "layout_init rejects a null layout with the C's message" {
    reset();
    const frame = testFrame();
    try std.testing.expectEqual(null_ptr, abi.ra8_kbd_layout_init(null, &frame));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try expectMessage("kb must not be nullptr");
}

test "layout_init rejects a null frame with the C's message" {
    reset();
    var kb = std.mem.zeroes(abi.Layout);
    try std.testing.expectEqual(null_ptr, abi.ra8_kbd_layout_init(&kb, null));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try expectMessage("frame must not be nullptr");
}

test "layout_init checks the layout before the frame" {
    reset();
    try std.testing.expectEqual(null_ptr, abi.ra8_kbd_layout_init(null, null));
    try expectMessage("kb must not be nullptr");
}

test "layout_init lays 31 letter keys on an acceptable frame" {
    reset();
    var kb = std.mem.zeroes(abi.Layout);
    const frame = testFrame();
    try std.testing.expectEqual(ok, abi.ra8_kbd_layout_init(&kb, &frame));
    try std.testing.expectEqual(@as(u8, 31), kb.count);
    try std.testing.expectEqual(@intFromEnum(abi.Layer.letters), kb.layer);
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "frame-reject MC/DC V1: a real frame is accepted" {
    var kb = std.mem.zeroes(abi.Layout);
    const v1 = abi.Rect{ .x = frame_x, .y = frame_y, .w = frame_w, .h = frame_h };
    try std.testing.expectEqual(ok, abi.ra8_kbd_layout_init(&kb, &v1));
}

test "frame-reject MC/DC V2: zero width alone rejects" {
    reset();
    var kb = std.mem.zeroes(abi.Layout);
    const v2 = abi.Rect{ .x = frame_x, .y = frame_y, .w = 0, .h = frame_h };
    try std.testing.expectEqual(invalid_arg, abi.ra8_kbd_layout_init(&kb, &v2));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "frame-reject MC/DC V3: zero height alone rejects" {
    reset();
    var kb = std.mem.zeroes(abi.Layout);
    const v3 = abi.Rect{ .x = frame_x, .y = frame_y, .w = frame_w, .h = 0 };
    try std.testing.expectEqual(invalid_arg, abi.ra8_kbd_layout_init(&kb, &v3));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "a rejected frame leaves the previous layout untouched" {
    var kb = laidOut();
    const before = kb.count;
    const bad = abi.Rect{ .x = 0, .y = 0, .w = 0, .h = 0 };
    try std.testing.expectEqual(invalid_arg, abi.ra8_kbd_layout_init(&kb, &bad));
    try std.testing.expectEqual(before, kb.count);
    try std.testing.expectEqual(frame_w, kb.frame.w);
}

test "hit reports no key for a null layout and never calls the rectangle test" {
    reset();
    try std.testing.expectEqual(abi.no_hit, abi.ra8_kbd_hit(null, 0, 0));
    try std.testing.expectEqual(@as(usize, 0), contains_calls);
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "hit resolves the centre of every key through the rectangle seam" {
    const kb = laidOut();
    reset();
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        const rect = kb.keys[index].rect;
        const cx = rect.x + @divTrunc(rect.w, 2);
        const cy = rect.y + @divTrunc(rect.h, 2);
        try std.testing.expectEqual(index, abi.ra8_kbd_hit(&kb, cx, cy));
    }
    try std.testing.expect(contains_calls >= kb.count);
}

test "hit reports no key outside the frame" {
    const kb = laidOut();
    try std.testing.expectEqual(abi.no_hit, abi.ra8_kbd_hit(&kb, -100, -100));
}

test "hit scans every key before giving up" {
    const kb = laidOut();
    reset();
    try std.testing.expectEqual(abi.no_hit, abi.ra8_kbd_hit(&kb, -1, -1));
    try std.testing.expectEqual(@as(usize, kb.count), contains_calls);
}

test "key-glyph guard MC/DC V1: a laid-out key returns its glyph" {
    const kb = laidOut();
    const q = indexOfChar(&kb, 'q');
    try std.testing.expectEqual(@as(u8, 'q'), abi.ra8_kbd_key_glyph(&kb, q));
}

test "key-glyph guard MC/DC V2: a null layout returns nothing" {
    reset();
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_kbd_key_glyph(null, 0));
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "key-glyph guard MC/DC V3: an index at the count returns nothing" {
    const kb = laidOut();
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_kbd_key_glyph(&kb, kb.count));
}

test "key_glyph follows the SHIFT flag" {
    var kb = laidOut();
    const q = indexOfChar(&kb, 'q');
    try std.testing.expectEqual(@as(u8, 'q'), abi.ra8_kbd_key_glyph(&kb, q));
    kb.shift = true;
    try std.testing.expectEqual(@as(u8, 'Q'), abi.ra8_kbd_key_glyph(&kb, q));
}

test "key_glyph reports nothing for a special key" {
    const kb = laidOut();
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_kbd_key_glyph(&kb, indexOfKind(&kb, .enter)));
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_kbd_key_glyph(&kb, indexOfKind(&kb, .shift)));
    try std.testing.expectEqual(@as(u8, 0), abi.ra8_kbd_key_glyph(&kb, indexOfKind(&kb, .space)));
}

test "text_init rejects a null buffer with the C's message" {
    reset();
    try std.testing.expectEqual(null_ptr, abi.ra8_kbd_text_init(null));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try expectMessage("t must not be nullptr");
}

test "text_init empties the buffer" {
    var t = std.mem.zeroes(abi.Text);
    t.len = 12;
    t.committed = true;
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(@as(u8, 0), t.len);
    try std.testing.expect(!t.committed);
    try std.testing.expectEqualStrings("", typed(&t));
}

test "apply rejects a null text buffer first" {
    reset();
    var kb = laidOut();
    try std.testing.expectEqual(null_ptr, abi.ra8_kbd_apply(null, &kb, 0));
    try expectMessage("t must not be nullptr");
}

test "apply rejects a null layout with its own message" {
    reset();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(null_ptr, abi.ra8_kbd_apply(&t, null, 0));
    try expectMessage("kb must not be nullptr");
}

test "apply checks the text buffer before the layout" {
    reset();
    try std.testing.expectEqual(null_ptr, abi.ra8_kbd_apply(null, null, 0));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try expectMessage("t must not be nullptr");
}

test "apply accepts the no-hit sentinel as a no-op" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    reset();
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, abi.no_hit));
    try std.testing.expectEqual(@as(u8, 0), t.len);
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "apply types lowercase through the public API" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'h')));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'i')));
    try std.testing.expectEqualStrings("hi", typed(&t));
}

test "one-shot SHIFT capitalises only the next character" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfKind(&kb, .shift)));
    try std.testing.expect(kb.shift);
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'h')));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'i')));
    try std.testing.expect(!kb.shift);
    try std.testing.expectEqualStrings("Hi", typed(&t));
}

test "the layer keys walk letters, numbers, and symbols" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfLayerKey(&kb, .numbers)));
    try std.testing.expectEqual(@intFromEnum(abi.Layer.numbers), kb.layer);
    try std.testing.expectEqual(@as(u8, 30), kb.count);
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfLayerKey(&kb, .symbols)));
    try std.testing.expectEqual(@intFromEnum(abi.Layer.symbols), kb.layer);
    try std.testing.expectEqual(@as(u8, 27), kb.count);
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfLayerKey(&kb, .letters)));
    try std.testing.expectEqual(@intFromEnum(abi.Layer.letters), kb.layer);
    try std.testing.expectEqual(@as(u8, 31), kb.count);
}

test "the C suite's full typing sequence lands through the ABI" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfKind(&kb, .shift)));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'h')));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'i')));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfKind(&kb, .space)));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfLayerKey(&kb, .numbers)));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, '9')));
    try std.testing.expectEqualStrings("Hi 9", typed(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfLayerKey(&kb, .symbols)));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, '[')));
    try std.testing.expectEqualStrings("Hi 9[", typed(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfLayerKey(&kb, .numbers)));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfLayerKey(&kb, .letters)));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfKind(&kb, .backspace)));
    try std.testing.expectEqualStrings("Hi 9", typed(&t));
    try std.testing.expect(!t.committed);
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfKind(&kb, .enter)));
    try std.testing.expect(t.committed);
}

test "BACKSPACE on an empty buffer stays at zero length" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfKind(&kb, .backspace)));
    try std.testing.expectEqual(@as(u8, 0), t.len);
}

test "the text buffer stops one byte short under a long press run" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    const a = indexOfChar(&kb, 'a');
    var round: usize = 0;
    while (round < 200) : (round += 1) {
        try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, a));
    }
    try std.testing.expectEqual(@as(u8, 63), t.len);
    try std.testing.expectEqual(@as(usize, 63), typed(&t).len);
}

test "SPACE and a character both clear a pending SHIFT" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    kb.shift = true;
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfKind(&kb, .space)));
    try std.testing.expect(!kb.shift);
    kb.shift = true;
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'a')));
    try std.testing.expect(!kb.shift);
    try std.testing.expectEqualStrings(" A", typed(&t));
}

test "a tap resolved by hit applies to the key under the finger" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    const q = indexOfChar(&kb, 'q');
    const rect = kb.keys[q].rect;
    const found = abi.ra8_kbd_hit(&kb, rect.x + @divTrunc(rect.w, 2), rect.y + @divTrunc(rect.h, 2));
    try std.testing.expectEqual(q, found);
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, found));
    try std.testing.expectEqualStrings("q", typed(&t));
}

test "no guard on the happy path emits a log line" {
    var kb = laidOut();
    var t = std.mem.zeroes(abi.Text);
    reset();
    try std.testing.expectEqual(ok, abi.ra8_kbd_text_init(&t));
    try std.testing.expectEqual(ok, abi.ra8_kbd_apply(&t, &kb, indexOfChar(&kb, 'a')));
    _ = abi.ra8_kbd_key_glyph(&kb, 0);
    _ = abi.ra8_kbd_hit(&kb, 0, frame_y);
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}
