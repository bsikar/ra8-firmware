//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ra8_gfx as a Zig module host code imports (RA8FW-949): the fonts the
//! panel draws with (the 8x16 sans cell face, the Literata reading atlas
//! and the display atlases) and the text layout that measures and draws
//! runs in them. The emulator and host GUI apps take it from the root
//! build.zig as `dependency.module("ra8_gfx")`, so desktop text uses the
//! same glyphs as the panel. Plain Zig with no chip code: pixels go out
//! through the caller's `PixelFn`, so any framebuffer can take them.

/// Text layout, measurement and rasterisation over the bundled fonts.
pub const text = @import("internal/text.zig");

pub const Face = text.Face;
pub const Weight = text.Weight;
pub const Extent = text.Extent;
pub const PixelFn = text.PixelFn;
