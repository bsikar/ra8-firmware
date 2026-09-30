//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the bundled 8x16 font: the single exported descriptor
//! `ra8_gfx_font_8x16` that `inc/ra8_gfx_font.h` declares, pointing at the
//! table in `internal/font_8x16.zig`. Nothing is computed here; the geometry
//! fields are the table's own constants, so the descriptor and the bytes it
//! describes cannot drift apart.
//!
//! `../ra8_gfx_abi.zig` references this file so the export lands in the
//! installed archive: the linker only sees what the archive's root analyses.

const impl = @import("internal/root.zig");
const font = @import("internal/font_8x16.zig");

/// Re-exported so a test binary shares the exact table and geometry.
pub const table = font;

/// Re-exported so a test binary shares the exact descriptor type.
pub const Font = impl.Font;

/// `ra8_gfx_font_8x16` -- the bundled IBM PC VGA table, ASCII 0x20..0x7E.
pub export const ra8_gfx_font_8x16: impl.Font = .{
    .glyph_data = &font.glyphs,
    .glyph_width = font.geometry.width,
    .glyph_height = font.geometry.height,
    .bytes_per_glyph = font.geometry.bytes_per_glyph,
    .first_codepoint = font.geometry.first_codepoint,
    .last_codepoint = font.geometry.last_codepoint,
};
