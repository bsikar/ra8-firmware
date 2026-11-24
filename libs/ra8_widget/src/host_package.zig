//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the `ra8_widget_host` package export (RA8FW-828, RA8FW-1016):
//! exposes both the grey8 `Canvas` preview backend, the desktop RGBA
//! `DesktopSurface` / `Surface` + `surface_paint` backend, and the panel
//! `EinkSurface` + `eink_paint` backend alongside the Zig box, UI rect, and
//! widget core symbols for host UI tools and desktop apps.

pub const gfx = @import("ra8_gfx");
pub const widget = @import("ra8_widget");
pub const ops = widget.ops;

pub const Canvas = @import("host_paint.zig").Canvas;
pub const DesktopSurface = @import("host_desktop.zig").DesktopSurface;
pub const surface_paint = @import("host_surface_paint.zig");
pub const eink_paint = @import("eink_panel_paint.zig");

pub const Surface = gfx.Surface;
pub const EinkSurface = gfx.EinkSurface;
pub const Capabilities = gfx.Capabilities;
pub const PixelFormat = gfx.PixelFormat;
pub const Format = gfx.Format;
pub const Rect = gfx.Rect;

comptime {
    _ = @import("box");
    _ = @import("ui");
    _ = widget.core;
}
