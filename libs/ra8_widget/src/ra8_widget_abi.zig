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

/// The module-private paint helpers of `src/ra8_widget_internal.h`.
pub const paint = @import("widget_paint_abi.zig");

/// The text-label leaf widget: `ra8_widget_label_vtable` / `_init`.
pub const label = @import("widget_label_abi.zig");
