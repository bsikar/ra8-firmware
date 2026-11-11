//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the epub open/close core (RA8FW-1162): the C-era
//! src/epub_open.c ported verbatim behind the unchanged C headers. This TU
//! is the epub heart: the public epub_open / epub_open_streamed / epub_close
//! triple, the miniz IO and allocator adapters, the bounded
//! container/OPF/TOC extraction chain, and the archive-level
//! decompression-guard hook. miniz (SOUP), the decompression policy
//! (ra8_core through its C ABI), the XML shim seams (epub_xml_shim.c),
//! the miniz arena (epub_miniz_alloc.c) and the guard exports (the
//! ra8_epub archive) all stay extern, so this TU adds no policy of its
//! own. The TU ships as its own archive (ra8_epub_open) so a core-only
//! consumer never pulls its externs: archive members are pulled only when
//! something references the open/close entry points, which is what the C
//! TU's presence in the CMake glob expressed.
//!
//! Carried verbatim from the C TU: the static 4 KiB container scratch and
//! the static 48 KiB OPF/TOC scratch rely on single-threaded
//! init-context use; the epub parser is single-threaded init-context-only,
//! so the statics are safe. The book's zip_archive_storage is
//! k_epub_zip_archive_bytes (256) and must stay >= sizeof(mz_zip_archive)
//! (miniz 3.0.2 sizeof=112; checked at compile time below), and
//! mz_zip_archive alignment must not exceed max_align_t (the storage field
//! is 16-aligned; miniz needs 8).

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_no_mem: ra8_err_t = 0x102;
const k_ra8_err_invalid_arg: ra8_err_t = 0x103;
const k_ra8_err_not_found: ra8_err_t = 0x106;
const k_ra8_err_not_initialized: ra8_err_t = 0x10F;
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
const k_epub_container_xml_buf = 4096;
const k_epub_opf_xml_buf = 49152;
const k_epub_toc_none: u8 = 0;
const k_epub_toc_ncx: u8 = 1;
const k_epub_toc_nav: u8 = 2;

// ---- xml.h (apps/shared_libs/xml/inc/xml.h) --------------------------------
const xml_span_t = extern struct { offset: u32, length: u32 };
const xml_frame_t = extern struct { name_offset: u32, name_length: u16, consumer: u16 };
const k_xml_workspace_frames = 512;
const xml_workspace_t = extern struct { frames: [k_xml_workspace_frames]xml_frame_t };
const epub_xml_workspace_t = extern struct {
    reader: xml_workspace_t,
    references: [k_epub_max_chapters]xml_span_t,
    legacy_cover_id: xml_span_t,
    reference_count: u16,
};

// ---- epub_miniz_alloc.h (the arena TU stays C for a later slice) -----------
const k_epub_miniz_pool_bytes = 163840;
const epub_miniz_arena_t = extern struct { base: ?[*]u8, capacity: usize, initialized: u8 };
const epub_miniz_workspace_t = extern struct { bytes: [k_epub_miniz_pool_bytes]u8 align(16) };

// ---- media descriptors (epub.h) ---------------------------------------------
const epub_stream_read_fn = *const fn (?*anyopaque, u64, ?*anyopaque, u32, ?*u32) callconv(.c) ra8_err_t;
const epub_stream_media_t = extern struct { read: ?epub_stream_read_fn, ctx: ?*anyopaque, size: u64 };
const epub_mem_media_t = extern struct { data: ?[*]const u8, size: usize };
const epub_container_result_t = extern struct { opf_path: [k_epub_max_path_len]u8 };

const epub_toc_entry_t = extern struct { title: [k_epub_meta_len]u8, href: [k_epub_max_path_len]u8, depth: u8 };
const epub_manifest_item_t = extern struct { id: [k_epub_id_len]u8, href: [k_epub_max_path_len]u8, media_type: [k_epub_media_len]u8 };

// ---- the book (epub.h epub_book_t, exact C layout) --------------------------
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

// ---- miniz (SOUP, apps/shared_libs/third_party/miniz/miniz.h) ---------------
// The open TU writes the allocator and IO hooks before reader init and passes
// the handle through afterwards, so the full mz_zip_archive layout is here.
// mz_zip_mode / mz_zip_type / mz_zip_error are plain int-sized enums in the
// vendored miniz ABI.
const mz_alloc_fn = *const fn (?*anyopaque, usize, usize) callconv(.c) ?*anyopaque;
const mz_free_fn = *const fn (?*anyopaque, ?*anyopaque) callconv(.c) void;
const mz_realloc_fn = *const fn (?*anyopaque, ?*anyopaque, usize, usize) callconv(.c) ?*anyopaque;
const mz_file_read_fn = *const fn (?*anyopaque, u64, ?*anyopaque, usize) callconv(.c) usize;
const mz_file_write_fn = *const fn (?*anyopaque, u64, ?*const anyopaque, usize) callconv(.c) usize;
const mz_needs_keepalive_fn = *const fn (?*anyopaque, u64, ?*const anyopaque, usize) callconv(.c) c_int;

const mz_zip_archive = extern struct {
    m_archive_size: u64,
    m_central_directory_file_ofs: u64,
    m_total_files: c_uint,
    m_zip_mode: c_int,
    m_zip_type: c_int,
    m_last_error: c_int,
    m_file_offset_alignment: u64,
    m_pAlloc: ?mz_alloc_fn,
    m_pFree: ?mz_free_fn,
    m_pRealloc: ?mz_realloc_fn,
    m_pAlloc_opaque: ?*anyopaque,
    m_pRead: ?mz_file_read_fn,
    m_pWrite: ?mz_file_write_fn,
    m_pNeeds_keepalive: ?mz_needs_keepalive_fn,
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

comptime {
    if (k_epub_zip_archive_bytes < @sizeOf(mz_zip_archive))
        @compileError("k_epub_zip_archive_bytes too small for mz_zip_archive");
    if (@alignOf(mz_zip_archive) > @alignOf(epub_book_t))
        @compileError("mz_zip_archive alignment exceeds the book storage alignment");
}

// ---- miniz reader externs ----------------------------------------------------
extern fn mz_zip_reader_init_mem(pZip: *mz_zip_archive, pMem: *const anyopaque, size: usize, flags: c_uint) callconv(.c) c_int;
extern fn mz_zip_reader_init(pZip: *mz_zip_archive, size: u64, flags: c_uint) callconv(.c) c_int;
extern fn mz_zip_reader_end(pZip: *mz_zip_archive) callconv(.c) c_int;
extern fn mz_zip_reader_locate_file(pZip: *mz_zip_archive, name: [*]const u8, comment: ?[*]const u8, flags: c_uint) callconv(.c) c_int;
extern fn mz_zip_reader_file_stat(pZip: *mz_zip_archive, index: c_uint, pStat: *mz_zip_archive_file_stat) callconv(.c) c_int;
extern fn mz_zip_reader_extract_to_mem(pZip: *mz_zip_archive, index: c_uint, buf: [*]u8, len: usize, flags: c_uint) callconv(.c) c_int;

// ---- miniz arena (epub_miniz_alloc.c, stays C for now) ----------------------
extern fn epub_miniz_arena_init(arena: *epub_miniz_arena_t, workspace: *anyopaque, workspace_bytes: usize) callconv(.c) ra8_err_t;
extern fn epub_miniz_arena_deinit(arena: *epub_miniz_arena_t) callconv(.c) void;
extern fn epub_miniz_alloc(opaque_: ?*anyopaque, items: usize, size: usize) callconv(.c) ?*anyopaque;
extern fn epub_miniz_free(opaque_: ?*anyopaque, address: ?*anyopaque) callconv(.c) void;
extern fn epub_miniz_realloc(opaque_: ?*anyopaque, address: ?*anyopaque, items: usize, size: usize) callconv(.c) ?*anyopaque;

// ---- decompression policy (ra8_core through its C ABI) ---------------------
const ra8_decomp_read_fn = *const fn (?*anyopaque, u64, ?*anyopaque, usize) callconv(.c) usize;
extern fn ra8_decomp_zip_entry_preflight(read: ra8_decomp_read_fn, ctx: ?*anyopaque, archive_size: u64) callconv(.c) ra8_err_t;

// ---- XML shim seams (epub_xml_shim.c, stays C) ------------------------------
extern fn priv_epub_xml_parse_container(xml: [*]const u8, xml_len: usize, out: *epub_container_result_t, workspace: *epub_xml_workspace_t) callconv(.c) ra8_err_t;
extern fn priv_epub_xml_parse_opf(xml: [*]const u8, xml_len: usize, book: *epub_book_t) callconv(.c) ra8_err_t;
extern fn priv_epub_xml_parse_ncx(xml: [*]const u8, xml_len: usize, book: *epub_book_t) callconv(.c) ra8_err_t;
extern fn priv_epub_xml_parse_nav(xml: [*]const u8, xml_len: usize, book: *epub_book_t) callconv(.c) ra8_err_t;
extern fn priv_epub_join_path(dir: [*]const u8, name: [*]const u8, dst: [*]u8, cap: usize) callconv(.c) void;

// ---- guard exports (the ra8_epub archive, RA8FW-1160) ----------------------
extern fn priv_epub_zip_guard_archive(zip: *mz_zip_archive) callconv(.c) ra8_err_t;
extern fn priv_epub_zip_guard_entry(pStat: *const mz_zip_archive_file_stat) callconv(.c) ra8_err_t;

// Test-only knobs, defined by teststub/open_test_stubs.c.
extern var teststub_fail_preflight: c_int;
extern var teststub_fail_arena_init: c_int;
extern var teststub_fail_init: c_int;
extern var teststub_fail_extract: c_int;
extern var teststub_fail_guard_archive: c_int;
extern var teststub_locate_found: c_int;
extern var teststub_entry_uncomp: u64;
extern var teststub_parse_called: c_int;
extern var teststub_arena_deinit_called: c_int;
extern var teststub_zip_end_called: c_int;

// Static (file-scope) scratch keeps the firmware stack frames small -- the
// OPF blob can be tens of KiB and the container blob 4 KiB, either of which
// would otherwise blow the per-thread stack budget. Single-threaded
// init-context use makes the statics safe (carried verbatim from the C TU).
var s_container_buf: [k_epub_container_xml_buf]u8 = undefined;
var s_opf_buf: [k_epub_opf_xml_buf]u8 = undefined;

fn internal_byte_copy(dst: [*]u8, src: [*]const u8, n: usize) void {
    for (0..n) |i| {
        dst[i] = src[i];
    }
}

fn internal_byte_zero(dst: [*]u8, n: usize) void {
    for (0..n) |i| {
        dst[i] = 0;
    }
}

fn internal_zip_destroy(zip: *mz_zip_archive) void {
    _ = mz_zip_reader_end(zip);
}

export fn priv_epub_mem_read(ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: usize) callconv(.c) usize {
    const mem: ?*const epub_mem_media_t = @ptrCast(@alignCast(ctx));
    if (mem == null or buf == null or offset > @as(u64, mem.?.size) or
        @as(u64, len) > (@as(u64, mem.?.size) - offset))
    {
        return 0;
    }
    internal_byte_copy(@ptrCast(buf.?), mem.?.data.? + @as(usize, @intCast(offset)), len);
    return len;
}

export fn priv_epub_stream_read(opaque_arg: ?*anyopaque, file_ofs: u64, buf: ?*anyopaque, n: usize) callconv(.c) usize {
    const sm: ?*const epub_stream_media_t = @ptrCast(@alignCast(opaque_arg));
    if (sm == null or sm.?.read == null) {
        return 0;
    }
    if (file_ofs >= sm.?.size) {
        return 0;
    }
    const avail: u64 = sm.?.size - file_ofs;
    const want: u64 = if (@as(u64, n) > avail) avail else @as(u64, n);
    // miniz IO chunks are bounded well below 4 GiB.
    if (want > 0xFFFFFFFF) {
        return 0;
    }
    if (want == 0) {
        return 0;
    }
    var got: u32 = 0;
    // The reason a read stopped is dropped here because miniz's m_pRead has
    // nowhere to carry it. Every backing above still reports it, and the
    // per-entry path reads the backing directly and keeps it.
    if (sm.?.read.?(sm.?.ctx, file_ofs, buf, @intCast(want), &got) != k_ra8_ok) {
        return 0;
    }
    return got;
}

export fn priv_epub_dirname(path: ?[*]const u8, dst: ?[*]u8, cap: usize) callconv(.c) void {
    if (dst == null or cap == 0) {
        return;
    }
    const d = dst.?;
    d[0] = 0;
    if (path == null) {
        return;
    }
    var len: usize = 0;
    var index: usize = 0;
    while (path.?[index] != 0) : (index += 1) {
        if (path.?[index] == '/') {
            len = index + 1;
        }
    }
    if (len == 0) {
        return;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    internal_byte_copy(d, path.?, len);
    d[len] = 0;
}

fn internal_extract(zip: *mz_zip_archive, name: [*]const u8, buf: [*]u8, cap: usize, got: *usize) ra8_err_t {
    got.* = 0;
    const idx = mz_zip_reader_locate_file(zip, name, null, 0);
    if (idx < 0) {
        return k_ra8_err_not_found;
    }
    var st = std.mem.zeroes(mz_zip_archive_file_stat);
    if (mz_zip_reader_file_stat(zip, @intCast(idx), &st) == 0) {
        return k_ra8_err_validation_failed; // stat cannot fail: locate found it
    }
    const gerr = priv_epub_zip_guard_entry(&st);
    if (gerr != k_ra8_ok) {
        return gerr; // lying header / declared bomb: reject before inflation
    }
    if (@as(u64, st.m_uncomp_size) > @as(u64, cap)) {
        return k_ra8_err_no_mem;
    }
    if (mz_zip_reader_extract_to_mem(zip, @intCast(idx), buf, cap, 0) == 0) {
        return k_ra8_err_validation_failed;
    }
    got.* = @intCast(st.m_uncomp_size);
    return k_ra8_ok;
}

fn internal_load_toc(zip: *mz_zip_archive, book: *epub_book_t, scratch: [*]u8, cap: usize) void {
    if (book.toc_kind == k_epub_toc_none) {
        return;
    }
    if (book.toc_path[0] == 0) {
        return;
    }
    var full_path: [k_epub_max_path_len]u8 = undefined;
    priv_epub_join_path(&book.opf_dir, &book.toc_path, &full_path, full_path.len);
    var got: usize = 0;
    var err = internal_extract(zip, &full_path, scratch, cap, &got);
    if (err == k_ra8_err_not_found) {
        err = internal_extract(zip, &book.toc_path, scratch, cap, &got);
    }
    if (err != k_ra8_ok) {
        return;
    }
    if (book.toc_kind == k_epub_toc_nav) {
        _ = priv_epub_xml_parse_nav(scratch, got, book);
    } else {
        _ = priv_epub_xml_parse_ncx(scratch, got, book);
    }
}

fn internal_parse_archive(zip: *mz_zip_archive, out_book: *epub_book_t, opf_scratch: [*]u8, opf_cap: usize) ra8_err_t {
    const s_container_path = "META-INF/container.xml";
    var got: usize = 0;
    var err = internal_extract(zip, s_container_path, &s_container_buf, s_container_buf.len, &got);
    if (err != k_ra8_ok) {
        return err;
    }

    var cres = std.mem.zeroes(epub_container_result_t);
    err = priv_epub_xml_parse_container(&s_container_buf, got, &cres, &out_book.xml_workspace);
    if (err != k_ra8_ok) {
        return err;
    }

    var opf_got: usize = 0;
    err = internal_extract(zip, &cres.opf_path, opf_scratch, opf_cap, &opf_got);
    if (err != k_ra8_ok) {
        return err;
    }

    priv_epub_dirname(&cres.opf_path, &out_book.opf_dir, k_epub_max_path_len);
    err = priv_epub_xml_parse_opf(opf_scratch, opf_got, out_book);
    if (err != k_ra8_ok) {
        return err;
    }

    // Optional navigation document (NCX / nav.xhtml). Reuses opf_scratch now
    // that the OPF has been parsed into the book. Best-effort.
    internal_load_toc(zip, out_book, opf_scratch, opf_cap);
    return k_ra8_ok;
}

export fn priv_epub_set_miniz_alloc(zip: ?*mz_zip_archive, book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if (zip == null or book == null) {
        return k_ra8_err_null_ptr;
    }
    const err = epub_miniz_arena_init(&book.?.miniz_arena, &book.?.miniz_workspace.bytes, @sizeOf(epub_miniz_workspace_t));
    if (err != k_ra8_ok) {
        return err; // embedded workspace is exact and aligned
    }
    zip.?.m_pAlloc = epub_miniz_alloc;
    zip.?.m_pFree = epub_miniz_free;
    zip.?.m_pRealloc = epub_miniz_realloc;
    zip.?.m_pAlloc_opaque = @ptrCast(&book.?.miniz_arena);
    return k_ra8_ok;
}

export fn priv_epub_finish_open(zip: ?*mz_zip_archive, out_book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if (zip == null or out_book == null) {
        return k_ra8_err_null_ptr;
    }
    // Archive-level decompression-limits guard: an archive whose central
    // directory floods the policy entry cap dies before any entry work.
    const gerr = priv_epub_zip_guard_archive(zip.?);
    if (gerr != k_ra8_ok) {
        internal_zip_destroy(zip.?);
        return gerr;
    }
    const err = internal_parse_archive(zip.?, out_book.?, &s_opf_buf, s_opf_buf.len);
    if (err != k_ra8_ok) {
        internal_zip_destroy(zip.?);
        return err;
    }
    out_book.?.zip_archive_active = 1;
    out_book.?.in_use = 1;
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

export fn epub_open(media: ?*const anyopaque, path: ?[*:0]const u8, out_book: ?*epub_book_t) callconv(.c) ra8_err_t {
    _ = path;
    if (media == null or out_book == null) {
        return k_ra8_err_null_ptr;
    }
    const mem: *const epub_mem_media_t = @ptrCast(@alignCast(media.?));
    if (mem.data == null or mem.size == 0) {
        return k_ra8_err_invalid_arg;
    }

    var preflight_mem = mem.*;
    const cap_err = ra8_decomp_zip_entry_preflight(priv_epub_mem_read, &preflight_mem, preflight_mem.size);
    if (cap_err != k_ra8_ok) {
        return cap_err;
    }

    const book = out_book.?;
    book.* = std.mem.zeroes(epub_book_t);

    const zip: *mz_zip_archive = @ptrCast(&book.zip_archive_storage);
    const aerr = priv_epub_set_miniz_alloc(zip, book);
    if (aerr != k_ra8_ok) {
        book.* = std.mem.zeroes(epub_book_t);
        return aerr; // embedded workspace is exact and aligned
    }
    if (mz_zip_reader_init_mem(zip, @ptrCast(mem.data.?), mem.size, 0) == 0) {
        _ = epub_miniz_arena_deinit(&book.miniz_arena);
        book.* = std.mem.zeroes(epub_book_t);
        return k_ra8_err_validation_failed;
    }

    const err = priv_epub_finish_open(zip, book);
    if (err != k_ra8_ok) {
        _ = epub_miniz_arena_deinit(&book.miniz_arena);
        book.* = std.mem.zeroes(epub_book_t);
        return err;
    }
    book.zip_bytes = mem.data;
    book.zip_size = mem.size;
    return k_ra8_ok;
}

export fn epub_open_streamed(media: ?*const epub_stream_media_t, path: ?[*:0]const u8, out_book: ?*epub_book_t) callconv(.c) ra8_err_t {
    _ = path;
    if (media == null or out_book == null) {
        return k_ra8_err_null_ptr;
    }
    const m = media.?;
    if (m.read == null or m.size == 0) {
        return k_ra8_err_invalid_arg;
    }

    var preflight_stream = m.*;
    const cap_err = ra8_decomp_zip_entry_preflight(priv_epub_stream_read, &preflight_stream, m.size);
    if (cap_err != k_ra8_ok) {
        return cap_err;
    }

    const book = out_book.?;
    book.* = std.mem.zeroes(epub_book_t);
    book.stream_media = m.*;

    const zip: *mz_zip_archive = @ptrCast(&book.zip_archive_storage);
    const aerr = priv_epub_set_miniz_alloc(zip, book);
    if (aerr != k_ra8_ok) {
        book.* = std.mem.zeroes(epub_book_t);
        return aerr; // embedded workspace is exact and aligned
    }
    zip.m_pRead = priv_epub_stream_read;
    zip.m_pIO_opaque = @ptrCast(&book.stream_media);
    if (mz_zip_reader_init(zip, m.size, 0) == 0) {
        _ = epub_miniz_arena_deinit(&book.miniz_arena);
        book.* = std.mem.zeroes(epub_book_t);
        return k_ra8_err_validation_failed;
    }

    const err = priv_epub_finish_open(zip, book);
    if (err != k_ra8_ok) {
        _ = epub_miniz_arena_deinit(&book.miniz_arena);
        book.* = std.mem.zeroes(epub_book_t);
        return err;
    }
    book.zip_bytes = null;
    book.zip_size = @truncate(m.size);
    return k_ra8_ok;
}

export fn epub_close(book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if (book == null) {
        return k_ra8_err_null_ptr;
    }
    const b = book.?;
    if (b.in_use == 0) {
        return k_ra8_err_not_initialized;
    }
    if (b.zip_archive_active != 0) {
        const zip: *mz_zip_archive = @ptrCast(&b.zip_archive_storage);
        internal_zip_destroy(zip);
        b.zip_archive_active = 0;
    }
    _ = epub_miniz_arena_deinit(&b.miniz_arena);
    b.in_use = 0;
    b.chapter_count = 0;
    return k_ra8_ok;
}

// ---------------------------------------------------------------------------
// Tests.
// ---------------------------------------------------------------------------

const testing = std.testing;

var g_stream_media: epub_stream_media_t = undefined;
var g_stream_buf: [256]u8 = undefined;
var g_stream_len: u32 = 0;
var g_stream_err: ra8_err_t = 0;
var g_stream_calls: usize = 0;

fn test_stream_read(ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: u32, out_read: ?*u32) callconv(.c) ra8_err_t {
    _ = ctx;
    _ = offset;
    g_stream_calls += 1;
    g_stream_len = len;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        @as([*]u8, @ptrCast(buf.?))[i] = 0xAA;
    }
    out_read.?.* = len;
    return g_stream_err;
}

test "zip archive storage fits miniz" {
    try testing.expect(k_epub_zip_archive_bytes >= @sizeOf(mz_zip_archive));
    try testing.expectEqual(@as(usize, 112), @sizeOf(mz_zip_archive));
    try testing.expectEqual(@as(usize, 16), @alignOf(epub_book_t));
}

test "mem_read bounds" {
    var blob = [_]u8{ 1, 2, 3, 4, 5 };
    var media = epub_mem_media_t{ .data = &blob, .size = blob.len };
    var out: [8]u8 = undefined;
    try testing.expectEqual(@as(usize, 5), priv_epub_mem_read(&media, 0, &out, 5));
    try testing.expectEqual(@as(u8, 3), out[2]);
    // Request past the end: the C guard drops the whole read.
    try testing.expectEqual(@as(usize, 0), priv_epub_mem_read(&media, 3, &out, 5));
    // Exactly at the end with len 0 is a zero-byte read.
    try testing.expectEqual(@as(usize, 0), priv_epub_mem_read(&media, 5, &out, 0));
    // Offset past the end.
    try testing.expectEqual(@as(usize, 0), priv_epub_mem_read(&media, 6, &out, 1));
    try testing.expectEqual(@as(usize, 0), priv_epub_mem_read(null, 0, &out, 1));
    try testing.expectEqual(@as(usize, 0), priv_epub_mem_read(&media, 0, null, 1));
}

test "dirname" {
    var dst: [192]u8 = undefined;
    priv_epub_dirname("a/b/c.opf", &dst, dst.len);
    try testing.expectEqualStrings("a/b/", std.mem.sliceTo(&dst, 0));
    priv_epub_dirname("plain.opf", &dst, dst.len);
    try testing.expectEqualStrings("", std.mem.sliceTo(&dst, 0));
    priv_epub_dirname(null, &dst, dst.len);
    try testing.expectEqualStrings("", std.mem.sliceTo(&dst, 0));
    priv_epub_dirname("a/b/c.opf", null, 0);
    priv_epub_dirname("a/b/c.opf", &dst, 2); // cap clamp: keep one byte + NUL
    try testing.expectEqualStrings("a", std.mem.sliceTo(&dst, 0));
}

test "stream_read clamps" {
    g_stream_err = 0;
    g_stream_calls = 0;
    g_stream_media = .{ .read = test_stream_read, .ctx = null, .size = 100 };
    var out: [8]u8 = undefined;
    try testing.expectEqual(@as(usize, 5), priv_epub_stream_read(&g_stream_media, 95, &out, 10));
    try testing.expectEqual(@as(u32, 5), g_stream_len);
    try testing.expectEqual(@as(usize, 0), priv_epub_stream_read(&g_stream_media, 100, &out, 1));
    try testing.expectEqual(@as(usize, 0), priv_epub_stream_read(null, 0, &out, 1));
    var no_read = g_stream_media;
    no_read.read = null;
    try testing.expectEqual(@as(usize, 0), priv_epub_stream_read(&no_read, 0, &out, 1));
    g_stream_err = 0x5FD;
    try testing.expectEqual(@as(usize, 0), priv_epub_stream_read(&g_stream_media, 0, &out, 4));
}

test "open null and invalid guards" {
    var book = std.mem.zeroes(epub_book_t);
    var blob = [_]u8{ 1, 2, 3, 4 };
    var media = epub_mem_media_t{ .data = &blob, .size = blob.len };
    var nomem = epub_mem_media_t{ .data = null, .size = 4 };
    var empty = epub_mem_media_t{ .data = &blob, .size = 0 };
    try testing.expectEqual(k_ra8_err_null_ptr, epub_open(null, null, &book));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_open(&media, null, null));
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_open(&nomem, null, &book));
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_open(&empty, null, &book));
}

test "open preflight failure passes through" {
    var book = std.mem.zeroes(epub_book_t);
    var blob = [_]u8{ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    var media = epub_mem_media_t{ .data = &blob, .size = blob.len };
    teststub_fail_preflight = 0x5FE;
    defer teststub_fail_preflight = 0;
    try testing.expectEqual(@as(ra8_err_t, 0x5FE), epub_open(&media, null, &book));
    try testing.expectEqual(@as(u8, 0), book.in_use);
}

test "open arena failure zeroes the book" {
    var book = std.mem.zeroes(epub_book_t);
    var blob = [_]u8{ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    var media = epub_mem_media_t{ .data = &blob, .size = blob.len };
    teststub_fail_arena_init = 1;
    defer teststub_fail_arena_init = 0;
    try testing.expectEqual(@as(ra8_err_t, 0x5FF), epub_open(&media, null, &book));
    try testing.expectEqual(@as(u8, 0), book.in_use);
    try testing.expect(book.zip_bytes == null);
}

test "open init failure deinitializes the arena and zeroes the book" {
    var book = std.mem.zeroes(epub_book_t);
    var blob = [_]u8{ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    var media = epub_mem_media_t{ .data = &blob, .size = blob.len };
    teststub_fail_init = 1;
    defer teststub_fail_init = 0;
    teststub_arena_deinit_called = 0;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_open(&media, null, &book));
    try testing.expectEqual(@as(u8, 0), book.in_use);
    try testing.expectEqual(@as(u8, 0), book.zip_archive_active);
    try testing.expectEqual(@as(u8, 1), teststub_arena_deinit_called);
}

test "open happy path parses the archive chain" {
    var book = std.mem.zeroes(epub_book_t);
    var blob = [_]u8{ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    var media = epub_mem_media_t{ .data = &blob, .size = blob.len };
    teststub_locate_found = 1;
    teststub_entry_uncomp = 64;
    teststub_parse_called = 0;
    try testing.expectEqual(k_ra8_ok, epub_open(&media, null, &book));
    try testing.expectEqual(@as(u8, 1), book.zip_archive_active);
    try testing.expectEqual(@as(u8, 1), book.in_use);
    try testing.expect(book.zip_bytes != null);
    try testing.expectEqual(@as(usize, blob.len), book.zip_size);
    try testing.expectEqual(@as(c_int, 2), teststub_parse_called); // container then OPF
    try testing.expectEqualStrings("sub/", std.mem.sliceTo(&book.opf_dir, 0));

    // Close path.
    teststub_zip_end_called = 0;
    try testing.expectEqual(k_ra8_ok, epub_close(&book));
    try testing.expectEqual(@as(u8, 1), teststub_zip_end_called);
    try testing.expectEqual(@as(u8, 0), book.in_use);
    try testing.expectEqual(@as(u16, 0), book.chapter_count);
    try testing.expectEqual(k_ra8_err_not_initialized, epub_close(&book));
}

test "close guards" {
    try testing.expectEqual(k_ra8_err_null_ptr, epub_close(null));
    var book = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_err_not_initialized, epub_close(&book));
}

test "finish_open guard failure destroys the zip and reports" {
    var book = std.mem.zeroes(epub_book_t);
    const zip: *mz_zip_archive = @ptrCast(&book.zip_archive_storage);
    teststub_fail_guard_archive = 1;
    teststub_zip_end_called = 0;
    try testing.expectEqual(@as(ra8_err_t, 0x507), priv_epub_finish_open(zip, &book));
    try testing.expectEqual(@as(u8, 1), teststub_zip_end_called);
    try testing.expectEqual(@as(u8, 0), book.zip_archive_active);
    teststub_zip_end_called = 0;
    teststub_fail_guard_archive = 0;
    teststub_fail_extract = 1;
    defer teststub_fail_extract = 0;
    try testing.expectEqual(k_ra8_err_validation_failed, priv_epub_finish_open(zip, &book));
    try testing.expectEqual(@as(u8, 1), teststub_zip_end_called);
}

test "finish_open loads the nav TOC" {
    var book = std.mem.zeroes(epub_book_t);
    const zip: *mz_zip_archive = @ptrCast(&book.zip_archive_storage);
    book.toc_kind = k_epub_toc_nav;
    book.toc_path[0] = 'n';
    book.toc_path[1] = '.';
    book.toc_path[2] = 'x';
    book.toc_path[3] = 0;
    teststub_locate_found = 1;
    teststub_entry_uncomp = 32;
    teststub_parse_called = 0;
    try testing.expectEqual(k_ra8_ok, priv_epub_finish_open(zip, &book));
    try testing.expectEqual(@as(c_int, 4), teststub_parse_called); // container, opf, nav
    try testing.expectEqual(@as(u8, 1), book.in_use);
}

test "finish_open loads the ncx TOC" {
    var book = std.mem.zeroes(epub_book_t);
    const zip: *mz_zip_archive = @ptrCast(&book.zip_archive_storage);
    book.toc_kind = k_epub_toc_ncx;
    book.toc_path[0] = 't';
    book.toc_path[1] = 0;
    teststub_locate_found = 1;
    teststub_entry_uncomp = 32;
    teststub_parse_called = 0;
    try testing.expectEqual(k_ra8_ok, priv_epub_finish_open(zip, &book));
    try testing.expectEqual(@as(c_int, 3), teststub_parse_called); // container, opf, ncx
}

test "finish_open skips the TOC when none was parsed" {
    var book = std.mem.zeroes(epub_book_t);
    const zip: *mz_zip_archive = @ptrCast(&book.zip_archive_storage);
    book.toc_kind = k_epub_toc_none;
    teststub_locate_found = 1;
    teststub_entry_uncomp = 32;
    teststub_parse_called = 0;
    try testing.expectEqual(k_ra8_ok, priv_epub_finish_open(zip, &book));
    try testing.expectEqual(@as(c_int, 2), teststub_parse_called); // container, opf only
}

test "streamed open wires the IO hook" {
    var book = std.mem.zeroes(epub_book_t);
    g_stream_err = 0;
    g_stream_media = .{ .read = test_stream_read, .ctx = null, .size = 64 };
    var media = epub_stream_media_t{ .read = test_stream_read, .ctx = null, .size = 64 };
    teststub_locate_found = 1;
    teststub_entry_uncomp = 64;
    teststub_parse_called = 0;
    try testing.expectEqual(k_ra8_ok, epub_open_streamed(&media, null, &book));
    const zip: *mz_zip_archive = @ptrCast(&book.zip_archive_storage);
    try testing.expect(zip.m_pRead != null);
    try testing.expectEqual(@intFromPtr(&priv_epub_stream_read), @intFromPtr(zip.m_pRead.?));
    try testing.expect(zip.m_pIO_opaque != null);
    try testing.expectEqual(@as(u64, 64), book.zip_size);
    try testing.expect(book.zip_bytes == null);
    try testing.expectEqual(@as(u8, 1), book.in_use);
    _ = epub_close(&book);
}

test "streamed open guards" {
    var book = std.mem.zeroes(epub_book_t);
    var no_read = epub_stream_media_t{ .read = null, .ctx = null, .size = 8 };
    var empty = epub_stream_media_t{ .read = test_stream_read, .ctx = null, .size = 0 };
    try testing.expectEqual(k_ra8_err_null_ptr, epub_open_streamed(null, null, &book));
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_open_streamed(&no_read, null, &book));
    try testing.expectEqual(k_ra8_err_invalid_arg, epub_open_streamed(&empty, null, &book));
}
