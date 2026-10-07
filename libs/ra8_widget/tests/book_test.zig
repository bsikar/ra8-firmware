//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for the book-grid membrane. The tiling helpers are pure, so the
//! grid maths is checked directly; everything the widget draws or routes is
//! checked through a recording paint backend and a recording `on_open`.

const std = @import("std");
const abi = @import("abi");

/// Link-time substitutes for the C symbols this library still owns in
/// `src/ra8_widget.c` and `libs/ra8_ui`.
var last_message: ?[*:0]const u8 = null;
var invalidations: u32 = 0;
var last_refresh: u8 = 0xFF;

export fn ra8_log_emit_error(_: [*:0]const u8, message: [*:0]const u8) void {
    last_message = message;
}

export fn ra8_widget_invalidate(w: *abi.Widget, refresh: abi.Refresh) callconv(.c) u16 {
    invalidations += 1;
    last_refresh = @backingInt(refresh);
    w.dirty = true;
    w.refresh = @backingInt(refresh);
    return abi.err.ok;
}

export fn ra8_ui_rect_contains(r: *const abi.Rect, px: i32, py: i32) callconv(.c) bool {
    return px >= r.x and px < r.x + r.w and py >= r.y and py < r.y + r.h;
}

/// One recorded draw call, so a test can say what was painted where.
const Fill = struct { x: i32, y: i32, w: i32, h: i32, color: u32 };
const Text = struct { x: i32, y: i32, str: [*:0]const u8, fg: u32, bg: u32 };

var fills: [64]Fill = undefined;
var fill_count: usize = 0;
var texts: [32]Text = undefined;
var text_count: usize = 0;
var opened: [8]u16 = undefined;
var open_count: usize = 0;

fn recordFill(_: ?*anyopaque, x: i32, y: i32, w: i32, h: i32, color: u32) callconv(.c) void {
    fills[fill_count] = .{ .x = x, .y = y, .w = w, .h = h, .color = color };
    fill_count += 1;
}

fn recordText(_: ?*anyopaque, x: i32, y: i32, str: [*:0]const u8, fg: u32, bg: u32) callconv(.c) void {
    texts[text_count] = .{ .x = x, .y = y, .str = str, .fg = fg, .bg = bg };
    text_count += 1;
}

fn measure(_: ?*anyopaque, str: [*:0]const u8, out_w: *i32, out_h: *i32) callconv(.c) void {
    out_w.* = @intCast(std.mem.len(str) * 8);
    out_h.* = 12;
}

fn recordOpen(_: *abi.Widget, index: u16) callconv(.c) void {
    opened[open_count] = index;
    open_count += 1;
}

fn reset() void {
    last_message = null;
    invalidations = 0;
    last_refresh = 0xFF;
    fill_count = 0;
    text_count = 0;
    open_count = 0;
}

const full_paint: abi.Paint = .{
    .user = null,
    .fill_rect = recordFill,
    .draw_text = recordText,
    .text_size = measure,
};

const books = [_]abi.Book{
    .{ .title = "Dune", .author = "Herbert", .cover = 0x00806040, .percent = 42 },
    .{ .title = "1984", .author = "Orwell", .cover = 0x00404060, .percent = 100 },
    .{ .title = "Emma", .author = "Austen", .cover = 0x00204080, .percent = 0 },
};

/// A 200x200 grid of the three books above, two columns, 8 px pad and gap.
fn gridOf(count: u16, cols: u16) abi.BookGrid {
    return .{
        .paint = &full_paint,
        .books = &books,
        .on_open = recordOpen,
        .bg = 0x00FFFFFF,
        .title_fg = 0x00000000,
        .author_fg = 0x00555555,
        .bar_track = 0x00DDDDDD,
        .bar_fill = 0x00228822,
        .count = count,
        .cols = cols,
        .selected = 0,
        .pad = 8,
        .gap = 8,
        .label_h = 16,
        .bar_h = 4,
    };
}

fn widgetOf(grid: *abi.BookGrid) abi.Widget {
    var w: abi.Widget = std.mem.zeroes(abi.Widget);
    _ = abi.ra8_widget_book_grid_init(&w, grid);
    w.rect = .{ .x = 0, .y = 0, .w = 200, .h = 200 };
    return w;
}

fn touchAt(x: i32, y: i32) abi.Event {
    return .{ .kind = .touch, .reserved = 0, .button_id = 0, .x = x, .y = y };
}

test "the descriptor matches the C layout the header publishes" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(0, @offsetOf(abi.BookGrid, "paint"));
    try std.testing.expectEqual(ptr, @offsetOf(abi.BookGrid, "books"));
    try std.testing.expectEqual(2 * ptr, @offsetOf(abi.BookGrid, "on_open"));
    try std.testing.expectEqual(3 * ptr + 20, @offsetOf(abi.BookGrid, "count"));
    try std.testing.expectEqual(3 * ptr + 32, @offsetOf(abi.BookGrid, "bar_h"));
    try std.testing.expectEqual(2 * ptr + 4, @offsetOf(abi.Book, "percent"));
}

test "the vtable renders and routes but does not measure" {
    const vt = abi.ra8_widget_book_grid_vtable();
    try std.testing.expect(vt.measure == null);
    try std.testing.expect(vt.render != null);
    try std.testing.expect(vt.on_input != null);
    try std.testing.expectEqual(vt, abi.ra8_widget_book_grid_vtable());
}

test "init binds the vtable, the context and visibility" {
    reset();
    var grid = gridOf(3, 2);
    var w: abi.Widget = std.mem.zeroes(abi.Widget);
    try std.testing.expectEqual(abi.err.ok, abi.ra8_widget_book_grid_init(&w, &grid));
    try std.testing.expectEqual(abi.ra8_widget_book_grid_vtable(), w.vt.?);
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&grid)), w.ctx);
    try std.testing.expect(w.visible);
}

test "init refuses a null widget or a null descriptor" {
    reset();
    var grid = gridOf(3, 2);
    var w: abi.Widget = std.mem.zeroes(abi.Widget);
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_book_grid_init(null, &grid));
    try std.testing.expectEqual(abi.err.null_ptr, abi.ra8_widget_book_grid_init(&w, null));
    try std.testing.expect(last_message != null);
}

test "the content rect is the grid inset by its padding on every side" {
    const content = abi.contentRect(.{ .x = 10, .y = 20, .w = 200, .h = 100 }, 8);
    try std.testing.expectEqual(18, content.x);
    try std.testing.expectEqual(28, content.y);
    try std.testing.expectEqual(184, content.w);
    try std.testing.expectEqual(84, content.h);
}

test "a zero padding leaves the content rect the grid rect" {
    const grid: abi.Rect = .{ .x = 3, .y = 4, .w = 50, .h = 60 };
    const content = abi.contentRect(grid, 0);
    try std.testing.expectEqual(grid, content);
}

test "rows round up so a partial final row still gets height" {
    try std.testing.expectEqual(1, abi.rowsFor(1, 2));
    try std.testing.expectEqual(1, abi.rowsFor(2, 2));
    try std.testing.expectEqual(2, abi.rowsFor(3, 2));
    try std.testing.expectEqual(2, abi.rowsFor(4, 2));
    try std.testing.expectEqual(3, abi.rowsFor(5, 2));
    try std.testing.expectEqual(1, abi.rowsFor(7, 7));
    try std.testing.expectEqual(7, abi.rowsFor(7, 1));
}

test "cells tile the content rect in reading order" {
    const content: abi.Rect = .{ .x = 0, .y = 0, .w = 108, .h = 108 };
    const a = abi.cellRect(content, 0, 2, 2, 8);
    const b = abi.cellRect(content, 1, 2, 2, 8);
    const c = abi.cellRect(content, 2, 2, 2, 8);

    try std.testing.expectEqual(50, a.w);
    try std.testing.expectEqual(50, a.h);
    try std.testing.expectEqual(0, a.x);
    try std.testing.expectEqual(0, a.y);
    try std.testing.expectEqual(58, b.x);
    try std.testing.expectEqual(0, b.y);
    try std.testing.expectEqual(0, c.x);
    try std.testing.expectEqual(58, c.y);
}

test "a one-column grid stacks every card in the same column" {
    const content: abi.Rect = .{ .x = 5, .y = 5, .w = 100, .h = 100 };
    for (0..4) |i| {
        const cell = abi.cellRect(content, @intCast(i), 1, 4, 0);
        try std.testing.expectEqual(5, cell.x);
        try std.testing.expectEqual(100, cell.w);
        try std.testing.expectEqual(5 + @as(i32, @intCast(i)) * 25, cell.y);
    }
}

test "the gap is taken out of the cells, not added to the grid" {
    const content: abi.Rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };
    const last = abi.cellRect(content, 1, 2, 1, 10);
    try std.testing.expectEqual(45, last.w);
    try std.testing.expectEqual(55, last.x);
    try std.testing.expectEqual(100, last.x + last.w);
}

test "a cols of zero is read as one column" {
    var grid = gridOf(3, 0);
    try std.testing.expectEqual(1, grid.columns());
    var one = gridOf(3, 1);
    try std.testing.expectEqual(1, one.columns());
    var four = gridOf(3, 4);
    try std.testing.expectEqual(4, four.columns());
}

test "the card array becomes a bounded slice, or null when there is none" {
    var grid = gridOf(3, 2);
    try std.testing.expectEqual(3, grid.cards().?.len);

    var empty = gridOf(0, 2);
    try std.testing.expect(empty.cards() == null);

    var headless = gridOf(3, 2);
    headless.books = null;
    try std.testing.expect(headless.cards() == null);
}

test "render fills the background then paints every card" {
    reset();
    var grid = gridOf(3, 2);
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(0, fills[0].x);
    try std.testing.expectEqual(200, fills[0].w);
    try std.testing.expectEqual(grid.bg, fills[0].color);
    try std.testing.expectEqual(6, text_count);
    try std.testing.expectEqual(books[0].title.?, texts[0].str);
    try std.testing.expectEqual(books[0].author.?, texts[1].str);
    try std.testing.expectEqual(books[2].author.?, texts[5].str);
}

test "each card stacks a cover, two label rows and a progress bar" {
    reset();
    var grid = gridOf(1, 1);
    var w = widgetOf(&grid);
    w.rect = .{ .x = 0, .y = 0, .w = 100, .h = 100 };
    w.vt.?.render.?(&w);

    // background, then cover, then the bar track, then the bar fill
    const cover = fills[1];
    const track = fills[2];
    const fill = fills[3];
    try std.testing.expectEqual(books[0].cover, cover.color);
    try std.testing.expectEqual(8, cover.y);
    try std.testing.expectEqual(48, cover.h);
    try std.testing.expectEqual(grid.bar_track, track.color);
    try std.testing.expectEqual(4, track.h);
    try std.testing.expectEqual(88, track.y);
    try std.testing.expectEqual(grid.bar_fill, fill.color);
    try std.testing.expectEqual(35, fill.w);
}

test "a full bar fills the whole card width and an empty one paints nothing" {
    reset();
    var grid = gridOf(2, 1);
    grid.books = books[1..];
    var w = widgetOf(&grid);
    w.rect = .{ .x = 0, .y = 0, .w = 100, .h = 200 };
    w.vt.?.render.?(&w);

    // card 0 is 100%, card 1 is 0%. The bar spans the card, and the card is the
    // content rect (the 100-wide grid inset by its 8px padding), so a full bar
    // is 84 wide. The empty one is not painted at all.
    var full_fills: u32 = 0;
    var bar_fills: u32 = 0;
    for (fills[0..fill_count]) |f| {
        if (f.color != grid.bar_fill) continue;
        bar_fills += 1;
        if (f.w == 84) full_fills += 1;
    }
    try std.testing.expectEqual(1, full_fills);
    try std.testing.expectEqual(1, bar_fills);
}

test "a card too short for a cover skips the cover fill" {
    reset();
    var grid = gridOf(1, 1);
    var w = widgetOf(&grid);
    w.rect = .{ .x = 0, .y = 0, .w = 100, .h = 50 };
    w.vt.?.render.?(&w);

    for (fills[0..fill_count]) |f| {
        try std.testing.expect(f.color != books[0].cover);
    }
}

test "a record with no title or author draws only the rows it has" {
    reset();
    const sparse = [_]abi.Book{.{ .title = null, .author = "Anon", .cover = 0x11, .percent = 10 }};
    var grid = gridOf(1, 1);
    grid.books = &sparse;
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(1, text_count);
    try std.testing.expectEqual(sparse[0].author.?, texts[0].str);
}

test "the labels are drawn against the card's own cover colour" {
    reset();
    var grid = gridOf(1, 1);
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(grid.title_fg, texts[0].fg);
    try std.testing.expectEqual(books[0].cover, texts[0].bg);
    try std.testing.expectEqual(grid.author_fg, texts[1].fg);
    try std.testing.expectEqual(books[0].cover, texts[1].bg);
}

test "a backend with no draw_text still paints the covers and the bars" {
    reset();
    var textless = full_paint;
    textless.draw_text = null;
    var grid = gridOf(2, 2);
    grid.paint = &textless;
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(0, text_count);
    try std.testing.expect(fill_count > 1);
}

test "render paints nothing at all without a paint backend" {
    reset();
    var grid = gridOf(3, 2);
    grid.paint = null;
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(0, fill_count);
    try std.testing.expectEqual(0, text_count);
}

test "an empty grid still fills its background and stops there" {
    reset();
    var grid = gridOf(0, 2);
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(1, fill_count);
    try std.testing.expectEqual(grid.bg, fills[0].color);
}

test "a grid with a null book array fills its background and stops there" {
    reset();
    var grid = gridOf(3, 2);
    grid.books = null;
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    try std.testing.expectEqual(1, fill_count);
}

test "render on a widget with no context does nothing" {
    reset();
    var w: abi.Widget = std.mem.zeroes(abi.Widget);
    w.vt = abi.ra8_widget_book_grid_vtable();
    w.vt.?.render.?(&w);
    try std.testing.expectEqual(0, fill_count);
}

test "a tap opens the card it is drawn inside" {
    reset();
    var grid = gridOf(3, 2);
    var w = widgetOf(&grid);
    const content = abi.contentRect(w.rect, grid.pad);
    const cell = abi.cellRect(content, 1, 2, 2, grid.gap);
    const tap = touchAt(cell.x + 2, cell.y + 2);

    try std.testing.expect(w.vt.?.on_input.?(&w, &tap));
    try std.testing.expectEqual(1, grid.selected);
    try std.testing.expectEqual(1, open_count);
    try std.testing.expectEqual(1, opened[0]);
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(@backingInt(abi.Refresh.fast), last_refresh);
}

test "every card's own pixels route to that card" {
    reset();
    var grid = gridOf(3, 2);
    var w = widgetOf(&grid);
    const content = abi.contentRect(w.rect, grid.pad);

    for (0..3) |i| {
        const idx: u16 = @intCast(i);
        const cell = abi.cellRect(content, idx, 2, 2, grid.gap);
        const tap = touchAt(cell.x + @divTrunc(cell.w, 2), cell.y + @divTrunc(cell.h, 2));
        try std.testing.expect(w.vt.?.on_input.?(&w, &tap));
        try std.testing.expectEqual(idx, grid.selected);
    }
}

test "a tap in the gap between cards is declined" {
    reset();
    var grid = gridOf(3, 2);
    var w = widgetOf(&grid);
    const content = abi.contentRect(w.rect, grid.pad);
    const first = abi.cellRect(content, 0, 2, 2, grid.gap);
    const gap_tap = touchAt(first.x + first.w + 1, first.y + 2);

    try std.testing.expect(!w.vt.?.on_input.?(&w, &gap_tap));
    try std.testing.expectEqual(0, open_count);
    try std.testing.expectEqual(0, invalidations);
}

test "a tap in the padding outside the content rect is declined" {
    reset();
    var grid = gridOf(3, 2);
    var w = widgetOf(&grid);
    try std.testing.expect(!w.vt.?.on_input.?(&w, &touchAt(1, 1)));
    try std.testing.expectEqual(0, open_count);
}

test "a grid with no cards routes nothing" {
    reset();
    var grid = gridOf(0, 2);
    var w = widgetOf(&grid);
    try std.testing.expect(!w.vt.?.on_input.?(&w, &touchAt(20, 20)));
    try std.testing.expectEqual(0, invalidations);
}

test "a button event is declined even over a card" {
    reset();
    var grid = gridOf(3, 2);
    var w = widgetOf(&grid);
    const content = abi.contentRect(w.rect, grid.pad);
    const cell = abi.cellRect(content, 0, 2, 2, grid.gap);
    const press: abi.Event = .{
        .kind = .button,
        .reserved = 0,
        .button_id = 7,
        .x = cell.x + 2,
        .y = cell.y + 2,
    };
    try std.testing.expect(!w.vt.?.on_input.?(&w, &press));
    try std.testing.expectEqual(0, open_count);
}

test "a grid with no open callback still selects and invalidates" {
    reset();
    var grid = gridOf(3, 2);
    grid.on_open = null;
    var w = widgetOf(&grid);
    const content = abi.contentRect(w.rect, grid.pad);
    const cell = abi.cellRect(content, 2, 2, 2, grid.gap);

    try std.testing.expect(w.vt.?.on_input.?(&w, &touchAt(cell.x + 1, cell.y + 1)));
    try std.testing.expectEqual(2, grid.selected);
    try std.testing.expectEqual(1, invalidations);
    try std.testing.expectEqual(0, open_count);
}

test "input on a widget with no context is declined" {
    reset();
    var w: abi.Widget = std.mem.zeroes(abi.Widget);
    w.vt = abi.ra8_widget_book_grid_vtable();
    try std.testing.expect(!w.vt.?.on_input.?(&w, &touchAt(5, 5)));
}

test "the tapped card is the card drawn under the tap" {
    reset();
    var grid = gridOf(3, 2);
    var w = widgetOf(&grid);
    w.vt.?.render.?(&w);

    // the second card's cover fill, taken from the render recording
    var covers: [3]Fill = undefined;
    var found: usize = 0;
    for (fills[0..fill_count]) |f| {
        if (found < 3 and f.color == books[found].cover) {
            covers[found] = f;
            found += 1;
        }
    }
    try std.testing.expectEqual(3, found);

    reset();
    const tap = touchAt(covers[1].x + 1, covers[1].y + 1);
    try std.testing.expect(w.vt.?.on_input.?(&w, &tap));
    try std.testing.expectEqual(1, grid.selected);
}
