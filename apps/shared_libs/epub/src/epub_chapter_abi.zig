//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the epub chapter/metadata/cover/glyph accessors
//! (RA8FW-1163): the C-era src/epub_chapter.c ported verbatim behind the
//! unchanged C headers. Everything here reads against an already-open
//! epub_book_t: the spine, TOC, metadata and manifest accessors, the
//! miniz-backed chapter/cover/resource/font extractors, the TTF attach
//! path, and the stb_truetype glyph rasteriser. miniz (SOUP), stb_truetype
//! (SOUP), the sfnt guard (ra8_stbtt_guard.h; its implementation moved to the
//! Zig ra8_stbtt_guard.zig archive member, RA8FW-1240) and the
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
pub const k_ra8_ok: ra8_err_t = 0;
pub const k_ra8_err_no_mem: ra8_err_t = 0x102;
pub const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
pub const k_ra8_err_invalid_size: ra8_err_t = 0x105;
pub const k_ra8_err_not_found: ra8_err_t = 0x106;
pub const k_ra8_err_not_initialized: ra8_err_t = 0x10F;
pub const k_ra8_err_out_of_range: ra8_err_t = 0x208;
pub const k_ra8_err_validation_failed: ra8_err_t = 0x501;
pub const k_ra8_err_null_ptr: ra8_err_t = 0x504;

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
pub const epub_toc_entry_t = extern struct { title: [k_epub_meta_len]u8, href: [k_epub_max_path_len]u8, depth: u8 };
const epub_manifest_item_t = extern struct { id: [k_epub_id_len]u8, href: [k_epub_max_path_len]u8, media_type: [k_epub_media_len]u8 };
pub const epub_metadata_t = extern struct {
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

// Test-only capture accessors, defined by teststub/chapter_test_stubs.c.

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

pub export fn priv_epub_join_path(dir: ?[*]const u8, name: ?[*]const u8, dst: ?[*]u8, cap: usize) callconv(.c) void {
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
pub export fn epub_get_chapter_count(book: ?*const epub_book_t, out_count: ?*u16) callconv(.c) ra8_err_t {
    if ((book == null) or (out_count == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_count.?.* = book.?.chapter_count;
    return k_ra8_ok;
}

pub export fn epub_load_chapter(book: ?*epub_book_t, idx: u16, out_xhtml: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
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

pub export fn epub_get_toc_kind(book: ?*const epub_book_t, out_kind: ?*u8) callconv(.c) ra8_err_t {
    if ((book == null) or (out_kind == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_kind.?.* = book.?.toc_kind;
    return k_ra8_ok;
}

pub export fn epub_get_toc_count(book: ?*const epub_book_t, out_count: ?*u16) callconv(.c) ra8_err_t {
    if ((book == null) or (out_count == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_count.?.* = book.?.toc_count;
    return k_ra8_ok;
}

pub export fn epub_get_toc_entry(book: ?*const epub_book_t, idx: u16, out_entry: ?*epub_toc_entry_t) callconv(.c) ra8_err_t {
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

pub export fn epub_toc_entry_to_chapter(book: ?*const epub_book_t, toc_idx: u16, out_chapter_idx: ?*u16) callconv(.c) ra8_err_t {
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

pub export fn epub_get_metadata(book: ?*const epub_book_t, out_meta: ?*epub_metadata_t) callconv(.c) ra8_err_t {
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

pub export fn epub_get_cover_image(book: ?*epub_book_t, out_buf: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
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

pub export fn epub_get_resource(book: ?*epub_book_t, path: ?[*:0]const u8, out_buf: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
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

pub export fn epub_get_embedded_font_count(book: ?*const epub_book_t, out_count: ?*u16) callconv(.c) ra8_err_t {
    if ((book == null) or (out_count == null)) {
        return k_ra8_err_null_ptr;
    }
    if (book.?.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    out_count.?.* = book.?.embedded_font_count;
    return k_ra8_ok;
}

pub export fn epub_manifest_count(book: ?*const epub_book_t) callconv(.c) u16 {
    if (book == null) {
        return 0;
    }
    if (book.?.in_use == 0) {
        return 0;
    }
    return book.?.manifest_count;
}

pub export fn epub_manifest_item(book: ?*const epub_book_t, index: u16) callconv(.c) ?*const epub_manifest_item_t {
    if ((book == null) or (book.?.in_use == 0)) {
        return null;
    }
    if (index >= book.?.manifest_count) {
        return null;
    }
    return &book.?.manifest[index];
}

pub export fn epub_get_embedded_font(book: ?*epub_book_t, idx: u16, out_buf: ?[*]u8, max_len: usize, got_len: ?*usize) callconv(.c) ra8_err_t {
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

pub export fn epub_set_font(book: ?*epub_book_t, font_data: ?[*]const u8, font_size: usize) callconv(.c) ra8_err_t {
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

pub export fn epub_render_glyph(book: ?*epub_book_t, codepoint: i32, font_size: f32, out_bitmap: ?[*]u8, max_pixels: usize, out_w: ?*u32, out_h: ?*u32) callconv(.c) ra8_err_t {
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
