//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the on-screen-keyboard leaf widget declared in
//! `inc/ra8_widget_keyboard.h`: `ra8_widget_keyboard_vtable` and
//! `ra8_widget_keyboard_init`.
//!
//! The keyboard draws a key grid and routes a tap into it, but it never links
//! `ra8_keyboard`: the layout engine arrives through the injected
//! `ra8_widget_keyboard_ops_t` seam (`count` / `key_info` / `hit` / `apply`),
//! exactly as pixels arrive through `ra8_widget_paint_t`. So this file owns
//! only two decisions: what one key looks like, and what a tap does.
//!
//! Like every leaf so far it routes to key *indices*, not to child widgets:
//! the widget tree's child-array shape is still the container's problem.

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

/// Key indices the seam can answer with.
pub const key = struct {
    /// `k_ra8_widget_key_no_hit`: the tap landed on a gap or off the grid.
    pub const no_hit: u8 = 255;
    /// `glyph == 0`: this key carries a label, not a character.
    pub const no_glyph: u8 = 0;
};

/// Fixed geometry of a key's own text: centred, with no inset of its own,
/// because the key rect the seam hands back is already the text box.
pub const geometry = struct {
    pub const no_pad: i16 = 0;
};

const tag: [*:0]const u8 = "ra8_widget_keyboard";

/// One key's drawable geometry plus glyph, filled by the seam
/// (`ra8_widget_key_info_t`). `glyph` is mirrored as a byte rather than
/// `c_char` so the copy into the draw buffer stays signedness-free; the C
/// field is one `char` either way.
pub const KeyInfo = extern struct {
    rect: Rect,
    label: ?[*:0]const u8,
    glyph: u8,
    pad0: u8,
    pad1: u16,
};

/// The injected keyboard-engine seam (`ra8_widget_keyboard_ops_t`). Every
/// callback is optional: an app that binds none leaves the widget inert but
/// still well-behaved, which is what the C nullptr checks buy.
pub const Ops = extern struct {
    user: ?*anyopaque,
    count: ?*const fn (user: ?*anyopaque) callconv(.c) u8,
    key_info: ?*const fn (user: ?*anyopaque, idx: u8, out: *KeyInfo) callconv(.c) void,
    hit: ?*const fn (user: ?*anyopaque, x: i32, y: i32) callconv(.c) u8,
    apply: ?*const fn (user: ?*anyopaque, idx: u8) callconv(.c) bool,
};

/// Caller-owned keyboard descriptor (`ra8_widget_keyboard_t`).
pub const Keyboard = extern struct {
    paint: ?*const Paint,
    ops: ?*const Ops,
    on_commit: ?*const fn (w: *Widget) callconv(.c) void,
    bg: u32,
    key_face: u32,
    key_border: u32,
    key_fg: u32,
    border_w: i16,
    reserved: u16,
};

/// Draw one key: its bordered face, then its centred glyph or label.
///
/// A character key wins over a label, matching the C precedence, and a key
/// with neither draws just its face.
fn drawKey(kbd: *const Keyboard, backend: *const Paint, info: *const KeyInfo) void {
    paint_abi.priv_widget_fill_box(backend, &info.rect, kbd.key_face, kbd.key_border, kbd.border_w);

    const draw_text = backend.draw_text orelse return;

    var glyph_buf: [1:0]u8 = .{key.no_glyph};
    var text: [*:0]const u8 = undefined;
    if (info.glyph != key.no_glyph) {
        glyph_buf[0] = info.glyph;
        text = &glyph_buf;
    } else {
        text = info.label orelse return;
    }

    var pen_x: i32 = 0;
    var pen_y: i32 = 0;
    paint_abi.priv_widget_text_pos(
        backend,
        &info.rect,
        text,
        geometry.no_pad,
        .center,
        &pen_x,
        &pen_y,
    );
    draw_text(backend.user, pen_x, pen_y, text, kbd.key_fg, kbd.key_face);
}

/// Fill the keyboard band, then paint every key the seam reports.
fn render(w: *Widget) callconv(.c) void {
    const kbd: *const Keyboard = @ptrCast(@alignCast(w.ctx orelse return));
    const backend = kbd.paint orelse return;

    paint_abi.priv_widget_fill_box(backend, &w.rect, kbd.bg, kbd.bg, geometry.no_pad);

    const ops = kbd.ops orelse return;
    const count = ops.count orelse return;
    const key_info = ops.key_info orelse return;

    const n = count(ops.user);
    for (0..n) |i| {
        var info: KeyInfo = undefined;
        @memset(@as([*]u8, @ptrCast(&info))[0..@sizeOf(KeyInfo)], 0);
        key_info(ops.user, @intCast(i), &info);
        drawKey(kbd, backend, &info);
    }
}

/// Map a touch to a key, apply it, and fire the commit edge.
///
/// The keyboard owns its whole band, so every touch is consumed even when it
/// lands on a gap; a button event is declined so it can keep travelling.
fn onInput(w: *Widget, event: *const Event) callconv(.c) bool {
    const kbd: *const Keyboard = @ptrCast(@alignCast(w.ctx orelse return false));
    if (event.kind != .touch) return false;

    const ops = kbd.ops orelse return true;
    const hit = ops.hit orelse return true;
    const apply = ops.apply orelse return true;

    const idx = hit(ops.user, event.x, event.y);
    if (idx == key.no_hit) return true;

    const committed = apply(ops.user, idx);
    _ = types.ra8_widget_invalidate(w, .quality);
    if (committed) {
        if (kbd.on_commit) |notify| notify(w);
    }
    return true;
}

const vtable: Vtable = .{
    .measure = null,
    .render = render,
    .on_input = onInput,
};

/// Return the one immutable vtable shared by every on-screen keyboard.
pub export fn ra8_widget_keyboard_vtable() callconv(.c) *const Vtable {
    return &vtable;
}

/// Bind `w` to keyboard `kbd`: vtable, context, visible.
pub export fn ra8_widget_keyboard_init(w: ?*Widget, kbd: ?*Keyboard) callconv(.c) u16 {
    const widget = w orelse return types.refuseNull(tag, "w must not be nullptr");
    const descriptor = kbd orelse return types.refuseNull(tag, "kbd must not be nullptr");

    widget.vt = &vtable;
    widget.ctx = descriptor;
    widget.visible = true;
    return err.ok;
}

comptime {
    const ptr = @sizeOf(usize);

    if (@offsetOf(KeyInfo, "rect") != 0) @compileError("ra8_widget_key_info_t rect offset");
    if (@offsetOf(KeyInfo, "label") != 16) @compileError("ra8_widget_key_info_t label offset");
    if (@offsetOf(KeyInfo, "glyph") != 16 + ptr) @compileError("ra8_widget_key_info_t glyph offset");
    if (@offsetOf(KeyInfo, "pad0") != 17 + ptr) @compileError("ra8_widget_key_info_t pad0 offset");
    if (@offsetOf(KeyInfo, "pad1") != 18 + ptr) @compileError("ra8_widget_key_info_t pad1 offset");
    if (@alignOf(KeyInfo) != @alignOf(usize)) @compileError("ra8_widget_key_info_t alignment");

    if (@sizeOf(Ops) != 5 * ptr) @compileError("ra8_widget_keyboard_ops_t size");
    if (@offsetOf(Ops, "user") != 0) @compileError("ra8_widget_keyboard_ops_t user offset");
    if (@offsetOf(Ops, "count") != ptr) @compileError("ra8_widget_keyboard_ops_t count offset");
    if (@offsetOf(Ops, "key_info") != 2 * ptr) @compileError("ra8_widget_keyboard_ops_t key_info offset");
    if (@offsetOf(Ops, "hit") != 3 * ptr) @compileError("ra8_widget_keyboard_ops_t hit offset");
    if (@offsetOf(Ops, "apply") != 4 * ptr) @compileError("ra8_widget_keyboard_ops_t apply offset");

    if (@offsetOf(Keyboard, "paint") != 0) @compileError("ra8_widget_keyboard_t paint offset");
    if (@offsetOf(Keyboard, "ops") != ptr) @compileError("ra8_widget_keyboard_t ops offset");
    if (@offsetOf(Keyboard, "on_commit") != 2 * ptr) @compileError("ra8_widget_keyboard_t on_commit offset");
    if (@offsetOf(Keyboard, "bg") != 3 * ptr) @compileError("ra8_widget_keyboard_t bg offset");
    if (@offsetOf(Keyboard, "key_face") != 3 * ptr + 4) @compileError("ra8_widget_keyboard_t key_face offset");
    if (@offsetOf(Keyboard, "key_border") != 3 * ptr + 8) @compileError("ra8_widget_keyboard_t key_border offset");
    if (@offsetOf(Keyboard, "key_fg") != 3 * ptr + 12) @compileError("ra8_widget_keyboard_t key_fg offset");
    if (@offsetOf(Keyboard, "border_w") != 3 * ptr + 16) @compileError("ra8_widget_keyboard_t border_w offset");
    if (@offsetOf(Keyboard, "reserved") != 3 * ptr + 18) @compileError("ra8_widget_keyboard_t reserved offset");
    if (@alignOf(Keyboard) != @alignOf(usize)) @compileError("ra8_widget_keyboard_t alignment");
}
