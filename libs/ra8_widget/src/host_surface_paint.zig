//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The widget Paint table bound to `ra8_gfx`'s host argb8888 surface
//! (RA8FW-950), so desktop GUI apps render the same widgets the panel does
//! in full colour with alpha blending, lines, blits, clip stack, and text.

const gfx = @import("ra8_gfx");
const panel_paint = @import("panel_paint");

/// Bind a Paint table of type `Paint` drawing into `surface`, every member bound.
pub fn table(comptime Paint: type, surface: *gfx.Surface) Paint {
    return panel_paint.table(gfx.Surface, Paint, surface, gfx.Capabilities.desktop_argb8888);
}
