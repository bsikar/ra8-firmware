//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the book-grid leaf widget declared in
//! `inc/ra8_widget_book.h`: `ra8_widget_book_grid_vtable` and
//! `ra8_widget_book_grid_init`.
//!
//! The e-reader Library screen is a grid of book cards, each a cover swatch
//! above a title, an author line and a read-progress bar, and each a tap
//! target. The tiling helpers are `pub` and pure, and drawing and hit-testing
//! both go through `cellRect`, so a tap opens the card it looks like it opens.

const types = @import("widget_abi_types.zig");
const paint_abi = @import("widget_paint_abi.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = types.Rect;
/// Draw backend of the published ABI (`ra8_widget_paint_t`).
pub const Paint = types.Paint;
/// Widget instance of the published ABI (`ra8_widget_t`).
pub const Widget = types.Widget;
/// Behaviour table of the published ABI (`ra8_widget_vtable_t`).
pub const Vtable = types.Vtable;
/// One input event of the published ABI (`ra8_widget_event_t`).
pub const Event = types.Event;
/// E-ink-style refresh hint of the published ABI (`ra8_widget_refresh_t`).
pub const Refresh = types.Refresh;
/// The `ra8_err_t` values this membrane answers with.
pub const err = types.err;

/// Fixed geometry of the grid and its cards.
pub const geometry = struct {
    /// A grid holding this many cards draws and routes nothing.
    pub const no_cards: u16 = 0;
    /// A `cols` below this is read as this: a grid is at least one column wide.
    pub const min_cols: u16 = 1;
    /// The read-progress denominator; a `percent` above it saturates the bar.
    pub const pct_full: u32 = 100;
    /// A card rect is already the text box, so a label row gets no inset.
    pub const no_pad: i16 = 0;
    /// A cover shorter than this has no room to draw.
    pub const no_cover_height: i32 = 0;
    /// A progress fill this wide paints nothing.
    pub const no_fill_width: i32 = 0;
};

const tag: [*:0]const u8 = "ra8_widget_book_grid";

/// `ra8_ui_rect_contains` from `libs/ra8_ui`: the hit test the C grid used,
/// kept so a card's tap target stays bit-identical to the C's.
extern fn ra8_ui_rect_contains(r: *const Rect, px: i32, py: i32) callconv(.c) bool;

/// One card's data (`ra8_widget_book_t`): the grid owns the backend and the
/// colours, so a record is just the per-book content.
pub const Book = extern struct {
    title: ?[*:0]const u8,
    author: ?[*:0]const u8,
    cover: u32,
    percent: u16,
};

/// Caller-owned book-grid descriptor (`ra8_widget_book_grid_t`).
///
/// `books` is the C `const ra8_widget_book_t*`: a bare pointer to `count`
/// records. It becomes a slice in one place, `cards()`, which every entry
/// point starts from, so nothing below indexes a raw pointer.
pub const BookGrid = extern struct {
    paint: ?*const Paint,
    books: ?[*]const Book,
    on_open: ?*const fn (w: *Widget, index: u16) callconv(.c) void,
    bg: u32,
    title_fg: u32,
    author_fg: u32,
    bar_track: u32,
    bar_fill: u32,
    count: u16,
    cols: u16,
    selected: u16,
    pad: i16,
    gap: i16,
    label_h: i16,
    bar_h: i16,

    /// The card records as a bounded slice, or null when the grid is empty or
    /// carries no array. The one pointer-to-slice conversion in this file.
    pub fn cards(self: *const BookGrid) ?[]const Book {
        if (self.count == geometry.no_cards) return null;
        const books = self.books orelse return null;
        return books[0..self.count];
    }

    /// Columns to tile with: the descriptor's, floored at one.
    pub fn columns(self: *const BookGrid) u16 {
        return if (self.cols >= geometry.min_cols) self.cols else geometry.min_cols;
    }
};

/// The grid rect inset by `pad` on every side: where the cards tile.
pub fn contentRect(grid: Rect, pad: i16) Rect {
    const p: i32 = pad;
    return .{ .x = grid.x + p, .y = grid.y + p, .w = grid.w - (p + p), .h = grid.h - (p + p) };
}

/// Rows needed for `count` cards laid `cols` wide, rounded up so a partial
/// final row still gets height.
pub fn rowsFor(count: u16, cols: u16) u16 {
    return @intCast((@as(u32, count) + cols - 1) / cols);
}

/// Card `idx`'s cell: `content` tiled into `cols` x `rows` equal cells
/// separated by `gap`. Drawing and hit-testing both derive from this, which is
/// what keeps a card's tap target under the card.
pub fn cellRect(content: Rect, idx: u16, cols: u16, rows: u16, gap: i16) Rect {
    const g: i32 = gap;
    const col: i32 = @intCast(idx % cols);
    const row: i32 = @intCast(idx / cols);
    const cell_w = @divTrunc(content.w - (g * (@as(i32, cols) - 1)), @as(i32, cols));
    const cell_h = @divTrunc(content.h - (g * (@as(i32, rows) - 1)), @as(i32, rows));
    return .{
        .x = content.x + (col * (cell_w + g)),
        .y = content.y + (row * (cell_h + g)),
        .w = cell_w,
        .h = cell_h,
    };
}

/// The four stacked bands of one card, top to bottom.
const Card = struct {
    cover: Rect,
    title: Rect,
    author: Rect,
    bar: Rect,

    /// Split `cell` into its bands: the bar and the two label rows are fixed
    /// heights off the bottom, and the cover takes whatever is left above.
    fn of(cell: Rect, label_h: i16, bar_h: i16) Card {
        const lbl: i32 = label_h;
        const bar_y = (cell.y + cell.h) - @as(i32, bar_h);
        const author_y = bar_y - lbl;
        const title_y = author_y - lbl;
        return .{
            .cover = .{ .x = cell.x, .y = cell.y, .w = cell.w, .h = title_y - cell.y },
            .title = .{ .x = cell.x, .y = title_y, .w = cell.w, .h = lbl },
            .author = .{ .x = cell.x, .y = author_y, .w = cell.w, .h = lbl },
            .bar = .{ .x = cell.x, .y = bar_y, .w = cell.w, .h = bar_h },
        };
    }
};

/// Draw one label left-aligned in its row. A record with no text is a gap.
fn drawLabel(backend: *const Paint, row: Rect, text: ?[*:0]const u8, fg: u32, bg: u32) void {
    const label = text orelse return;
    const draw_text = backend.draw_text orelse return;

    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    paint_abi.priv_widget_text_pos(backend, &row, label, geometry.no_pad, .left, .sans, false, &pen_x, &pen_y);
    draw_text(backend.user, pen_x, pen_y, label, fg, bg);
}

/// Paint one card: cover swatch, title, author, progress bar.
fn drawCard(grid: *const BookGrid, book: Book, cell: Rect) void {
    const backend = grid.paint orelse return;
    const bands = Card.of(cell, grid.label_h, grid.bar_h);

    if (bands.cover.h > geometry.no_cover_height) {
        paint_abi.priv_widget_fill_box(backend, &bands.cover, book.cover, book.cover, geometry.no_pad);
    }
    if (backend.draw_text != null) {
        drawLabel(backend, bands.title, book.title, grid.title_fg, book.cover);
        drawLabel(backend, bands.author, book.author, grid.author_fg, book.cover);
    }

    paint_abi.priv_widget_fill_box(backend, &bands.bar, grid.bar_track, grid.bar_track, geometry.no_pad);
    const filled = paint_abi.priv_widget_fill_frac(book.percent, geometry.pct_full, cell.w);
    if (filled > geometry.no_fill_width) {
        const fill_rect = backend.fill_rect orelse return;
        fill_rect(backend.user, cell.x, bands.bar.y, filled, bands.bar.h, grid.bar_fill);
    }
}

/// Fill the grid background, then tile and paint every card.
fn render(w: *Widget) callconv(.c) void {
    const grid: *const BookGrid = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = grid.paint orelse return;

    paint_abi.priv_widget_fill_box(backend, &w.rect, grid.bg, grid.bg, geometry.no_pad);

    const books = grid.cards() orelse return;
    const cols = grid.columns();
    const rows = rowsFor(grid.count, cols);
    const content = contentRect(w.rect, grid.pad);

    for (books, 0..) |book, i| {
        drawCard(grid, book, cellRect(content, @intCast(i), cols, rows, grid.gap));
    }
}

/// Route a touch to the card it landed on; a tap in no card is declined so it
/// can keep travelling.
fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const grid: *BookGrid = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;

    const books = grid.cards() orelse return false;
    const cols = grid.columns();
    const rows = rowsFor(grid.count, cols);
    const content = contentRect(w.rect, grid.pad);

    for (books, 0..) |_, i| {
        const idx: u16 = @intCast(i);
        const cell = cellRect(content, idx, cols, rows, grid.gap);
        if (!ra8_ui_rect_contains(&cell, event.x, event.y)) continue;

        grid.selected = idx;
        _ = types.ra8_widget_invalidate(w, .fast);
        if (grid.on_open) |notify| notify(w, idx);
        return true;
    }
    return false;
}

const vtable: Vtable = .{
    .measure = null,
    .render = render,
    .on_input = onInput,
};

/// Return the one immutable vtable shared by every book grid.
pub export fn ra8_widget_book_grid_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

/// Bind `w` to book grid `grid`: vtable, context, visible.
pub export fn ra8_widget_book_grid_init(w: ?*Widget, grid: ?*BookGrid) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = grid orelse return types.refuseNull(tag, "grid must not be nullptr");

    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}

comptime {
    const ptr = @sizeOf(usize);

    if (@offsetOf(Book, "title") != 0) @compileError("ra8_widget_book_t title offset");
    if (@offsetOf(Book, "author") != ptr) @compileError("ra8_widget_book_t author offset");
    if (@offsetOf(Book, "cover") != 2 * ptr) @compileError("ra8_widget_book_t cover offset");
    if (@offsetOf(Book, "percent") != 2 * ptr + 4) @compileError("ra8_widget_book_t percent offset");
    if (@alignOf(Book) != @alignOf(usize)) @compileError("ra8_widget_book_t alignment");

    if (@offsetOf(BookGrid, "paint") != 0) @compileError("ra8_widget_book_grid_t paint offset");
    if (@offsetOf(BookGrid, "books") != ptr) @compileError("ra8_widget_book_grid_t books offset");
    if (@offsetOf(BookGrid, "on_open") != 2 * ptr) @compileError("ra8_widget_book_grid_t on_open offset");
    if (@offsetOf(BookGrid, "bg") != 3 * ptr) @compileError("ra8_widget_book_grid_t bg offset");
    if (@offsetOf(BookGrid, "title_fg") != 3 * ptr + 4) @compileError("ra8_widget_book_grid_t title_fg offset");
    if (@offsetOf(BookGrid, "author_fg") != 3 * ptr + 8) @compileError("ra8_widget_book_grid_t author_fg offset");
    if (@offsetOf(BookGrid, "bar_track") != 3 * ptr + 12) @compileError("ra8_widget_book_grid_t bar_track offset");
    if (@offsetOf(BookGrid, "bar_fill") != 3 * ptr + 16) @compileError("ra8_widget_book_grid_t bar_fill offset");
    if (@offsetOf(BookGrid, "count") != 3 * ptr + 20) @compileError("ra8_widget_book_grid_t count offset");
    if (@offsetOf(BookGrid, "cols") != 3 * ptr + 22) @compileError("ra8_widget_book_grid_t cols offset");
    if (@offsetOf(BookGrid, "selected") != 3 * ptr + 24) @compileError("ra8_widget_book_grid_t selected offset");
    if (@offsetOf(BookGrid, "pad") != 3 * ptr + 26) @compileError("ra8_widget_book_grid_t pad offset");
    if (@offsetOf(BookGrid, "gap") != 3 * ptr + 28) @compileError("ra8_widget_book_grid_t gap offset");
    if (@offsetOf(BookGrid, "label_h") != 3 * ptr + 30) @compileError("ra8_widget_book_grid_t label_h offset");
    if (@offsetOf(BookGrid, "bar_h") != 3 * ptr + 32) @compileError("ra8_widget_book_grid_t bar_h offset");
    if (@alignOf(BookGrid) != @alignOf(usize)) @compileError("ra8_widget_book_grid_t alignment");
}
