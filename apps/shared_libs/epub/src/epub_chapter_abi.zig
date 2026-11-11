//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the epub chapter/metadata/cover/glyph accessors
//! (RA8FW-1163): the C-era src/epub_chapter.c ported verbatim behind the
//! unchanged C headers. Everything here reads against an already-open
//! epub_book_t: the spine, TOC, metadata and manifest accessors, the
//! miniz-backed chapter/cover/resource/font extractors, the TTF attach
//! path, and the stb_truetype glyph rasteriser. miniz (SOUP), stb_truetype
//! (SOUP), the sfnt guard (ra8_stbtt_guard.h, stays C in reflow) and the
//! zip guard exports (the ra8_epub archive) all stay extern, so this TU
//! adds no policy of its own. The TU ships as its own archive
//! (ra8_epub_chapter) so a consumer that never reads chapter/glyph data
//! never pulls the miniz or stbtt externs. The priv_* helpers this TU
//! exports (join_path, book_not_ready, glyph_dim_invalid) are consumed by
//! the entry archive (epub_entry_abi.zig) and by the open/close archive, so this archive
//! sits after ra8_epub_open in the link order.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_not_found: ra8_err_t = 0x106;
const k_ra8_err_not_initialized: ra8_err_t = 0x10F;
const k_ra8_err_out_of_range: ra8_err_t = 0x208;
const k_ra8_err_validation_failed: ra8_err_t = 0x501;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

// ---- epub.h limits ---------------------------------------------------------
const k_epub_max_chapters = 64;
const k_epub_max_toc = 64;
const k_epub_max_path_len = 192;
const k_epub_meta_len = 128;
const k_epub_max_fonts = 8;
const k_epub_max_manifest = 96;
const k_epub_id_len = 64;
const k_epub_media_len = 48;
const k_epub_zip_archive_bytes = 256;

// ---- the book (epub.h epub_book_t, exact C layout; mirrors the open TU) ----
const epub_toc_entry_t = extern struct { title: [k_epub_meta_len]u8, href: [k_epub_max_path_len]u8, depth: u8 };
const epub_manifest_item_t = extern struct { id: [k_epub_id_len]u8, href: [k_epub_max_path_len]u8, media_type: [k_epub_media_len]u8 };
const epub_metadata_t = extern struct {
    title: ?[*:0]const u8,
    author: ?[*:0]const u8,
    language: ?[*:0]const u8,
    identifier: ?[*:0]const u8,
};

const epub_miniz_arena_t = extern struct { base: ?[*]u8, capacity: usize, initialized: u8 };
const epub_miniz_workspace_t = extern struct { bytes: [163840]u8 align(16) };
const xml_span_t = extern struct { offset: u32, length: u32 };
const xml_frame_t = extern struct { name_offset: u32, name_length: u16, consumer: u16 };
const xml_reader_workspace_t = extern struct { frames: [512]xml_frame_t };
const epub_xml_workspace_t = extern struct {
    reader: xml_reader_workspace_t,
    references: [k_epub_max_chapters]xml_span_t,
    legacy_cover_id: xml_span_t,
    reference_count: u16,
};
const epub_stream_media_t = extern struct { read: ?*const fn (?*anyopaque, u64, ?*anyopaque, u32, ?*u32) callconv(.c) ra8_err_t, ctx: ?*anyopaque, size: u64 };

pub const epub_book_t = extern struct {
    zip_bytes: ?[*]const u8,
    zip_size: usize,
    zip_archive_storage: [k_epub_zip_archive_bytes]u8 align(16),
    zip_archive_active: u8,
    miniz_arena: epub_miniz_arena_t,
    miniz_workspace: epub_miniz_workspace_t,
    xml_workspace: epub_xml_workspace_t,
    stream_media: epub_stream_media_t,
    chapter_count: u16,
    chapter_paths: [k_epub_max_chapters][k_epub_max_path_len]u8,
    title: [k_epub_meta_len]u8,
    author: [k_epub_meta_len]u8,
    language: [k_epub_meta_len]u8,
    identifier: [k_epub_meta_len]u8,
    cover_path: [k_epub_max_path_len]u8,
    embedded_font_count: u16,
    embedded_font_paths: [k_epub_max_fonts][k_epub_max_path_len]u8,
    manifest_count: u16,
    manifest: [k_epub_max_manifest]epub_manifest_item_t,
    opf_dir: [k_epub_max_path_len]u8,
    toc_path: [k_epub_max_path_len]u8,
    toc_kind: u8,
    toc_count: u16,
    toc: [k_epub_max_toc]epub_toc_entry_t,
    font_data: ?[*]const u8,
    font_size: usize,
    in_use: u8,
};

comptime {
    if (k_epub_zip_archive_bytes < 112)
        @compileError("k_epub_zip_archive_bytes too small for mz_zip_archive");
}

// ---- miniz (SOUP, apps/shared_libs/third_party/miniz/miniz.h) ---------------
const mz_zip_archive = extern struct {
    m_archive_size: u64,
    m_central_directory_file_ofs: u64,
    m_total_files: c_uint,
    m_zip_mode: c_int,
    m_zip_type: c_int,
    m_last_error: c_int,
    m_file_offset_alignment: u64,
    m_pAlloc: ?*const fn (?*anyopaque, usize, usize) callconv(.c) ?*anyopaque,
    m_pFree: ?*const fn (?*anyopaque, ?*anyopaque) callconv(.c) void,
    m_pRealloc: ?*const fn (?*anyopaque, ?*anyopaque, usize, usize) callconv(.c) ?*anyopaque,
    m_pAlloc_opaque: ?*anyopaque,
    m_pRead: ?*const fn (?*anyopaque, u64, ?*anyopaque, usize) callconv(.c) usize,
    m_pWrite: ?*const fn (?*anyopaque, u64, ?*const anyopaque, usize) callconv(.c) usize,
    m_pNeeds_keepalive: ?*const fn (?*anyopaque, u64, ?*const anyopaque, usize) callconv(.c) c_int,
    m_pIO_opaque: ?*anyopaque,
    m_pState: ?*anyopaque,
};

// Full C layout (probed against the vendored miniz in the RA8FW-1165 slice):
// 1112 bytes with the 512-byte filename, the 512-byte comment and the
// trailing time slot. The earlier partial mirror undersized the struct while
// a production link reaches the real mz_zip_reader_file_stat, which writes
// the whole thing.
const mz_zip_archive_file_stat = extern struct {
    m_file_index: c_uint,
    m_central_dir_ofs: u64,
    m_version_made_by: c_ushort,
    m_version_needed: c_ushort,
    m_bit_flag: c_ushort,
    m_method: c_ushort,
    m_crc32: c_uint,
    m_comp_size: u64,
    m_uncomp_size: u64,
    m_internal_attr: c_ushort,
    m_external_attr: c_uint,
    m_local_header_ofs: u64,
    m_comment_size: c_uint,
    m_is_directory: c_int,
    m_is_encrypted: c_int,
    m_is_supported: c_int,
    m_filename: [512]u8,
    m_comment: [512]u8,
    m_time: c_long, // MZ_TIME_T slot: m_time, or m_padding under MINIZ_NO_TIME.
};

// ---- stb_truetype (SOUP, stays C) -------------------------------------------
// The TU only ever passes fontinfo handles through the stbtt externs, so the
// type is mirrored by size alone: sizeof(stbtt_fontinfo) on this target is
// 168 (probed against the vendored header). The handle lives on the stack
// exactly as the C TU keeps it.
const k_stbtt_fontinfo_bytes = 168;
const stbtt_fontinfo = extern struct { storage: [k_stbtt_fontinfo_bytes]u8 align(8) };

extern fn mz_zip_reader_locate_file(pZip: *mz_zip_archive, name: [*]const u8, comment: ?[*]const u8, flags: c_uint) callconv(.c) c_int;
extern fn mz_zip_reader_file_stat(pZip: *mz_zip_archive, index: c_uint, pStat: *mz_zip_archive_file_stat) callconv(.c) c_int;
extern fn mz_zip_reader_extract_to_mem(pZip: *mz_zip_archive, index: c_uint, buf: [*]u8, len: usize, flags: c_uint) callconv(.c) c_int;

extern fn priv_epub_zip_guard_entry(pStat: *const mz_zip_archive_file_stat) callconv(.c) ra8_err_t;

extern fn stbtt_GetFontOffsetForIndex(data: [*]const u8, index: c_int) callconv(.c) c_int;
extern fn stbtt_InitFont(info: *stbtt_fontinfo, data: [*]const u8, data_size: c_int, offset: c_int) callconv(.c) c_int;
extern fn stbtt_ScaleForPixelHeight(info: *const stbtt_fontinfo, scale: f32) callconv(.c) f32;
extern fn stbtt_GetCodepointBitmapBox(info: *const stbtt_fontinfo, codepoint: c_int, xs: f32, ys: f32, ix0: *c_int, iy0: *c_int, ix1: *c_int, iy1: *c_int) callconv(.c) void;
extern fn stbtt_MakeCodepointBitmap(info: *const stbtt_fontinfo, output: [*]u8, out_w: c_int, out_h: c_int, stride: c_int, xs: f32, ys: f32, codepoint: c_int) callconv(.c) void;

extern fn ra8_stbtt_sfnt_dir_in_bounds(data: [*]const u8, len: usize, fontstart: c_uint) callconv(.c) bool;

// Test-only knobs, defined by teststub/chapter_test_stubs.c.
extern var teststub_locate_miss_prefixed: c_int;
extern var teststub_fail_stat: c_int;
extern var teststub_guard_result: c_int;
extern var teststub_fail_extract: c_int;
extern var teststub_entry_uncomp: u64;
extern var teststub_font_offset: c_int;
extern var teststub_fail_sfnt_dir: c_int;
extern var teststub_fail_font_init: c_int;
extern var teststub_box_x0: c_int;
extern var teststub_box_y0: c_int;
extern var teststub_box_x1: c_int;
extern var teststub_box_y1: c_int;

// Test-only capture accessors, defined by teststub/chapter_test_stubs.c.
extern fn teststub_located_prefixed() [*:0]const u8;
extern fn teststub_located_bare() [*:0]const u8;
extern fn teststub_extract_len() usize;
extern fn teststub_extract_buf() [*]const u8;
extern fn teststub_locate_calls() c_int;
extern fn teststub_scale() f32;
extern fn teststub_make_stride() c_int;
extern fn teststub_make_codepoint() c_int;
extern fn teststub_reset() void;

const testing = std.testing;

// ---- C-string helpers -------------------------------------------------------
fn c_strlen(p: [*]const u8) usize {
    var n: usize = 0;
    while (p[n] != 0) n += 1;
    return n;
}

// ---- priv_* helpers (exported; consumed by the entry archive and the open TU) ----
export fn priv_epub_glyph_dim_invalid(w: c_int, h: c_int) callconv(.c) c_int {
    return @intFromBool((w < 0) or (h < 0));
}

export fn priv_epub_book_not_ready(in_use: u8, zip_archive_active: u8) callconv(.c) c_int {
    return @intFromBool((in_use == 0) or (zip_archive_active == 0));
}

export fn priv_epub_join_path(dir: ?[*]const u8, name: ?[*]const u8, dst: ?[*]u8, cap: usize) callconv(.c) void {
    if ((dst == null) or (cap == 0)) return;
    dst.?[0] = 0;
    var off: usize = 0;
    if (dir) |d| {
        while ((off < (cap - 1)) and (d[off] != 0)) {
            dst.?[off] = d[off];
            off += 1;
        }
    }
    if (name) |n| {
        var i: usize = 0;
        while ((off < (cap - 1)) and (n[i] != 0)) {
            dst.?[off] = n[i];
            off += 1;
            i += 1;
        }
    }
    dst.?[off] = 0;
}

// ---- internals ---------------------------------------------------------------
fn internal_locate_extract(zip: *mz_zip_archive, prefixed_path: [*]const u8, bare_path: [*]const u8, out_buf: [*]u8, max_len: usize, got_len: *usize) ra8_err_t {
    var file_idx = mz_zip_reader_locate_file(zip, prefixed_path, null, 0);
    if (file_idx < 0) {
        file_idx = mz_zip_reader_locate_file(zip, bare_path, null, 0);
    }
    if (file_idx < 0) {
        return k_ra8_err_not_found;
    }
    var st = std.mem.zeroes(mz_zip_archive_file_stat);
    // Name lookup does not fully parse extended-size metadata, so malformed
    // central-directory fields may still fail at this validation boundary.
    if (mz_zip_reader_file_stat(zip, @intCast(file_idx), &st) == 0) {
        return k_ra8_err_validation_failed;
    }
    const gerr = priv_epub_zip_guard_entry(&st);
    if (gerr != k_ra8_ok) {
        return gerr; // lying header / declared bomb: reject before inflation
    }
    if (st.m_uncomp_size > max_len) {
        return k_ra8_err_no_mem;
    }
    // Defensive: extract can only fail here if the compressed stream is
    // corrupt (bad CRC or LZ data), which requires a malformed archive.
    if (mz_zip_reader_extract_to_mem(zip, @intCast(file_idx), out_buf, max_len, 0) == 0) {
        return k_ra8_err_validation_failed;
    }
    got_len.* = @intCast(st.m_uncomp_size);
    return k_ra8_ok;
}

fn internal_font_init(book: *const epub_book_t, out_font: *stbtt_fontinfo) ra8_err_t {
    // Guard against malformed font blobs: stbtt_GetFontOffsetForIndex
    // returns -1 for non-TTF input, and stbtt_InitFont dereferences past
    // the buffer when handed that offset.
    const offset = stbtt_GetFontOffsetForIndex(book.font_data.?, 0);
    if (offset < 0) {
        return k_ra8_err_validation_failed;
    }
    // Bound-check the sfnt table directory before stbtt_InitFont walks it: the
    // font bytes are attacker-controlled (an EPUB @font-face / epub_set_font
    // blob) and stb_truetype reads the directory with no length check.
    if (!ra8_stbtt_sfnt_dir_in_bounds(book.font_data.?, book.font_size, @intCast(offset))) {
        return k_ra8_err_validation_failed;
    }
    if (stbtt_InitFont(out_font, book.font_data.?, @intCast(book.font_size), offset) == 0) {
        return k_ra8_err_validation_failed;
    }
    return k_ra8_ok;
}

fn internal_render_into(font: *const stbtt_fontinfo, codepoint: i32, font_size: f32, out_bitmap: [*]u8, max_pixels: usize, out_w: *u32, out_h: *u32) ra8_err_t {
    // No-allocation glyph rasterisation: the two-step Box + Make path that
    // writes into the caller's buffer (no STBTT_malloc inside).
    const scale = stbtt_ScaleForPixelHeight(font, font_size);
    var x0: c_int = 0;
    var y0: c_int = 0;
    var x1: c_int = 0;
    var y1: c_int = 0;
    stbtt_GetCodepointBitmapBox(font, codepoint, scale, scale, &x0, &y0, &x1, &y1);
    const w = x1 - x0;
    const h = y1 - y0;
    // The raw glyf bounding box remains untrusted after the font directory has
    // initialized; reject reversed coordinates before the signed dimensions are
    // converted to size_t.
    if (priv_epub_glyph_dim_invalid(w, h) != 0) {
        return k_ra8_err_validation_failed;
    }
    const total = @as(usize, @intCast(w)) * @as(usize, @intCast(h));
    if (total > max_pixels) {
        return k_ra8_err_no_mem;
    }
    if (total != 0) {
        // Stride = w (tightly packed alpha-8). MakeCodepointBitmap writes
        // directly into out_bitmap with no malloc.
        stbtt_MakeCodepointBitmap(font, out_bitmap, w, h, w, scale, scale, codepoint);
    }
    out_w.* = @intCast(w);
    out_h.* = @intCast(h);
    return k_ra8_ok;
}

// ---- public API ----------------------------------------------------------------
export fn epub_get_chapter_count(book: ?*const epub_book_t, out_count: ?*u16) callconv(.c) ra8_err_t {
    if ((book == null) or (out_count == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_count.?.* = book.?.chapter_count;
    return k_ra8_ok;
}

export fn epub_load_chapter(book: ?*epub_book_t, idx: u16, out_xhtml: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
    if ((book == null) or (out_xhtml == null) or (got_len == null)) {
        return k_ra8_err_null_ptr;
    }
    got_len.?.* = 0;
    if (priv_epub_book_not_ready(book.?.in_use, book.?.zip_archive_active) != 0) {
        return k_ra8_err_not_initialized;
    }
    if (max_len == 0) {
        return k_ra8_err_invalid_size;
    }
    if (idx >= book.?.chapter_count) {
        return k_ra8_err_out_of_range;
    }

    var full_path: [k_epub_max_path_len]u8 = undefined;
    priv_epub_join_path(&book.?.opf_dir, &book.?.chapter_paths[idx], &full_path, full_path.len);

    const zip: *mz_zip_archive = @ptrCast(@alignCast(&book.?.zip_archive_storage));
    return internal_locate_extract(zip, &full_path, &book.?.chapter_paths[idx], out_xhtml.?, max_len, got_len.?);
}

export fn epub_get_toc_kind(book: ?*const epub_book_t, out_kind: ?*u8) callconv(.c) ra8_err_t {
    if ((book == null) or (out_kind == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_kind.?.* = book.?.toc_kind;
    return k_ra8_ok;
}

export fn epub_get_toc_count(book: ?*const epub_book_t, out_count: ?*u16) callconv(.c) ra8_err_t {
    if ((book == null) or (out_count == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_count.?.* = book.?.toc_count;
    return k_ra8_ok;
}

export fn epub_get_toc_entry(book: ?*const epub_book_t, idx: u16, out_entry: ?*epub_toc_entry_t) callconv(.c) ra8_err_t {
    if ((book == null) or (out_entry == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (idx >= book.?.toc_count) {
        return k_ra8_err_out_of_range;
    }
    out_entry.?.* = book.?.toc[idx];
    return k_ra8_ok;
}

export fn epub_toc_entry_to_chapter(book: ?*const epub_book_t, toc_idx: u16, out_chapter_idx: ?*u16) callconv(.c) ra8_err_t {
    if ((book == null) or (out_chapter_idx == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (toc_idx >= book.?.toc_count) {
        return k_ra8_err_out_of_range;
    }

    // Match the entry href up to (but excluding) any "#fragment": the spine
    // stores whole-document paths, while a TOC href may target an anchor.
    const href = @as([*]const u8, @ptrCast(&book.?.toc[toc_idx].href));
    var base_len: usize = 0;
    while ((href[base_len] != 0) and (href[base_len] != '#')) {
        base_len += 1;
    }

    var i: u16 = 0;
    while (i < book.?.chapter_count) : (i += 1) {
        const path = @as([*]const u8, @ptrCast(&book.?.chapter_paths[i]));
        if (c_strlen(path) != base_len) {
            continue;
        }
        if (base_len != 0 and !std.mem.eql(u8, path[0..base_len], href[0..base_len])) {
            continue;
        }
        out_chapter_idx.?.* = i;
        return k_ra8_ok;
    }
    return k_ra8_err_not_found;
}

export fn epub_get_metadata(book: ?*const epub_book_t, out_meta: ?*epub_metadata_t) callconv(.c) ra8_err_t {
    if ((book == null) or (out_meta == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_meta.?.title = @ptrCast(&book.?.title);
    out_meta.?.author = @ptrCast(&book.?.author);
    out_meta.?.language = @ptrCast(&book.?.language);
    out_meta.?.identifier = @ptrCast(&book.?.identifier);
    return k_ra8_ok;
}

export fn epub_get_cover_image(book: ?*epub_book_t, out_buf: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
    if ((book == null) or (out_buf == null) or (got_len == null)) {
        return k_ra8_err_null_ptr;
    }
    got_len.?.* = 0;
    if (priv_epub_book_not_ready(book.?.in_use, book.?.zip_archive_active) != 0) {
        return k_ra8_err_not_initialized;
    }
    if (max_len == 0) {
        return k_ra8_err_invalid_size;
    }
    if (book.?.cover_path[0] == 0) {
        return k_ra8_err_not_found;
    }

    var full_path: [k_epub_max_path_len]u8 = undefined;
    priv_epub_join_path(&book.?.opf_dir, &book.?.cover_path, &full_path, full_path.len);

    const zip: *mz_zip_archive = @ptrCast(@alignCast(&book.?.zip_archive_storage));
    return internal_locate_extract(zip, &full_path, &book.?.cover_path, out_buf.?, max_len, got_len.?);
}

export fn epub_get_resource(book: ?*epub_book_t, path: ?[*:0]const u8, out_buf: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
    if ((book == null) or (path == null) or (out_buf == null) or (got_len == null)) {
        return k_ra8_err_null_ptr;
    }
    got_len.?.* = 0;
    if (priv_epub_book_not_ready(book.?.in_use, book.?.zip_archive_active) != 0) {
        return k_ra8_err_not_initialized;
    }
    if (max_len == 0) {
        return k_ra8_err_invalid_size;
    }

    var full_path: [k_epub_max_path_len]u8 = undefined;
    priv_epub_join_path(&book.?.opf_dir, path.?, &full_path, full_path.len);

    const zip: *mz_zip_archive = @ptrCast(@alignCast(&book.?.zip_archive_storage));
    return internal_locate_extract(zip, &full_path, path.?, out_buf.?, max_len, got_len.?);
}

export fn epub_get_embedded_font_count(book: ?*const epub_book_t, out_count: ?*u16) callconv(.c) ra8_err_t {
    if ((book == null) or (out_count == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_count.?.* = book.?.embedded_font_count;
    return k_ra8_ok;
}

export fn epub_manifest_count(book: ?*const epub_book_t) callconv(.c) u16 {
    if (book == null) {
        return 0;
    }
    if (book.?.in_use == 0) {
        return 0;
    }
    return book.?.manifest_count;
}

export fn epub_manifest_item(book: ?*const epub_book_t, index: u16) callconv(.c) ?*const epub_manifest_item_t {
    if ((book == null) or (book.?.in_use == 0)) {
        return null;
    }
    if (index >= book.?.manifest_count) {
        return null;
    }
    return &book.?.manifest[index];
}

export fn epub_get_embedded_font(book: ?*epub_book_t, idx: u16, out_buf: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
    if ((book == null) or (out_buf == null) or (got_len == null)) {
        return k_ra8_err_null_ptr;
    }
    got_len.?.* = 0;
    if (priv_epub_book_not_ready(book.?.in_use, book.?.zip_archive_active) != 0) {
        return k_ra8_err_not_initialized;
    }
    if (max_len == 0) {
        return k_ra8_err_invalid_size;
    }
    if (idx >= book.?.embedded_font_count) {
        return k_ra8_err_out_of_range;
    }

    var full_path: [k_epub_max_path_len]u8 = undefined;
    priv_epub_join_path(&book.?.opf_dir, &book.?.embedded_font_paths[idx], &full_path, full_path.len);

    const zip: *mz_zip_archive = @ptrCast(@alignCast(&book.?.zip_archive_storage));
    return internal_locate_extract(zip, &full_path, &book.?.embedded_font_paths[idx], out_buf.?, max_len, got_len.?);
}

const k_epub_min_font_bytes = 16;

export fn epub_set_font(book: ?*epub_book_t, font_data: ?[*]const u8, font_size: usize) callconv(.c) ra8_err_t {
    if ((book == null) or (font_data == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (font_size < k_epub_min_font_bytes) {
        return k_ra8_err_invalid_size;
    }
    book.?.font_data = font_data;
    book.?.font_size = font_size;
    return k_ra8_ok;
}

export fn epub_render_glyph(book: ?*epub_book_t, codepoint: i32, font_size: f32, out_bitmap: ?[*]u8, max_pixels: usize, out_w: ?*u32, out_h: ?*u32) callconv(.c) ra8_err_t {
    if ((book == null) or (out_bitmap == null) or (out_w == null) or (out_h == null)) {
        return k_ra8_err_null_ptr;
    }
    out_w.?.* = 0;
    out_h.?.* = 0;
    if ((book.?.in_use == 0) or (book.?.font_data == null)) {
        return k_ra8_err_not_initialized;
    }
    if (font_size <= 0.0) {
        return k_ra8_err_invalid_arg;
    }

    var font: stbtt_fontinfo = undefined;
    const err = internal_font_init(book.?, &font);
    if (err != k_ra8_ok) {
        return err;
    }
    return internal_render_into(&font, codepoint, font_size, out_bitmap.?, max_pixels, out_w.?, out_h.?);
}

// ---------------------------------------------------------------------------
// Tests (stub-driven: no real archive, no real font).
// ---------------------------------------------------------------------------

fn setStr(comptime N: usize, buf: *[N]u8, s: []const u8) void {
    @memcpy(buf[0..s.len], s);
    buf[s.len] = 0;
}

fn freshBook() epub_book_t {
    teststub_reset();
    var book = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    book.chapter_count = 2;
    setStr(192, &book.chapter_paths[0], "ch1.xhtml");
    setStr(192, &book.chapter_paths[1], "ch2.xhtml");
    setStr(192, &book.opf_dir, "oebps/");
    setStr(192, &book.cover_path, "cover.png");
    setStr(192, &book.embedded_font_paths[0], "fonts/a.ttf");
    book.embedded_font_count = 1;
    book.manifest_count = 1;
    setStr(64, &book.manifest[0].id, "id1");
    setStr(192, &book.manifest[0].href, "ch1.xhtml");
    book.toc_count = 1;
    setStr(192, &book.toc[0].href, "ch1.xhtml#sec2");
    book.toc_kind = 2;
    return book;
}

fn freshBuf() [256]u8 {
    return @splat(0);
}

test "join_path concatenates dir and name" {
    var out: [32]u8 = undefined;
    priv_epub_join_path("oebps/", "ch1.xhtml", &out, out.len);
    try testing.expectEqualStrings("oebps/ch1.xhtml", std.mem.sliceTo(&out, 0));
}

test "join_path truncates to cap-1 and NUL-terminates" {
    var out: [8]u8 = undefined;
    priv_epub_join_path("oebps/", "ch1.xhtml", &out, out.len);
    try testing.expectEqualStrings("oebps/c", std.mem.sliceTo(&out, 0));
}

test "join_path with null dir writes name only" {
    var out: [16]u8 = undefined;
    priv_epub_join_path(null, "x.html", &out, out.len);
    try testing.expectEqualStrings("x.html", std.mem.sliceTo(&out, 0));
}

test "chapter_count ok, null and not-initialized paths" {
    var book = freshBook();
    var n: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_chapter_count(&book, &n));
    try testing.expectEqual(@as(u16, 2), n);
    try testing.expectEqual(k_ra8_err_null_ptr, epub_get_chapter_count(null, &n));
    book.in_use = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_get_chapter_count(&book, &n));
}

test "load_chapter happy path joins opf_dir, extracts and sets got_len" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 5;
    try testing.expectEqual(k_ra8_ok, epub_load_chapter(&book, 0, &buf, buf.len, &got));
    try testing.expectEqualStrings("oebps/ch1.xhtml", std.mem.sliceTo(teststub_located_prefixed(), 0));
    try testing.expectEqual(@as(usize, 5), got);
    try testing.expectEqual(@as(usize, 5), teststub_extract_len());
    try testing.expectEqual(@as(c_int, 1), teststub_locate_calls());
}

test "load_chapter falls back to the bare path on a prefixed miss" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_locate_miss_prefixed = 1;
    defer teststub_locate_miss_prefixed = 0;
    try testing.expectEqual(k_ra8_ok, epub_load_chapter(&book, 1, &buf, buf.len, &got));
    try testing.expectEqualStrings("ch2.xhtml", std.mem.sliceTo(teststub_located_bare(), 0));
    try testing.expectEqual(@as(c_int, 2), teststub_locate_calls());
}

test "load_chapter rejects a not-ready book, zero max_len and an out-of-range index" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 9;
    book.zip_archive_active = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_load_chapter(&book, 0, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 0), got);
    book.zip_archive_active = 1;
    try testing.expectEqual(k_ra8_err_invalid_size, epub_load_chapter(&book, 0, &buf, 0, &got));
    try testing.expectEqual(k_ra8_err_out_of_range, epub_load_chapter(&book, 2, &buf, buf.len, &got));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_load_chapter(&book, 0, null, buf.len, &got));
}

test "load_chapter propagates the guard verdict before inflation" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_guard_result = 0x501;
    defer teststub_guard_result = 0;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_load_chapter(&book, 0, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 0), got);
}

test "load_chapter rejects an entry larger than the caller buffer" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 1000;
    defer teststub_entry_uncomp = 16;
    try testing.expectEqual(k_ra8_err_no_mem, epub_load_chapter(&book, 0, &buf, buf.len, &got));
}

test "load_chapter maps an extract failure to validation_failed" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_fail_extract = 1;
    defer teststub_fail_extract = 0;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_load_chapter(&book, 0, &buf, buf.len, &got));
}

test "toc accessors: kind, count, entry and out-of-range" {
    var book = freshBook();
    var kind: u8 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_toc_kind(&book, &kind));
    try testing.expectEqual(@as(u8, 2), kind);
    var n: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_toc_count(&book, &n));
    try testing.expectEqual(@as(u16, 1), n);
    var entry: epub_toc_entry_t = undefined;
    try testing.expectEqual(k_ra8_ok, epub_get_toc_entry(&book, 0, &entry));
    try testing.expectEqualStrings("ch1.xhtml#sec2", std.mem.sliceTo(&entry.href, 0));
    try testing.expectEqual(k_ra8_err_out_of_range, epub_get_toc_entry(&book, 1, &entry));
}

test "toc_entry_to_chapter strips the fragment and finds the spine index" {
    var book = freshBook();
    var idx: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_toc_entry_to_chapter(&book, 0, &idx));
    try testing.expectEqual(@as(u16, 0), idx);
    setStr(192, &book.toc[0].href, "missing.xhtml");
    try testing.expectEqual(k_ra8_err_not_found, epub_toc_entry_to_chapter(&book, 0, &idx));
    try testing.expectEqual(k_ra8_err_out_of_range, epub_toc_entry_to_chapter(&book, 1, &idx));
}

test "metadata returns pointers into the book" {
    var book = freshBook();
    setStr(128, &book.title, "A Book");
    setStr(128, &book.author, "An Author");
    var meta: epub_metadata_t = undefined;
    try testing.expectEqual(k_ra8_ok, epub_get_metadata(&book, &meta));
    try testing.expectEqualStrings("A Book", std.mem.sliceTo(meta.title.?, 0));
    try testing.expectEqualStrings("An Author", std.mem.sliceTo(meta.author.?, 0));
    book.in_use = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_get_metadata(&book, &meta));
}

test "cover_image: empty cover path is not-found, real path extracts" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 32;
    try testing.expectEqual(k_ra8_ok, epub_get_cover_image(&book, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 32), got);
    book.cover_path[0] = 0;
    try testing.expectEqual(k_ra8_err_not_found, epub_get_cover_image(&book, &buf, buf.len, &got));
}

test "resource extracts by the caller path" {
    var book = freshBook();
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 9;
    try testing.expectEqual(k_ra8_ok, epub_get_resource(&book, "css/main.css", &buf, buf.len, &got));
    try testing.expectEqualStrings("oebps/css/main.css", std.mem.sliceTo(teststub_located_prefixed(), 0));
    try testing.expectEqual(k_ra8_err_invalid_size, epub_get_resource(&book, "css/main.css", &buf, 0, &got));
}

test "manifest accessors: count, item pointer, not-ready and out-of-range" {
    var book = freshBook();
    try testing.expectEqual(@as(u16, 1), epub_manifest_count(&book));
    const item = epub_manifest_item(&book, 0).?;
    try testing.expectEqualStrings("id1", std.mem.sliceTo(&item.id, 0));
    try testing.expect(epub_manifest_item(&book, 1) == null);
    book.in_use = 0;
    try testing.expectEqual(@as(u16, 0), epub_manifest_count(&book));
    try testing.expect(epub_manifest_item(&book, 0) == null);
}

test "embedded font: count, extract and out-of-range" {
    var book = freshBook();
    var n: u16 = 0;
    try testing.expectEqual(k_ra8_ok, epub_get_embedded_font_count(&book, &n));
    try testing.expectEqual(@as(u16, 1), n);
    var buf = freshBuf();
    var got: usize = 0;
    teststub_entry_uncomp = 128;
    try testing.expectEqual(k_ra8_ok, epub_get_embedded_font(&book, 0, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 128), got);
    try testing.expectEqual(k_ra8_err_out_of_range, epub_get_embedded_font(&book, 1, &buf, buf.len, &got));
}

test "set_font rejects a tiny blob and accepts a real one" {
    var book = freshBook();
    var blob = freshBuf();
    blob[0] = 0x00;
    try testing.expectEqual(k_ra8_err_invalid_size, epub_set_font(&book, &blob, 8));
    try testing.expectEqual(k_ra8_ok, epub_set_font(&book, &blob, blob.len));
    try testing.expect(book.font_data != null);
    try testing.expectEqual(@as(usize, 256), book.font_size);
    book.in_use = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_set_font(&book, &blob, blob.len));
}

test "render_glyph: not-init, bad size, sfnt-dir and reversed-box rejects" {
    var book = freshBook();
    var blob = freshBuf();
    var bmp = freshBuf();
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    try testing.expectEqual(@as(u32, 0), w);
    try testing.expectEqual(k_ra8_ok, epub_set_font(&book, &blob, blob.len));
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_render_glyph(&book, 65, 0, &bmp, bmp.len, &w, &h));
    teststub_fail_sfnt_dir = 1;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    teststub_fail_sfnt_dir = 0;
    teststub_box_x0 = 4;
    teststub_box_x1 = 2;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    teststub_box_x0 = 0;
    teststub_box_x1 = 8;
}

test "render_glyph rejects a bitmap over the pixel budget and renders under it" {
    var book = freshBook();
    var blob = freshBuf();
    var bmp = freshBuf();
    var w: u32 = 0;
    var h: u32 = 0;
    try testing.expectEqual(k_ra8_ok, epub_set_font(&book, &blob, blob.len));
    try testing.expectEqual(k_ra8_err_no_mem, epub_render_glyph(&book, 65, 12, &bmp, 7, &w, &h));
    try testing.expectEqual(k_ra8_ok, epub_render_glyph(&book, 65, 12, &bmp, bmp.len, &w, &h));
    try testing.expectEqual(@as(u32, 8), w);
    try testing.expectEqual(@as(u32, 16), h);
    try testing.expectEqual(@as(c_int, 8), teststub_make_stride());
    try testing.expectEqual(@as(c_int, 65), teststub_make_codepoint());
    try testing.expect(teststub_scale() > 0.0);
}
