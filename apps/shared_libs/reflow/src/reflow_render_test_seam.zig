//! Test-only link seam for the reflow Zig unit test harness (RA8FW-1222).
//!
//! The render module's rasteriser reaches three Layer-3 surfaces behind the
//! ported reflow_render.zig: the ra8_glyph_atlas glyph cache (libs/ra8_mem),
//! the ra8_gfx pixel hook, and the SVG / imgdec decode stack behind
//! reflow_svg.c / reflow_image.c. That stack is out of scope for this unit
//! harness (the CMake suite exercises it end to end), so the harness answers
//! each surface with its documented "not supported" status, which the render
//! path handles by falling back: direct rasterisation for the atlas, and a
//! skipped (blank) box for SVG and raster images. The pixel hook accepts
//! every write. The real blank-code-point policy now ships in
//! reflow_render.zig itself, so the seam exports none of it. The seam must
//! never be imported by reflow_parse_abi.zig: the archive keeps no stub.
const k_ra8_ok: c_int = 0;
const k_ra8_err_not_supported: c_int = 0x107;

export fn ra8_gfx_pixel(x: c_int, y: c_int, color: u32) callconv(.c) c_int {
    _ = x;
    _ = y;
    _ = color;
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

export fn ra8_svg_is_svg(bytes: [*]const u8, len: usize) callconv(.c) bool {
    _ = bytes;
    _ = len;
    return false;
}

export fn ra8_svg_image_href(svg: [*]const u8, len: usize, out_off: *usize, out_len: *usize) callconv(.c) c_int {
    _ = svg;
    _ = len;
    _ = out_off;
    _ = out_len;
    return k_ra8_err_not_supported;
}

export fn ra8_svg_render(svg: [*]const u8, len: usize, x: c_int, y: c_int, w: c_int, h: c_int) callconv(.c) c_int {
    _ = svg;
    _ = len;
    _ = x;
    _ = y;
    _ = w;
    _ = h;
    return k_ra8_ok;
}

export fn ra8_img_decode_blit(arena: *anyopaque, bytes: [*]const u8, len: usize, dst_x: c_int, dst_y: c_int, box_w: c_int, box_h: c_int, out_w: ?*i32, out_h: ?*i32) callconv(.c) c_int {
    _ = arena;
    _ = bytes;
    _ = len;
    _ = dst_x;
    _ = dst_y;
    _ = box_w;
    _ = box_h;
    _ = out_w;
    _ = out_h;
    return k_ra8_err_not_supported;
}
