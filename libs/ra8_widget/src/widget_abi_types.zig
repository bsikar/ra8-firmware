//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The widget tree's published C types, mirrored once for every leaf-widget
//! membrane in this archive: the widget instance, its vtable, the input event,
//! the refresh hint, and the two `ra8_err_t` values a bind guard answers with.
//!
//! Each concrete widget owns its own descriptor struct in its own file; what
//! lives here is only what all of them share, so porting the next translation
//! unit does not re-declare `ra8_widget_t` a tenth time.

const paint_abi = @import("widget_paint_abi.zig");

/// Rectangle of the published ABI (`ra8_ui_rect_t`).
pub const Rect = paint_abi.Rect;
/// Alignment selector of the published ABI (`ra8_widget_align_t`).
pub const Alignment = paint_abi.Alignment;
/// Draw backend of the published ABI (`ra8_widget_paint_t`).
pub const Paint = paint_abi.Paint;

/// The `ra8_err_t` values these membranes answer with.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const null_ptr: u16 = 0x504;
};

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// `RA8_CHECK_NULL_PTR(ptr, s_tag, message)`: log under the caller's tag, then
/// answer null_ptr.
pub fn refuseNull(tag: [*:0]const u8, message: [*:0]const u8) u16 {
    ra8_log_emit_error(tag, message);
    return err.null_ptr;
}

/// E-ink-style refresh hint of the published ABI (`ra8_widget_refresh_t`).
pub const Refresh = enum(u8) {
    none = 0,
    fast = 1,
    quality = 2,
};

/// Input event kind of the published ABI (`ra8_widget_ev_kind_t`).
pub const EventKind = enum(u8) {
    touch = 0,
    button = 1,
};

/// One input event of the published ABI (`ra8_widget_event_t`).
pub const Event = extern struct {
    kind: EventKind,
    reserved: u8,
    button_id: u16,
    x: i32,
    y: i32,
};

/// Behaviour table of the published ABI (`ra8_widget_vtable_t`). Every member
/// is optional because a vtable leaves out whatever its widget does not do.
pub const Vtable = extern struct {
    measure: ?*const fn (
        w: *Widget,
        avail_w: i32,
        avail_h: i32,
        out_w: *i32,
        out_h: *i32,
    ) callconv(.c) void,
    render: ?*const fn (w: *Widget) callconv(.c) void,
    on_input: ?*const fn (w: *Widget, event: *const Event) callconv(.c) bool,
};

/// Widget instance of the published ABI (`ra8_widget_t`).
pub const Widget = extern struct {
    vt: ?*const Vtable,
    ctx: ?*anyopaque,
    rect: Rect,
    fixed: i16,
    flex: u16,
    action_id: u16,
    refresh: u8,
    visible: bool,
    dirty: bool,
};

/// `ra8_widget_invalidate` from the still-C `src/ra8_widget.c`: mark the
/// widget dirty, folding the hint upward in strength.
pub extern fn ra8_widget_invalidate(w: *Widget, refresh: Refresh) callconv(.c) u16;

comptime {
    const ptr = @sizeOf(usize);

    if (@sizeOf(Event) != 12) @compileError("ra8_widget_event_t size");
    if (@alignOf(Event) != 4) @compileError("ra8_widget_event_t alignment");
    if (@offsetOf(Event, "kind") != 0) @compileError("ra8_widget_event_t kind offset");
    if (@offsetOf(Event, "button_id") != 2) @compileError("ra8_widget_event_t button_id offset");
    if (@offsetOf(Event, "x") != 4) @compileError("ra8_widget_event_t x offset");
    if (@offsetOf(Event, "y") != 8) @compileError("ra8_widget_event_t y offset");

    if (@sizeOf(EventKind) != 1) @compileError("ra8_widget_ev_kind_t width");
    if (@intFromEnum(EventKind.touch) != 0) @compileError("ra8_widget_ev_kind_t touch value");
    if (@intFromEnum(EventKind.button) != 1) @compileError("ra8_widget_ev_kind_t button value");

    if (@sizeOf(Refresh) != 1) @compileError("ra8_widget_refresh_t width");
    if (@intFromEnum(Refresh.fast) != 1) @compileError("ra8_widget_refresh_t fast value");
    if (@intFromEnum(Refresh.quality) != 2) @compileError("ra8_widget_refresh_t quality value");

    if (@sizeOf(Vtable) != 3 * ptr) @compileError("ra8_widget_vtable_t size");
    if (@offsetOf(Vtable, "measure") != 0) @compileError("ra8_widget_vtable_t measure offset");
    if (@offsetOf(Vtable, "render") != ptr) @compileError("ra8_widget_vtable_t render offset");
    if (@offsetOf(Vtable, "on_input") != 2 * ptr) @compileError("ra8_widget_vtable_t on_input offset");

    if (@alignOf(Widget) != @alignOf(usize)) @compileError("ra8_widget_t alignment");
    if (@offsetOf(Widget, "vt") != 0) @compileError("ra8_widget_t vt offset");
    if (@offsetOf(Widget, "ctx") != ptr) @compileError("ra8_widget_t ctx offset");
    if (@offsetOf(Widget, "rect") != 2 * ptr) @compileError("ra8_widget_t rect offset");
    if (@offsetOf(Widget, "fixed") != 2 * ptr + 16) @compileError("ra8_widget_t fixed offset");
    if (@offsetOf(Widget, "flex") != 2 * ptr + 18) @compileError("ra8_widget_t flex offset");
    if (@offsetOf(Widget, "action_id") != 2 * ptr + 20) @compileError("ra8_widget_t action_id offset");
    if (@offsetOf(Widget, "refresh") != 2 * ptr + 22) @compileError("ra8_widget_t refresh offset");
    if (@offsetOf(Widget, "visible") != 2 * ptr + 23) @compileError("ra8_widget_t visible offset");
    if (@offsetOf(Widget, "dirty") != 2 * ptr + 24) @compileError("ra8_widget_t dirty offset");
}
