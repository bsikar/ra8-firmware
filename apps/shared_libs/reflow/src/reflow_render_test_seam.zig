//! Test-only link seam for the reflow Zig unit test harness (RA8FW-1222).
//!
//! The render module's rasteriser reaches three Layer-3 surfaces behind the
//! ported reflow_render.zig: the ra8_glyph_atlas glyph cache (libs/ra8_mem),
//! and the ra8_gfx pixel hook. (The SVG document surface behind reflow_svg.c\n//! joined the real Zig library in RA8FW-1237, so the harness no longer stubs it.)
//! Those surfaces are out of scope for this unit harness (the CMake suite
//! exercises them end to end), so the harness answers each surface with its
//! documented "not supported" status, which the render path handles by
//! falling back: direct rasterisation for the atlas, and a skipped (blank)
//! box for SVG. Raster decodes reach the real reflow_image.zig export, whose
//! own seam answers the stb / webp stack (RA8FW-1231). The pixel hook accepts
//! every write. The real blank-code-point policy now ships in
//! reflow_render.zig itself, so the seam exports none of it. The seam must
//! never be imported by reflow_parse_abi.zig: the archive keeps no stub.
const k_ra8_ok: c_int = 0;
const k_ra8_err_not_supported: c_int = 0x107;

export fn ra8_gfx_pixel(x: c_int, y: c_int, color: u32) callconv(.c) c_int {
    shapeRecord(4, x, y, 0, 0, color, 0);
    return k_ra8_ok;
}

export fn ra8_glyph_atlas_init(atlas: *anyopaque, cfg: *const anyopaque) callconv(.c) c_int {
    _ = atlas;
    _ = cfg;
    return k_ra8_err_not_supported;
}

export fn ra8_glyph_atlas_get(atlas: *anyopaque, key: *const anyopaque, out_glyph: *anyopaque) callconv(.c) c_int {
    _ = atlas;
    _ = key;
    _ = out_glyph;
    return k_ra8_err_not_supported;
}

export fn ra8_glyph_atlas_put(atlas: *anyopaque, bitmap: [*]const u8) callconv(.c) c_int {
    _ = atlas;
    _ = bitmap;
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// RA8FW-1235: the shape rasteriser seam. The reflow unit harness has no
// framebuffer, so the ra8_gfx draw entry points record their calls instead:
// the SVG shape tests assert dispatch (fast path vs scanline fill) and the
// emitted coordinates from the record. Existing exports above are untouched.
// ---------------------------------------------------------------------------

/// One recorded ra8_gfx draw call. kind: 1 = line, 2 = rect, 3 = circle,
/// 4 = pixel. a/b/c/d carry the entry point's positional arguments.
pub const ShapeCall = struct {
    kind: u8,
    a: i32,
    b: i32,
    c: i32,
    d: i32,
    color: u32,
    filled: u8,
};

pub var shape_calls: [512]ShapeCall = @splat(.{ .kind = 0, .a = 0, .b = 0, .c = 0, .d = 0, .color = 0, .filled = 0 });
pub var shape_call_n: usize = 0;

/// Empty the record; a test calls this before the draw it wants to observe.
pub fn shapeCallsReset() void {
    shape_call_n = 0;
}

fn shapeRecord(kind: u8, a: i32, b: i32, c: i32, d: i32, color: u32, filled: u8) void {
    if (shape_call_n < shape_calls.len) {
        shape_calls[shape_call_n] = .{ .kind = kind, .a = a, .b = b, .c = c, .d = d, .color = color, .filled = filled };
        shape_call_n += 1;
    }
}

export fn ra8_gfx_line(x0: i32, y0: i32, x1: i32, y1: i32, color: u32) callconv(.c) u16 {
    shapeRecord(1, x0, y0, x1, y1, color, 0);
    return 0;
}

export fn ra8_gfx_rect(x: i32, y: i32, w: i32, h: i32, color: u32, filled: u8) callconv(.c) u16 {
    shapeRecord(2, x, y, w, h, color, filled);
    return 0;
}

export fn ra8_gfx_circle(cx: i32, cy: i32, r: i32, color: u32, filled: u8) callconv(.c) u16 {
    shapeRecord(3, cx, cy, r, 0, color, filled);
    return 0;
}
