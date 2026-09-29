//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the pure layout and typing core. The rectangle hit test arrives
//! as a function pointer, so this file binds a local copy of the `ra8_ui`
//! contract and never links the real library.

const std = @import("std");
const core = @import("implementation");

fn containsPure(r: ?*const core.Rect, px: i32, py: i32) callconv(.c) bool {
    const rect = r orelse return false;
    const left: i64 = rect.x;
    const top: i64 = rect.y;
    const right: i64 = left + @as(i64, rect.w);
    const bottom: i64 = top + @as(i64, rect.h);
    const point_x: i64 = px;
    const point_y: i64 = py;
    return (point_x >= left) and (point_x < right) and (point_y >= top) and (point_y < bottom);
}

const frame_x: i32 = 0;
const frame_y: i32 = 600;
const frame_w: i32 = 1024;
const frame_h: i32 = 360;

fn testFrame() core.Rect {
    return .{ .x = frame_x, .y = frame_y, .w = frame_w, .h = frame_h };
}

fn laidOut() core.Layout {
    var kb = std.mem.zeroes(core.Layout);
    const frame = testFrame();
    core.layoutInit(&kb, &frame) catch unreachable;
    return kb;
}

fn onLayer(layer: core.Layer) core.Layout {
    var kb = laidOut();
    kb.layer = @intFromEnum(layer);
    core.buildLayer(&kb);
    return kb;
}

fn indexOfChar(kb: *const core.Layout, ch: u8) ?u8 {
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        const key = kb.keys[index];
        if ((key.kind == @intFromEnum(core.KeyKind.char)) and (key.ch_lower == ch)) return index;
    }
    return null;
}

fn indexOfKind(kb: *const core.Layout, kind: core.KeyKind) ?u8 {
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        if (kb.keys[index].kind == @intFromEnum(kind)) return index;
    }
    return null;
}

fn indexOfLayerKey(kb: *const core.Layout, aux: core.Layer) ?u8 {
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        const key = kb.keys[index];
        if ((key.kind == @intFromEnum(core.KeyKind.layer)) and (key.aux == @intFromEnum(aux))) {
            return index;
        }
    }
    return null;
}

fn typed(t: *const core.Text) []const u8 {
    return std.mem.sliceTo(&t.buf, 0);
}

test "the ABI layouts keep the sizes and offsets the C header published" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(core.Rect));
    try std.testing.expectEqual(@as(usize, 20), @sizeOf(core.Key));
    try std.testing.expectEqual(@as(usize, 820), @sizeOf(core.Layout));
    try std.testing.expectEqual(@as(usize, 66), @sizeOf(core.Text));
    try std.testing.expectEqual(@as(usize, 800), @offsetOf(core.Layout, "count"));
    try std.testing.expectEqual(@as(usize, 804), @offsetOf(core.Layout, "frame"));
    try std.testing.expectEqual(@as(usize, 64), @offsetOf(core.Text, "len"));
}

test "the half-unit grid spans exactly the frame width" {
    const frame = testFrame();
    try std.testing.expectEqual(@as(i32, 0), core.hx(&frame, 0));
    try std.testing.expectEqual(@as(i32, 1024), core.hx(&frame, core.hu_div));
}

test "half-unit positions divide truncating, as the C did" {
    const frame = testFrame();
    try std.testing.expectEqual(@as(i32, 51), core.hx(&frame, 1));
    try std.testing.expectEqual(@as(i32, 102), core.hx(&frame, 2));
    try std.testing.expectEqual(@as(i32, 153), core.hx(&frame, 3));
    try std.testing.expectEqual(@as(i32, 256), core.hx(&frame, 5));
    try std.testing.expectEqual(@as(i32, 819), core.hx(&frame, 16));
}

test "half-unit positions are relative to the frame origin" {
    const frame = core.Rect{ .x = 40, .y = 0, .w = 200, .h = 80 };
    try std.testing.expectEqual(@as(i32, 40), core.hx(&frame, 0));
    try std.testing.expectEqual(@as(i32, 60), core.hx(&frame, 2));
    try std.testing.expectEqual(@as(i32, 240), core.hx(&frame, core.hu_div));
}

test "a frame wider than the int32 product saturates instead of wrapping" {
    const frame = core.Rect{ .x = 0, .y = 0, .w = std.math.maxInt(i32), .h = 80 };
    try std.testing.expectEqual(std.math.maxInt(i32), core.hx(&frame, core.hu_div));
    try std.testing.expect(core.hx(&frame, 2) > 0);
}

test "a negative frame width truncates toward zero" {
    const frame = core.Rect{ .x = 0, .y = 0, .w = -21, .h = 80 };
    try std.testing.expectEqual(@as(i32, -2), core.hx(&frame, 2));
}

test "add appends one fully initialised key" {
    var kb = std.mem.zeroes(core.Layout);
    core.add(&kb, 5, 10, 20, 30, 'a', 'A', .char, 0);
    try std.testing.expectEqual(@as(u8, 1), kb.count);
    try std.testing.expectEqual(@as(i32, 5), kb.keys[0].rect.x);
    try std.testing.expectEqual(@as(i32, 10), kb.keys[0].rect.w);
    try std.testing.expectEqual(@as(i32, 20), kb.keys[0].rect.y);
    try std.testing.expectEqual(@as(i32, 30), kb.keys[0].rect.h);
    try std.testing.expectEqual(@as(u8, 'a'), kb.keys[0].ch_lower);
    try std.testing.expectEqual(@as(u8, 'A'), kb.keys[0].ch_upper);
}

test "add silently discards a key once the layout is full" {
    var kb = std.mem.zeroes(core.Layout);
    var placed: usize = 0;
    while (placed < core.max_keys + 5) : (placed += 1) {
        core.add(&kb, 0, 1, 0, 1, 'x', 'X', .char, 0);
    }
    try std.testing.expectEqual(@as(u8, core.max_keys), kb.count);
}

test "place lays n keys side by side with no gaps" {
    var kb = std.mem.zeroes(core.Layout);
    const frame = testFrame();
    core.place(&kb, "abc", "ABC", 3, 0, &frame, 10, 90);
    try std.testing.expectEqual(@as(u8, 3), kb.count);
    var index: usize = 1;
    while (index < kb.count) : (index += 1) {
        const previous = kb.keys[index - 1].rect;
        try std.testing.expectEqual(previous.x + previous.w, kb.keys[index].rect.x);
    }
    try std.testing.expectEqual(@as(u8, 'B'), kb.keys[1].ch_upper);
}

test "place with no shifted row mirrors the unshifted glyph" {
    var kb = std.mem.zeroes(core.Layout);
    const frame = testFrame();
    core.place(&kb, "12", null, 2, 0, &frame, 10, 90);
    try std.testing.expectEqual(@as(u8, '1'), kb.keys[0].ch_upper);
    try std.testing.expectEqual(@as(u8, '2'), kb.keys[1].ch_upper);
}

test "span covers its half-unit range and carries no glyphs" {
    var kb = std.mem.zeroes(core.Layout);
    const frame = testFrame();
    core.span(&kb, 4, 16, &frame, 10, 90, .space, 0);
    try std.testing.expectEqual(@as(i32, 204), kb.keys[0].rect.x);
    try std.testing.expectEqual(@as(i32, 615), kb.keys[0].rect.w);
    try std.testing.expectEqual(@as(u8, 0), kb.keys[0].ch_lower);
    try std.testing.expectEqual(@as(u8, 0), kb.keys[0].ch_upper);
}

test "the letters layer lays 31 keys" {
    const kb = laidOut();
    try std.testing.expectEqual(@as(u8, 31), kb.count);
    try std.testing.expectEqual(@intFromEnum(core.Layer.letters), kb.layer);
    try std.testing.expect(!kb.shift);
}

test "the letters layer carries QWERTY in both cases" {
    const kb = laidOut();
    try std.testing.expectEqual(@as(u8, 'q'), kb.keys[0].ch_lower);
    try std.testing.expectEqual(@as(u8, 'Q'), kb.keys[0].ch_upper);
    try std.testing.expectEqual(@as(u8, 'a'), kb.keys[10].ch_lower);
    try std.testing.expectEqual(@as(u8, 'L'), kb.keys[18].ch_upper);
    try std.testing.expectEqual(@as(u8, 'z'), kb.keys[20].ch_lower);
    try std.testing.expectEqual(@as(u8, 'M'), kb.keys[26].ch_upper);
}

test "the letters layer puts SHIFT and BACKSPACE around row two" {
    const kb = laidOut();
    try std.testing.expectEqual(@intFromEnum(core.KeyKind.shift), kb.keys[19].kind);
    try std.testing.expectEqual(@intFromEnum(core.KeyKind.backspace), kb.keys[27].kind);
    try std.testing.expectEqual(@as(i32, 153), kb.keys[19].rect.w);
}

test "the letters bottom row is 123, SPACE, RETURN" {
    const kb = laidOut();
    try std.testing.expectEqual(@intFromEnum(core.KeyKind.layer), kb.keys[28].kind);
    try std.testing.expectEqual(@intFromEnum(core.Layer.numbers), kb.keys[28].aux);
    try std.testing.expectEqual(@intFromEnum(core.KeyKind.space), kb.keys[29].kind);
    try std.testing.expectEqual(@intFromEnum(core.KeyKind.enter), kb.keys[30].kind);
}

test "digits are not reachable on the letters layer" {
    const kb = laidOut();
    try std.testing.expect(indexOfChar(&kb, '9') == null);
}

test "every letters key sits inside the frame" {
    const kb = laidOut();
    var index: usize = 0;
    while (index < kb.count) : (index += 1) {
        const rect = kb.keys[index].rect;
        try std.testing.expect(rect.x >= frame_x);
        try std.testing.expect((rect.x + rect.w) <= (frame_x + frame_w));
        try std.testing.expect(rect.y >= frame_y);
        try std.testing.expect((rect.y + rect.h) <= (frame_y + frame_h));
    }
}

test "rows stack at multiples of the row height" {
    const kb = laidOut();
    const row_height = @divTrunc(frame_h, core.rows);
    try std.testing.expectEqual(frame_y, kb.keys[0].rect.y);
    try std.testing.expectEqual(frame_y + row_height, kb.keys[10].rect.y);
    try std.testing.expectEqual(frame_y + (2 * row_height), kb.keys[19].rect.y);
    try std.testing.expectEqual(frame_y + (3 * row_height), kb.keys[28].rect.y);
    try std.testing.expectEqual(row_height, kb.keys[0].rect.h);
}

test "the row height truncates rather than rounds" {
    var kb = std.mem.zeroes(core.Layout);
    const frame = core.Rect{ .x = 0, .y = 0, .w = 1024, .h = 362 };
    try core.layoutInit(&kb, &frame);
    try std.testing.expectEqual(@as(i32, 90), kb.keys[0].rect.h);
}

test "the numbers layer lays 30 keys with digits on top" {
    const kb = onLayer(.numbers);
    try std.testing.expectEqual(@as(u8, 30), kb.count);
    try std.testing.expectEqual(@as(u8, '1'), kb.keys[0].ch_lower);
    try std.testing.expectEqual(@as(u8, '0'), kb.keys[9].ch_lower);
}

test "the numbers second row carries the common symbols unshifted" {
    const kb = onLayer(.numbers);
    try std.testing.expectEqual(@as(u8, '-'), kb.keys[10].ch_lower);
    try std.testing.expectEqual(@as(u8, '"'), kb.keys[19].ch_lower);
    try std.testing.expectEqual(@as(u8, '"'), kb.keys[19].ch_upper);
}

test "the numbers punctuation row toggles to symbols" {
    const kb = onLayer(.numbers);
    try std.testing.expectEqual(@intFromEnum(core.KeyKind.layer), kb.keys[20].kind);
    try std.testing.expectEqual(@intFromEnum(core.Layer.symbols), kb.keys[20].aux);
    try std.testing.expectEqual(@as(u8, '.'), kb.keys[21].ch_lower);
    try std.testing.expectEqual(@as(u8, '\''), kb.keys[25].ch_lower);
    try std.testing.expectEqual(@intFromEnum(core.KeyKind.backspace), kb.keys[26].kind);
}

test "the symbols layer lays 27 keys" {
    const kb = onLayer(.symbols);
    try std.testing.expectEqual(@as(u8, 27), kb.count);
    try std.testing.expectEqual(@as(u8, '['), kb.keys[0].ch_lower);
    try std.testing.expectEqual(@as(u8, '='), kb.keys[9].ch_lower);
}

test "the symbols second row is centred on the grid" {
    const kb = onLayer(.symbols);
    try std.testing.expectEqual(@as(u8, '<'), kb.keys[10].ch_lower);
    try std.testing.expectEqual(@as(u8, '~'), kb.keys[16].ch_lower);
    try std.testing.expectEqual(core.hx(&kb.frame, core.sym1_hu0), kb.keys[10].rect.x);
}

test "the symbols punctuation row toggles back to numbers" {
    const kb = onLayer(.symbols);
    try std.testing.expectEqual(@intFromEnum(core.Layer.numbers), kb.keys[17].aux);
    try std.testing.expectEqual(@intFromEnum(core.Layer.letters), kb.keys[24].aux);
}

test "rebuilding a layer resets the key count first" {
    var kb = laidOut();
    kb.count = 7;
    core.buildLayer(&kb);
    try std.testing.expectEqual(@as(u8, 31), kb.count);
}

test "an unknown layer byte falls back to letters" {
    var kb = laidOut();
    kb.layer = 99;
    core.buildLayer(&kb);
    try std.testing.expectEqual(@as(u8, 31), kb.count);
    try std.testing.expectEqual(@as(u8, 'q'), kb.keys[0].ch_lower);
}

test "layoutInit refuses a frame with no width" {
    var kb = std.mem.zeroes(core.Layout);
    const frame = core.Rect{ .x = 0, .y = 0, .w = 0, .h = frame_h };
    try std.testing.expectError(error.NoArea, core.layoutInit(&kb, &frame));
}

test "layoutInit refuses a frame with no height" {
    var kb = std.mem.zeroes(core.Layout);
    const frame = core.Rect{ .x = 0, .y = 0, .w = frame_w, .h = 0 };
    try std.testing.expectError(error.NoArea, core.layoutInit(&kb, &frame));
}

test "layoutInit refuses a negative frame" {
    var kb = std.mem.zeroes(core.Layout);
    const frame = core.Rect{ .x = 0, .y = 0, .w = -1, .h = -1 };
    try std.testing.expectError(error.NoArea, core.layoutInit(&kb, &frame));
}

test "layoutInit clears SHIFT and stores the frame" {
    var kb = std.mem.zeroes(core.Layout);
    kb.shift = true;
    const frame = testFrame();
    try core.layoutInit(&kb, &frame);
    try std.testing.expect(!kb.shift);
    try std.testing.expectEqual(frame_w, kb.frame.w);
    try std.testing.expectEqual(frame_y, kb.frame.y);
}

test "hit finds the key under the centre of its rectangle" {
    const kb = laidOut();
    var index: u8 = 0;
    while (index < kb.count) : (index += 1) {
        const rect = kb.keys[index].rect;
        const cx = rect.x + @divTrunc(rect.w, 2);
        const cy = rect.y + @divTrunc(rect.h, 2);
        try std.testing.expectEqual(index, core.hit(&kb, cx, cy, containsPure));
    }
}

test "hit reports no key for a point outside the frame" {
    const kb = laidOut();
    try std.testing.expectEqual(core.no_hit, core.hit(&kb, -100, -100, containsPure));
}

test "hit returns the first matching key when rectangles overlap" {
    var kb = std.mem.zeroes(core.Layout);
    core.add(&kb, 0, 100, 0, 100, 'a', 'A', .char, 0);
    core.add(&kb, 0, 100, 0, 100, 'b', 'B', .char, 0);
    try std.testing.expectEqual(@as(u8, 0), core.hit(&kb, 50, 50, containsPure));
}

test "hit on an empty layout reports no key" {
    const kb = std.mem.zeroes(core.Layout);
    try std.testing.expectEqual(core.no_hit, core.hit(&kb, 0, 0, containsPure));
}

test "glyphOf tracks the SHIFT flag" {
    var kb = laidOut();
    const q = indexOfChar(&kb, 'q').?;
    try std.testing.expectEqual(@as(u8, 'q'), core.glyphOf(&kb, q));
    kb.shift = true;
    try std.testing.expectEqual(@as(u8, 'Q'), core.glyphOf(&kb, q));
}

test "glyphOf reports nothing for a special key" {
    const kb = laidOut();
    const enter = indexOfKind(&kb, .enter).?;
    try std.testing.expectEqual(@as(u8, 0), core.glyphOf(&kb, enter));
}

test "glyphOf reports nothing past the key count" {
    const kb = laidOut();
    try std.testing.expectEqual(@as(u8, 0), core.glyphOf(&kb, kb.count));
    try std.testing.expectEqual(@as(u8, 0), core.glyphOf(&kb, core.no_hit));
}

test "textInit empties the buffer and clears the commit flag" {
    var t = std.mem.zeroes(core.Text);
    t.len = 9;
    t.committed = true;
    core.textInit(&t);
    try std.testing.expectEqual(@as(u8, 0), t.len);
    try std.testing.expectEqual(@as(u8, 0), t.buf[0]);
    try std.testing.expect(!t.committed);
}

test "append keeps the buffer NUL terminated" {
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    core.append(&t, 'h');
    core.append(&t, 'i');
    try std.testing.expectEqualStrings("hi", typed(&t));
    try std.testing.expectEqual(@as(u8, 2), t.len);
}

test "append stops one byte short of the buffer" {
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    var written: usize = 0;
    while (written < core.text_max + 10) : (written += 1) {
        core.append(&t, 'x');
    }
    try std.testing.expectEqual(@as(u8, core.text_max - 1), t.len);
    try std.testing.expectEqual(@as(u8, 0), t.buf[core.text_max - 1]);
}

test "a character key appends and consumes the one-shot SHIFT" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    kb.shift = true;
    core.applyKey(&t, &kb, indexOfChar(&kb, 'h').?);
    core.applyKey(&t, &kb, indexOfChar(&kb, 'i').?);
    try std.testing.expectEqualStrings("Hi", typed(&t));
    try std.testing.expect(!kb.shift);
}

test "SPACE appends a blank and clears SHIFT" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    kb.shift = true;
    core.applyKey(&t, &kb, indexOfKind(&kb, .space).?);
    try std.testing.expectEqualStrings(" ", typed(&t));
    try std.testing.expect(!kb.shift);
}

test "BACKSPACE on an empty buffer is a no-op" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    core.applyKey(&t, &kb, indexOfKind(&kb, .backspace).?);
    try std.testing.expectEqual(@as(u8, 0), t.len);
}

test "BACKSPACE removes the last character" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    core.applyKey(&t, &kb, indexOfChar(&kb, 'a').?);
    core.applyKey(&t, &kb, indexOfChar(&kb, 'b').?);
    core.applyKey(&t, &kb, indexOfKind(&kb, .backspace).?);
    try std.testing.expectEqualStrings("a", typed(&t));
}

test "RETURN commits without touching the text" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    core.applyKey(&t, &kb, indexOfChar(&kb, 'a').?);
    core.applyKey(&t, &kb, indexOfKind(&kb, .enter).?);
    try std.testing.expect(t.committed);
    try std.testing.expectEqualStrings("a", typed(&t));
}

test "SHIFT toggles rather than latches" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    const shift = indexOfKind(&kb, .shift).?;
    core.applyKey(&t, &kb, shift);
    try std.testing.expect(kb.shift);
    core.applyKey(&t, &kb, shift);
    try std.testing.expect(!kb.shift);
}

test "a layer key switches the layer, clears SHIFT, and re-lays the grid" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    kb.shift = true;
    core.applyKey(&t, &kb, indexOfLayerKey(&kb, .numbers).?);
    try std.testing.expectEqual(@intFromEnum(core.Layer.numbers), kb.layer);
    try std.testing.expectEqual(@as(u8, 30), kb.count);
    try std.testing.expect(!kb.shift);
}

test "the no-hit sentinel and any index past the count are no-ops" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    core.applyKey(&t, &kb, core.no_hit);
    core.applyKey(&t, &kb, kb.count);
    try std.testing.expectEqual(@as(u8, 0), t.len);
    try std.testing.expectEqual(@as(u8, 31), kb.count);
}

test "typing across layers reproduces the C suite's sequence" {
    var kb = laidOut();
    var t = std.mem.zeroes(core.Text);
    core.textInit(&t);
    core.applyKey(&t, &kb, indexOfKind(&kb, .shift).?);
    core.applyKey(&t, &kb, indexOfChar(&kb, 'h').?);
    core.applyKey(&t, &kb, indexOfChar(&kb, 'i').?);
    core.applyKey(&t, &kb, indexOfKind(&kb, .space).?);
    core.applyKey(&t, &kb, indexOfLayerKey(&kb, .numbers).?);
    core.applyKey(&t, &kb, indexOfChar(&kb, '9').?);
    try std.testing.expectEqualStrings("Hi 9", typed(&t));
    core.applyKey(&t, &kb, indexOfLayerKey(&kb, .symbols).?);
    core.applyKey(&t, &kb, indexOfChar(&kb, '[').?);
    try std.testing.expectEqualStrings("Hi 9[", typed(&t));
    core.applyKey(&t, &kb, indexOfLayerKey(&kb, .numbers).?);
    try std.testing.expectEqual(@intFromEnum(core.Layer.numbers), kb.layer);
    core.applyKey(&t, &kb, indexOfLayerKey(&kb, .letters).?);
    try std.testing.expectEqual(@intFromEnum(core.Layer.letters), kb.layer);
    core.applyKey(&t, &kb, indexOfKind(&kb, .backspace).?);
    try std.testing.expectEqualStrings("Hi 9", typed(&t));
}

test "every printable ASCII symbol and digit is reachable across the layers" {
    const all = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~0123456789";
    for (all) |wanted| {
        var kb = laidOut();
        var found = indexOfChar(&kb, wanted) != null;
        if (!found) {
            kb.layer = @intFromEnum(core.Layer.numbers);
            core.buildLayer(&kb);
            found = indexOfChar(&kb, wanted) != null;
        }
        if (!found) {
            kb.layer = @intFromEnum(core.Layer.symbols);
            core.buildLayer(&kb);
            found = indexOfChar(&kb, wanted) != null;
        }
        try std.testing.expect(found);
    }
}

test "no glyph is reachable twice within one layer" {
    inline for (.{ core.Layer.letters, core.Layer.numbers, core.Layer.symbols }) |layer| {
        const kb = onLayer(layer);
        var outer: usize = 0;
        while (outer < kb.count) : (outer += 1) {
            if (kb.keys[outer].kind != @intFromEnum(core.KeyKind.char)) continue;
            var inner: usize = outer + 1;
            while (inner < kb.count) : (inner += 1) {
                if (kb.keys[inner].kind != @intFromEnum(core.KeyKind.char)) continue;
                try std.testing.expect(kb.keys[outer].ch_lower != kb.keys[inner].ch_lower);
            }
        }
    }
}
