//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for the Zig half of `ra8_widget`: the one file that says which
//! membranes this library publishes. It holds no code of its own, so each
//! membrane below stays a single-purpose file and porting the next widget
//! translation unit adds one line here instead of growing a sibling.
//!
//! The existing widget C ABI stays stable; a debug-only channel is optional. no C translation unit is left in this library;
//! its C consumers elsewhere in the tree link every one of these symbols out
//! of the archive.

/// The widget tree's shared published C types, mirrored once.
pub const types = @import("widget_abi_types.zig");

/// The module-private paint helpers of `src/ra8_widget_internal.h`.
pub const paint = @import("widget_paint_abi.zig");

/// The CPU greyscale-image renderer.
pub const image = @import("widget_image.zig");

/// The text-label leaf widget: `ra8_widget_label_vtable` / `_init`.
pub const label = @import("widget_label_abi.zig");

/// The push-button leaf widget: `ra8_widget_button_vtable` / `_init`.
pub const button = @import("widget_button_abi.zig");

/// The checkbox toggle leaf widget: `ra8_widget_toggle_vtable` / `_init`.
pub const toggle = @import("widget_toggle_abi.zig");

/// The one-of-N segmented leaf widget: `ra8_widget_segmented_vtable` / `_init`.
pub const segmented = @import("widget_segmented_abi.zig");

/// The progress-bar leaf widget: `ra8_widget_progress_bar_vtable` / `_init`.
pub const progress_bar = @import("widget_progress_bar_abi.zig");

/// The signed 13-cell equalizer level bar.
pub const level_bar = @import("widget_level_bar_abi.zig");

/// The status-bar leaf widget: `ra8_widget_status_bar_vtable` / `_init`.
pub const status_bar = @import("widget_status_bar_abi.zig");

/// The toolbar leaf widget: `ra8_widget_toolbar_vtable` / `_init`.
pub const toolbar = @import("widget_toolbar_abi.zig");

/// The on-screen-keyboard leaf widget: `ra8_widget_keyboard_vtable` / `_init`.
pub const keyboard = @import("widget_keyboard_abi.zig");

/// The caller-buffer-backed single-line text-entry leaf widget.
pub const text_field = @import("widget_text_field_abi.zig");

/// The navigation-strip leaf widget: `ra8_widget_nav_bar_vtable` / `_init`.
pub const nav_bar = @import("widget_nav_bar_abi.zig");

/// The Previous/Next page-count leaf widget.
pub const pager = @import("widget_pager_abi.zig");

/// The container panel: `ra8_widget_panel_vtable` / `_init` / `_compose`.
pub const panel = @import("widget_panel_abi.zig");

/// The reflowed-reading-body leaf widget: `ra8_widget_reflow_view_vtable` /
/// `_init`.
pub const reflow_view = @import("widget_reflow_view_abi.zig");

/// The book-grid leaf widget: `ra8_widget_book_grid_vtable` / `_init`.
pub const book = @import("widget_book_abi.zig");

/// The settings/list screen leaf widget.
pub const list = @import("widget_list_abi.zig");

/// The flat container ops: `ra8_widget_layout_stack`, `_dispatch`,
/// `_invalidate`, `_damage`, `_render_dirty`.
pub const core = @import("widget_core_abi.zig");

// A `pub const` import is analysed only when something refers to it, and
// nothing in this archive does, so on their own the declarations above leave
// every membrane's `export fn` uncompiled and the archive empty. Referring to
// each one here is what makes its exports land in the library.
comptime {
    _ = types;
    _ = image;
    _ = paint;
    _ = label;
    _ = button;
    _ = toggle;
    _ = segmented;
    _ = progress_bar;
    _ = level_bar;
    _ = status_bar;
    _ = toolbar;
    _ = keyboard;
    _ = text_field;
    _ = nav_bar;
    _ = pager;
    _ = panel;
    _ = reflow_view;
    _ = book;
    _ = list;
    _ = core;
}
