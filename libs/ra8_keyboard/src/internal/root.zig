//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Layout and typing core for `ra8_keyboard`, the iOS-style on-screen
//! keyboard. Everything here is pure: the caller owns the layout and the text
//! buffer, nothing is allocated, and the only outside dependency is the
//! rectangle hit test, which arrives as a function pointer so the ABI membrane
//! can bind it to `ra8_ui_rect_contains` while these tests bind a local copy.
//!
//! The half-unit grid is the whole geometry model: each row is 20 half-units
//! wide, a normal key is 2, and pixel positions come from truncating integer
//! division, exactly as the C did.

const std = @import("std");

/// Key rows per layer.
pub const rows: i32 = 4;
/// Key-rect slots in a layout.
pub const max_keys: usize = 40;
/// Text buffer capacity including the NUL.
pub const text_max: usize = 64;
/// `ra8_kbd_hit` "no key" sentinel.
pub const no_hit: u8 = 255;

/// Half-units per row; a normal key is `key_hu` of these.
pub const hu_div: i32 = 20;
/// Normal key width in half-units.
pub const key_hu: i32 = 2;
/// SHIFT / BACKSPACE width in half-units.
pub const wide_hu: i32 = 3;
/// Home-row inset each side.
pub const inset_hu: i32 = 1;
/// 123 / ABC / RETURN width.
pub const act_hu: i32 = 4;
/// SPACE width.
pub const space_hu: i32 = 12;
/// Keys in rows 0 and 1 of the letters and numbers layers.
pub const top_keys: i32 = 10;
/// Keys in the inset home row.
pub const mid_keys: i32 = 9;
/// Letters in letters row 2.
pub const r2_letters: i32 = 7;
/// Punctuation keys shared by the numbers and symbols layers.
pub const punct_n: i32 = 5;
/// Centred start half-unit for the punctuation keys.
pub const punct_hu0: i32 = 5;
/// Keys in symbols row 1.
pub const sym1_n: i32 = 7;
/// Centred start half-unit for symbols row 1.
pub const sym1_hu0: i32 = 3;

/// Active layer (`ra8_kbd_layer_t`).
pub const Layer = enum(u8) {
    letters = 0,
    numbers = 1,
    symbols = 2,
};

/// Key behaviour (`ra8_kbd_key_kind_t`).
pub const KeyKind = enum(u8) {
    char = 0,
    space = 1,
    backspace = 2,
    enter = 3,
    shift = 4,
    layer = 5,
};

/// Rectangle in framebuffer pixels (`ra8_ui_rect_t`, owned by `ra8_ui`).
pub const Rect = extern struct {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
};

/// One key: hit rectangle, glyph pair, behaviour tag (`ra8_kbd_key_t`).
pub const Key = extern struct {
    rect: Rect,
    ch_lower: u8,
    ch_upper: u8,
    kind: u8,
    aux: u8,
};

/// Caller-owned key grid for the active layer (`ra8_kbd_layout_t`).
pub const Layout = extern struct {
    keys: [max_keys]Key,
    count: u8,
    shift: bool,
    layer: u8,
    frame: Rect,
};

/// Caller-owned text state (`ra8_kbd_text_t`).
pub const Text = extern struct {
    buf: [text_max]u8,
    len: u8,
    committed: bool,
};

comptime {
    // These layouts are the C ABI: the header hands them to the caller by
    // value, so every offset below is load-bearing.
    std.debug.assert(@sizeOf(Rect) == 16);
    std.debug.assert(@sizeOf(Key) == 20);
    std.debug.assert(@offsetOf(Key, "rect") == 0);
    std.debug.assert(@offsetOf(Key, "ch_lower") == 16);
    std.debug.assert(@offsetOf(Key, "ch_upper") == 17);
    std.debug.assert(@offsetOf(Key, "kind") == 18);
    std.debug.assert(@offsetOf(Key, "aux") == 19);
    std.debug.assert(@sizeOf(Layout) == 820);
    std.debug.assert(@offsetOf(Layout, "keys") == 0);
    std.debug.assert(@offsetOf(Layout, "count") == 800);
    std.debug.assert(@offsetOf(Layout, "shift") == 801);
    std.debug.assert(@offsetOf(Layout, "layer") == 802);
    std.debug.assert(@offsetOf(Layout, "frame") == 804);
    std.debug.assert(@sizeOf(Text) == 66);
    std.debug.assert(@offsetOf(Text, "buf") == 0);
    std.debug.assert(@offsetOf(Text, "len") == 64);
    std.debug.assert(@offsetOf(Text, "committed") == 65);
}

/// Letters layer, unshifted row 0.
pub const letters_row0_lower = "qwertyuiop";
/// Letters layer, shifted row 0.
pub const letters_row0_upper = "QWERTYUIOP";
/// Letters layer, unshifted home row.
pub const letters_row1_lower = "asdfghjkl";
/// Letters layer, shifted home row.
pub const letters_row1_upper = "ASDFGHJKL";
/// Letters layer, unshifted row 2.
pub const letters_row2_lower = "zxcvbnm";
/// Letters layer, shifted row 2.
pub const letters_row2_upper = "ZXCVBNM";
/// Numbers layer, digit row.
pub const numbers_row0 = "1234567890";
/// Numbers layer, common-symbol row; no shift effect.
pub const numbers_row1 = "-/:;()$&@\"";
/// Symbols layer, bracket and math row.
pub const symbols_row0 = "[]{}#%^*+=";
/// Symbols layer, remaining ASCII symbols.
pub const symbols_row1 = "<>\\_`|~";
/// Punctuation row shared by the numbers and symbols layers.
pub const punctuation = ".,?!'";

fn clampToI32(value: i64) i32 {
    if (value > std.math.maxInt(i32)) return std.math.maxInt(i32);
    if (value < std.math.minInt(i32)) return std.math.minInt(i32);
    return @intCast(value);
}

/// Pixel X of half-unit `hu` inside `frame`.
///
/// The C computed `f->x + (f->w * hu) / 20` in `int32_t`, which overflows for a
/// frame wider than about 107 million pixels. The arithmetic here happens in
/// 64 bits and saturates, so a frame that large yields a pinned edge instead of
/// a wrapped coordinate. Every in-range frame maps identically, truncating
/// division included.
pub fn hx(frame: *const Rect, hu: i32) i32 {
    const product: i64 = @as(i64, frame.w) * @as(i64, hu);
    const scaled: i64 = @divTrunc(product, @as(i64, hu_div));
    return clampToI32(@as(i64, frame.x) + scaled);
}

fn widthBetween(frame: *const Rect, first_hu: i32, last_hu: i32) i32 {
    const left: i64 = hx(frame, first_hu);
    const right: i64 = hx(frame, last_hu);
    return clampToI32(right - left);
}

/// Append one key descriptor, silently discarded once the layout is full.
pub fn add(
    kb: *Layout,
    x: i32,
    w: i32,
    y: i32,
    h: i32,
    lower: u8,
    upper: u8,
    kind: KeyKind,
    aux: u8,
) void {
    if (@as(usize, kb.count) >= max_keys) return;
    kb.keys[kb.count] = .{
        .rect = .{ .x = x, .y = y, .w = w, .h = h },
        .ch_lower = lower,
        .ch_upper = upper,
        .kind = @intFromEnum(kind),
        .aux = aux,
    };
    kb.count += 1;
}

/// Place `n` character keys of `key_hu` half-units each from half-unit `hu0`.
///
/// `upper` of `null` means the layer has no shift effect, so the shifted glyph
/// is the unshifted one.
pub fn place(
    kb: *Layout,
    lower: []const u8,
    upper: ?[]const u8,
    n: i32,
    hu0: i32,
    frame: *const Rect,
    y: i32,
    row_height: i32,
) void {
    var index: i32 = 0;
    while (index < n) : (index += 1) {
        const first_hu = hu0 + (index * key_hu);
        const last_hu = hu0 + ((index + 1) * key_hu);
        const slot: usize = @intCast(index);
        const low = lower[slot];
        const high = if (upper) |shifted| shifted[slot] else low;
        add(
            kb,
            hx(frame, first_hu),
            widthBetween(frame, first_hu, last_hu),
            y,
            row_height,
            low,
            high,
            .char,
            0,
        );
    }
}

/// Append one special key spanning half-units `[first_hu, last_hu)`.
pub fn span(
    kb: *Layout,
    first_hu: i32,
    last_hu: i32,
    frame: *const Rect,
    y: i32,
    row_height: i32,
    kind: KeyKind,
    aux: u8,
) void {
    add(
        kb,
        hx(frame, first_hu),
        widthBetween(frame, first_hu, last_hu),
        y,
        row_height,
        0,
        0,
        kind,
        aux,
    );
}

/// Row 2 of the numbers and symbols layers: layer toggle, punctuation, BACKSPACE.
pub fn rowPunct(
    kb: *Layout,
    frame: *const Rect,
    y: i32,
    row_height: i32,
    toggle_aux: u8,
) void {
    span(kb, 0, act_hu, frame, y, row_height, .layer, toggle_aux);
    place(kb, punctuation, null, punct_n, punct_hu0, frame, y, row_height);
    span(kb, hu_div - act_hu, hu_div, frame, y, row_height, .backspace, 0);
}

/// Bottom row, identical in shape on every layer: layer toggle, SPACE, RETURN.
pub fn rowBottom(
    kb: *Layout,
    frame: *const Rect,
    y: i32,
    row_height: i32,
    left_aux: u8,
) void {
    span(kb, 0, act_hu, frame, y, row_height, .layer, left_aux);
    span(kb, act_hu, act_hu + space_hu, frame, y, row_height, .space, 0);
    span(kb, hu_div - act_hu, hu_div, frame, y, row_height, .enter, 0);
}

/// Build the letters layer: QWERTY, SHIFT, BACKSPACE, 123 toggle.
pub fn buildLetters(kb: *Layout, frame: *const Rect, row_height: i32) void {
    const top = frame.y;
    place(kb, letters_row0_lower, letters_row0_upper, top_keys, 0, frame, top, row_height);
    place(
        kb,
        letters_row1_lower,
        letters_row1_upper,
        mid_keys,
        inset_hu,
        frame,
        top + row_height,
        row_height,
    );
    const row2_y = top + (2 * row_height);
    span(kb, 0, wide_hu, frame, row2_y, row_height, .shift, 0);
    place(kb, letters_row2_lower, letters_row2_upper, r2_letters, wide_hu, frame, row2_y, row_height);
    span(kb, hu_div - wide_hu, hu_div, frame, row2_y, row_height, .backspace, 0);
    rowBottom(kb, frame, top + (3 * row_height), row_height, @intFromEnum(Layer.numbers));
}

/// Build the numbers layer: digits, common symbols, `#+=` toggle.
pub fn buildNumbers(kb: *Layout, frame: *const Rect, row_height: i32) void {
    const top = frame.y;
    place(kb, numbers_row0, null, top_keys, 0, frame, top, row_height);
    place(kb, numbers_row1, null, top_keys, 0, frame, top + row_height, row_height);
    rowPunct(kb, frame, top + (2 * row_height), row_height, @intFromEnum(Layer.symbols));
    rowBottom(kb, frame, top + (3 * row_height), row_height, @intFromEnum(Layer.letters));
}

/// Build the symbols layer: brackets, math operators, `123` toggle.
pub fn buildSymbols(kb: *Layout, frame: *const Rect, row_height: i32) void {
    const top = frame.y;
    place(kb, symbols_row0, null, top_keys, 0, frame, top, row_height);
    place(kb, symbols_row1, null, sym1_n, sym1_hu0, frame, top + row_height, row_height);
    rowPunct(kb, frame, top + (2 * row_height), row_height, @intFromEnum(Layer.numbers));
    rowBottom(kb, frame, top + (3 * row_height), row_height, @intFromEnum(Layer.letters));
}

/// Rebuild the grid for whatever layer `kb.layer` currently names.
///
/// An unknown layer byte falls back to letters, which is what the C's trailing
/// `else` did.
pub fn buildLayer(kb: *Layout) void {
    kb.count = 0;
    const row_height = @divTrunc(kb.frame.h, rows);
    const frame = kb.frame;
    if (kb.layer == @intFromEnum(Layer.numbers)) {
        buildNumbers(kb, &frame, row_height);
    } else if (kb.layer == @intFromEnum(Layer.symbols)) {
        buildSymbols(kb, &frame, row_height);
    } else {
        buildLetters(kb, &frame, row_height);
    }
}

/// Why `layoutInit` refused. The ABI maps this onto `ra8_err_t`.
pub const FrameFault = error{
    /// `w <= 0` or `h <= 0`: a frame with no area.
    NoArea,
};

/// Lay out the letters layer inside `frame`, clearing SHIFT.
pub fn layoutInit(kb: *Layout, frame: *const Rect) FrameFault!void {
    if ((frame.w <= 0) or (frame.h <= 0)) return error.NoArea;
    kb.frame = frame.*;
    kb.shift = false;
    kb.layer = @intFromEnum(Layer.letters);
    buildLayer(kb);
}

/// Rectangle hit test, supplied by the caller so the core stays pure.
pub const ContainsFn = *const fn (?*const Rect, i32, i32) callconv(.c) bool;

/// Index of the first key containing the point, else `no_hit`.
pub fn hit(kb: *const Layout, px: i32, py: i32, contains: ContainsFn) u8 {
    var index: usize = 0;
    while (index < kb.count) : (index += 1) {
        if (contains(&kb.keys[index].rect, px, py)) return @intCast(index);
    }
    return no_hit;
}

/// Shift-correct glyph for a key, or 0 for a special key or a bad index.
pub fn glyphOf(kb: *const Layout, key_idx: u8) u8 {
    if (key_idx >= kb.count) return 0;
    const key = &kb.keys[key_idx];
    if (key.kind != @intFromEnum(KeyKind.char)) return 0;
    return if (kb.shift) key.ch_upper else key.ch_lower;
}

/// Reset the text state to empty and uncommitted.
///
/// Only `buf[0]` is written, as the C did: the bytes past the NUL stay
/// whatever the caller's storage held.
pub fn textInit(t: *Text) void {
    t.len = 0;
    t.buf[0] = 0;
    t.committed = false;
}

/// Append one character if capacity allows, else discard it.
pub fn append(t: *Text, ch: u8) void {
    if (@as(usize, t.len) < (text_max - 1)) {
        t.buf[t.len] = ch;
        t.len += 1;
        t.buf[t.len] = 0;
    }
}

/// Apply a key press to the text and layout state.
///
/// An index at or past `kb.count`, the `no_hit` sentinel included, is a no-op.
pub fn applyKey(t: *Text, kb: *Layout, key_idx: u8) void {
    if (key_idx >= kb.count) return;
    const key = kb.keys[key_idx];
    switch (key.kind) {
        @intFromEnum(KeyKind.char) => {
            append(t, if (kb.shift) key.ch_upper else key.ch_lower);
            kb.shift = false;
        },
        @intFromEnum(KeyKind.space) => {
            append(t, ' ');
            kb.shift = false;
        },
        @intFromEnum(KeyKind.backspace) => {
            if (t.len > 0) {
                t.len -= 1;
                t.buf[t.len] = 0;
            }
        },
        @intFromEnum(KeyKind.enter) => t.committed = true,
        @intFromEnum(KeyKind.shift) => kb.shift = !kb.shift,
        @intFromEnum(KeyKind.layer) => {
            kb.layer = key.aux;
            kb.shift = false;
            buildLayer(kb);
        },
        else => {},
    }
}
