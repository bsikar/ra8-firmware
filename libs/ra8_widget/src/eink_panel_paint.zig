//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The widget Paint table bound to `ra8_gfx`'s packed gray4 e-ink panel
//! framebuffer (RA8FW-1012). Declares `gray4` + `dither` capabilities over
//! caller-backed framebuffer memory with zero heap allocation, so the same
//! widget code paints cleanly on desktop RGBA and on the chip's panel.

const gfx = @import("ra8_gfx");
const panel_paint = @import("panel_paint");

/// Bind a Paint table of type `Paint` drawing into a gray4 `EinkSurface`.
pub fn table(comptime Paint: type, panel: *gfx.EinkSurface) Paint {
    return panel_paint.table(gfx.EinkSurface, Paint, panel, gfx.Capabilities.eink_gray4);
}
