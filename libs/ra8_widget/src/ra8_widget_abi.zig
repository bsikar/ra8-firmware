//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for the Zig half of `ra8_widget`: the one file that says which
//! membranes this library publishes. It holds no code of its own, so each
//! membrane below stays a single-purpose file and porting the next widget
//! translation unit adds one line here instead of growing a sibling.
//!
//! The library's public C ABI (`inc/ra8_widget.h`) is unchanged; the widget
//! translation units still written in C link these symbols out of the archive.

/// The widget tree's shared published C types, mirrored once.
pub const types = @import("widget_abi_types.zig");

/// The module-private paint helpers of `src/ra8_widget_internal.h`.
pub const paint = @import("widget_paint_abi.zig");

/// The text-label leaf widget: `ra8_widget_label_vtable` / `_init`.
pub const label = @import("widget_label_abi.zig");

/// The push-button leaf widget: `ra8_widget_button_vtable` / `_init`.
pub const button = @import("widget_button_abi.zig");

/// The progress-bar leaf widget: `ra8_widget_progress_bar_vtable` / `_init`.
pub const progress_bar = @import("widget_progress_bar_abi.zig");

/// The status-bar leaf widget: `ra8_widget_status_bar_vtable` / `_init`.
pub const status_bar = @import("widget_status_bar_abi.zig");

/// The toolbar leaf widget: `ra8_widget_toolbar_vtable` / `_init`.
pub const toolbar = @import("widget_toolbar_abi.zig");

/// The on-screen-keyboard leaf widget: `ra8_widget_keyboard_vtable` / `_init`.
pub const keyboard = @import("widget_keyboard_abi.zig");
