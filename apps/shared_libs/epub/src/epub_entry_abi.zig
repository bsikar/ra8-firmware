//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig implementation of the bounded-RAM entry accessors (RA8FW-1165): the
//! C-era src/epub_entry.c ported verbatim behind the unchanged C headers.
//! The forward streaming cursor (epub_entry_open/read/close) over miniz's
//! extract iterator and the positioned read (epub_entry_pread) for stored
//! entries both live here; the whole TU ships as its own archive
//! (ra8_epub_entry) so a consumer that never streams or windows an entry
//! never pulls the miniz iterator externs. miniz stays extern (SOUP C); the
//! zip guard export comes from the ra8_epub archive and the priv_* helpers
//! (join_path, book_not_ready) from the chapter archive, so this archive
//! sits after ra8_epub_chapter in the link order. The remaining consumers,
//! epub_img_import_abi.zig (RA8FW-1193) and epub_img_tiles_abi.zig
//! (RA8FW-1194), reach the public surface through the unchanged
//! epub_entry.h.
//!
//! The mz_zip_archive_file_stat mirror here is the FULL C layout: 1112 bytes
//! with the 512-byte filename, the 512-byte comment and the trailing time
//! slot, probed against the vendored miniz (offsets 0/8/16/18/20/22/24/32/
//! 40/48/52/56/64/68/72/76/80/592/1104, alignment 8). A production link
//! reaches the real mz_zip_reader_file_stat, which writes the whole struct,
//! so the undersized mirrors the open and chapter TUs carried were widened
//! to this same full layout in this slice before anything called file_stat
//! with a short buffer.

const std = @import("std");

// ---- ra8_err_t (libs/ra8_core/inc/ra8_err.h, enum : uint16_t) --------------
const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_not_found: ra8_err_t = 0x106;
const k_ra8_err_not_supported: ra8_err_t = 0x107;
const k_ra8_err_not_initialized: ra8_err_t = 0x10F;
const k_ra8_err_validation_failed: ra8_err_t = 0x501;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

// ---- epub.h limits ---------------------------------------------------------
const k_epub_max_path_len = 192;
const k_epub_zip_archive_bytes = 256;

// ---- the book (epub.h epub_book_t, exact C layout; mirrors the open TU) ----
const epub_miniz_arena_t = extern struct { base: ?[*]u8, capacity: usize, initialized: u8 };
const epub_miniz_workspace_t = extern struct { bytes: [163840]u8 align(16) };
const xml_span_t = extern struct { offset: u32, length: u32 };
const xml_frame_t = extern struct { name_offset: u32, name_length: u16, consumer: u16 };
const xml_reader_workspace_t = extern struct { frames: [512]xml_frame_t };
const epub_xml_workspace_t = extern struct {
    reader: xml_reader_workspace_t,
    references: [64]xml_span_t,
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
    chapter_paths: [64][k_epub_max_path_len]u8,
    title: [128]u8,
    author: [128]u8,
    language: [128]u8,
    identifier: [128]u8,
    cover_path: [k_epub_max_path_len]u8,
    embedded_font_count: u16,
    embedded_font_paths: [8][k_epub_max_path_len]u8,
    manifest_count: u16,
    manifest: [96]epub_manifest_item_t,
    opf_dir: [k_epub_max_path_len]u8,
    toc_path: [k_epub_max_path_len]u8,
    toc_kind: u8,
    toc_count: u16,
    toc: [64]epub_toc_entry_t,
    font_data: ?[*]const u8,
    font_size: usize,
    in_use: u8,
};

const epub_manifest_item_t = extern struct { id: [64]u8, href: [k_epub_max_path_len]u8, media_type: [48]u8 };
const epub_toc_entry_t = extern struct { title: [128]u8, href: [k_epub_max_path_len]u8, depth: u8 };

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

// Full C layout (see the header comment): MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE is
// 512 and MZ_ZIP_MAX_ARCHIVE_FILE_COMMENT_SIZE is 512 in the vendored miniz.
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
    if (@sizeOf(usize) == 8) {
        if (@sizeOf(mz_zip_archive_file_stat) != 1112)
            @compileError("mz_zip_archive_file_stat mirror diverges from the C layout");
        if (@offsetOf(mz_zip_archive_file_stat, "m_local_header_ofs") != 56)
            @compileError("m_local_header_ofs offset diverges from the C layout");
        if (@offsetOf(mz_zip_archive_file_stat, "m_method") != 22)
            @compileError("m_method offset diverges from the C layout");
    }
    if (k_epub_zip_archive_bytes < @sizeOf(mz_zip_archive))
        @compileError("k_epub_zip_archive_bytes too small for mz_zip_archive");
}

// ---- the entry reader (epub_entry.h epub_entry_reader_t, exact C layout) ----
const k_epub_entry_reader_reserved_bytes = 7;

pub const epub_entry_reader_t = extern struct {
    iter: ?*mz_zip_reader_extract_iter_state,
    book: ?*epub_book_t,
    total: u64,
    consumed: u64,
    done: u8,
    reserved: [k_epub_entry_reader_reserved_bytes]u8,
};

comptime {
    // The C header's reader is pointer-shaped, so its size follows the target:
    // 40 B on the probed 64-bit host, 32 B under AAPCS (32-bit pointers, u64
    // still 8-aligned). The mirror tracks the C header on both.
    const k_reader_size: usize = if (@sizeOf(usize) == 8) 40 else 32;
    if (@sizeOf(epub_entry_reader_t) != k_reader_size)
        @compileError("epub_entry_reader_t mirror diverges from the C layout");
}

// ---- miniz reader + iterator externs ----------------------------------------
const mz_zip_reader_extract_iter_state = opaque {};

extern fn mz_zip_reader_locate_file(pZip: *mz_zip_archive, name: [*]const u8, comment: ?[*]const u8, flags: c_uint) callconv(.c) c_int;
extern fn mz_zip_reader_file_stat(pZip: *mz_zip_archive, index: c_uint, pStat: *mz_zip_archive_file_stat) callconv(.c) c_int;
extern fn mz_zip_reader_extract_iter_new(pZip: *mz_zip_archive, index: c_uint, flags: c_uint) callconv(.c) ?*mz_zip_reader_extract_iter_state;
extern fn mz_zip_reader_extract_iter_read(pState: *mz_zip_reader_extract_iter_state, buf: [*]u8, buf_size: usize) callconv(.c) usize;
extern fn mz_zip_reader_extract_iter_free(pState: *mz_zip_reader_extract_iter_state) callconv(.c) c_int;

// ---- sibling-archive helpers --------------------------------------------------
extern fn priv_epub_zip_guard_entry(pStat: *const mz_zip_archive_file_stat) callconv(.c) ra8_err_t;
extern fn priv_epub_join_path(dir: ?[*]const u8, name: ?[*]const u8, dst: ?[*]u8, cap: usize) callconv(.c) void;
extern fn priv_epub_book_not_ready(in_use: u8, zip_archive_active: u8) callconv(.c) c_int;

// ---- ZIP local-file-header layout (PKWARE APPNOTE.TXT s4.3.7) ----------------
const k_epub_ldh_size = 30;
const k_epub_ldh_fname_len_ofs = 26;
const k_epub_ldh_extra_len_ofs = 28;
const k_epub_ldh_sig = [4]u8{ 0x50, 0x4B, 0x03, 0x04 };

inline fn internal_zip(book: *epub_book_t) *mz_zip_archive {
    return @ptrCast(&book.zip_archive_storage);
}

fn internal_locate(zip: *mz_zip_archive, book: *epub_book_t, path: [*:0]const u8, out_idx: *i32) ra8_err_t {
    var full_path: [k_epub_max_path_len]u8 = undefined;
    priv_epub_join_path(&book.opf_dir, path, &full_path, full_path.len);
    var idx: i32 = mz_zip_reader_locate_file(zip, &full_path, null, 0);
    if (idx < 0) {
        idx = mz_zip_reader_locate_file(zip, path, null, 0);
    }
    if (idx < 0) {
        return k_ra8_err_not_found;
    }
    out_idx.* = idx;
    return k_ra8_ok;
}

fn internal_backing_read(book: *epub_book_t, archive_ofs: u64, buf: [*]u8, n: usize) bool {
    if (book.zip_bytes) |zip_bytes| {
        if (archive_ofs +% n > @as(u64, book.zip_size)) {
            return false; // offsets come from a validated central-directory entry
        }
        const ofs: usize = @intCast(archive_ofs); // bounded by the zip_size check above
        @memcpy(buf[0..n], zip_bytes[ofs..][0..n]);
        return true;
    }
    const read = book.stream_media.read orelse return false;
    if (n > std.math.maxInt(u32)) {
        return false; // entry windows are bounded well below 4 GiB
    }
    var got: u32 = 0;
    const err = read(book.stream_media.ctx, archive_ofs, @as(?*anyopaque, @ptrCast(buf)), @intCast(n), &got);
    // A failed backing and a short archive are both "no window" here, but they
    // are now distinguishable one layer down, which is the point of the vmem
    // error fix: the caller that compiles a book sees the error instead of a
    // clean truncation.
    return err == k_ra8_ok and got == n;
}

fn internal_data_offset(hdr: *const [k_epub_ldh_size]u8, local_header_ofs: u64, out_data_ofs: *u64) bool {
    if (!std.mem.eql(u8, hdr[0..4], &k_epub_ldh_sig)) {
        return false;
    }
    const fname: u32 = @as(u32, hdr[k_epub_ldh_fname_len_ofs]) | (@as(u32, hdr[k_epub_ldh_fname_len_ofs + 1]) << 8);
    const extra: u32 = @as(u32, hdr[k_epub_ldh_extra_len_ofs]) | (@as(u32, hdr[k_epub_ldh_extra_len_ofs + 1]) << 8);
    out_data_ofs.* = local_header_ofs +% k_epub_ldh_size +% fname +% extra;
    return true;
}

fn internal_open_iter(book: *epub_book_t, path: [*:0]const u8, out_iter: *?*mz_zip_reader_extract_iter_state, out_uncomp: *u64) ra8_err_t {
    const zip = internal_zip(book);
    var idx: i32 = 0;
    const err = internal_locate(zip, book, path, &idx);
    if (err != k_ra8_ok) {
        return err;
    }
    var st: mz_zip_archive_file_stat = undefined;
    if (mz_zip_reader_file_stat(zip, @intCast(idx), &st) == 0) {
        return k_ra8_err_validation_failed; // stat cannot fail on a located entry
    }
    const gerr = priv_epub_zip_guard_entry(&st);
    if (gerr != k_ra8_ok) {
        return gerr; // lying header / declared bomb: reject before inflation
    }
    const iter = mz_zip_reader_extract_iter_new(zip, @intCast(idx), 0) orelse return k_ra8_err_validation_failed;
    out_iter.* = iter;
    out_uncomp.* = st.m_uncomp_size;
    return k_ra8_ok;
}

fn internal_stored_data_offset(book: *epub_book_t, path: [*:0]const u8, out_data_ofs: *u64, out_uncomp: *u64) ra8_err_t {
    const zip = internal_zip(book);
    var idx: i32 = 0;
    const err = internal_locate(zip, book, path, &idx);
    if (err != k_ra8_ok) {
        return err;
    }
    var st: mz_zip_archive_file_stat = undefined;
    if (mz_zip_reader_file_stat(zip, @intCast(idx), &st) == 0) {
        return k_ra8_err_validation_failed; // stat cannot fail on a located entry
    }
    const gerr = priv_epub_zip_guard_entry(&st);
    if (gerr != k_ra8_ok) {
        return gerr; // lying header / declared bomb: reject before the copy
    }
    if (st.m_method != 0) {
        return k_ra8_err_not_supported; // DEFLATE -> use the forward cursor
    }
    var hdr: [k_epub_ldh_size]u8 = @splat(0);
    if (!internal_backing_read(book, st.m_local_header_ofs, &hdr, hdr.len)) {
        return k_ra8_err_validation_failed; // header bytes are within the archive
    }
    if (!internal_data_offset(&hdr, st.m_local_header_ofs, out_data_ofs)) {
        return k_ra8_err_validation_failed;
    }
    out_uncomp.* = st.m_uncomp_size;
    return k_ra8_ok;
}

// ---- public API ---------------------------------------------------------------

export fn epub_entry_open(book: ?*epub_book_t, path: ?[*:0]const u8, out_reader: ?*epub_entry_reader_t, out_size: ?*u64) callconv(.c) ra8_err_t {
    if (book == null) return k_ra8_err_null_ptr;
    if (path == null) return k_ra8_err_null_ptr;
    if (out_reader == null) return k_ra8_err_null_ptr;
    out_reader.?.* = std.mem.zeroes(epub_entry_reader_t);
    if (priv_epub_book_not_ready(book.?.in_use, book.?.zip_archive_active) != 0) {
        return k_ra8_err_not_initialized;
    }
    var iter: ?*mz_zip_reader_extract_iter_state = null;
    var uncomp: u64 = 0;
    const err = internal_open_iter(book.?, path.?, &iter, &uncomp);
    if (err != k_ra8_ok) {
        return err;
    }
    out_reader.?.iter = iter;
    out_reader.?.book = book;
    out_reader.?.total = uncomp;
    out_reader.?.done = if (uncomp == 0) 1 else 0;
    if (out_size) |size_out| {
        size_out.* = uncomp;
    }
    return k_ra8_ok;
}

export fn epub_entry_read(reader: ?*epub_entry_reader_t, buf: ?[*]u8, cap: usize, got: ?*usize) callconv(.c) ra8_err_t {
    if (reader == null) return k_ra8_err_null_ptr;
    if (buf == null) return k_ra8_err_null_ptr;
    if (got == null) return k_ra8_err_null_ptr;
    got.?.* = 0;
    if (reader.?.iter == null) {
        return k_ra8_err_not_initialized;
    }
    if (cap == 0) {
        return k_ra8_err_invalid_size;
    }
    if (reader.?.done != 0) {
        return k_ra8_ok; // clean EOF already reported; *got stays 0
    }
    const n = mz_zip_reader_extract_iter_read(reader.?.iter.?, buf.?, cap);
    reader.?.consumed +%= n;
    got.?.* = n;
    if (n < cap) {
        // A short read is end-of-entry only if every byte arrived; otherwise the
        // compressed stream ended early / failed to inflate.
        if (reader.?.consumed >= reader.?.total) {
            reader.?.done = 1;
            return k_ra8_ok;
        }
        return k_ra8_err_validation_failed;
    }
    return k_ra8_ok;
}

export fn epub_entry_close(reader: ?*epub_entry_reader_t) callconv(.c) ra8_err_t {
    if (reader == null) return k_ra8_err_null_ptr;
    if (reader.?.iter == null) {
        return k_ra8_ok; // idempotent: already closed / never opened
    }
    var full = false;
    if (reader.?.total > 0) {
        if (reader.?.consumed >= reader.?.total) {
            full = true;
        }
    }
    const ok = mz_zip_reader_extract_iter_free(reader.?.iter.?);
    reader.?.iter = null;
    if (full) {
        if (ok == 0) {
            return k_ra8_err_validation_failed;
        }
    }
    return k_ra8_ok;
}

fn internal_pread_null_ok(book: ?*const epub_book_t, path: ?[*:0]const u8, buf: ?[*]u8, got: ?*const usize) ra8_err_t {
    if (book == null) return k_ra8_err_null_ptr;
    if (path == null) return k_ra8_err_null_ptr;
    if (buf == null) return k_ra8_err_null_ptr;
    if (got == null) return k_ra8_err_null_ptr;
    return k_ra8_ok;
}

export fn epub_entry_pread(book: ?*epub_book_t, path: ?[*:0]const u8, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) ra8_err_t {
    const nz = internal_pread_null_ok(book, path, buf, got);
    if (nz != k_ra8_ok) {
        return nz;
    }
    got.?.* = 0;
    if (priv_epub_book_not_ready(book.?.in_use, book.?.zip_archive_active) != 0) {
        return k_ra8_err_not_initialized;
    }
    var data_ofs: u64 = 0;
    var uncomp: u64 = 0;
    const err = internal_stored_data_offset(book.?, path.?, &data_ofs, &uncomp);
    if (err != k_ra8_ok) {
        return err;
    }
    if (offset >= uncomp) {
        return k_ra8_ok; // window starts at/after EOF; *got stays 0
    }
    const avail = uncomp - offset;
    const n: usize = if (len > avail) @intCast(avail) else len;
    if (!internal_backing_read(book.?, data_ofs +% offset, buf.?, n)) {
        return k_ra8_err_validation_failed; // data window is within the archive
    }
    got.?.* = n;
    return k_ra8_ok;
}

// ---- tests ---------------------------------------------------------------------

const testing = std.testing;

extern var teststub_locate_idx: c_int;
extern var teststub_miss_prefixed: c_int;
extern var teststub_locate_calls: c_int;
extern var teststub_fail_stat: c_int;
extern var teststub_stat_method: c_ushort;
extern var teststub_stat_uncomp: u64;
extern var teststub_stat_lho: u64;
extern var teststub_guard_result: c_int;
extern var teststub_not_ready: c_int;
extern var teststub_fail_iter: c_int;
extern var teststub_iter_remaining: usize;
extern var teststub_iter_fill: u8;
extern var teststub_iter_free_result: c_int;

fn resetStubs() void {
    teststub_locate_idx = 0;
    teststub_miss_prefixed = 0;
    teststub_locate_calls = 0;
    teststub_fail_stat = 0;
    teststub_stat_method = 0;
    teststub_stat_uncomp = 0;
    teststub_stat_lho = 0;
    teststub_guard_result = 0;
    teststub_not_ready = 0;
    teststub_fail_iter = 0;
    teststub_iter_remaining = 0;
    teststub_iter_fill = 0xA5;
    teststub_iter_free_result = 1;
}

const k_reader_iter_sentinel: usize = 0x1000;

fn fakeOpenReader(book: *epub_book_t, uncomp: u64) epub_entry_reader_t {
    var reader: epub_entry_reader_t = std.mem.zeroes(epub_entry_reader_t);
    reader.iter = @ptrFromInt(k_reader_iter_sentinel);
    reader.book = book;
    reader.total = uncomp;
    reader.consumed = 0;
    reader.done = if (uncomp == 0) 1 else 0;
    return reader;
}

test "entry open null guards" {
    resetStubs();
    var reader: epub_entry_reader_t = undefined;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_open(null, "x", &reader, null));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_open(&book, null, &reader, null));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_open(&book, "x", null, null));
}

test "entry open refuses an unopened book" {
    resetStubs();
    teststub_not_ready = 1;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_entry_open(&book, "a.css", &reader, null));
}

test "entry open locate miss on both the prefixed and bare paths" {
    resetStubs();
    teststub_locate_idx = -1;
    teststub_miss_prefixed = 1;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(k_ra8_err_not_found, epub_entry_open(&book, "missing.css", &reader, null));
    try testing.expectEqual(@as(c_int, 2), teststub_locate_calls);
}

test "entry open stat failure" {
    resetStubs();
    teststub_fail_stat = 1;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_entry_open(&book, "a.css", &reader, null));
}

test "entry open guard reject passes the guard code through" {
    resetStubs();
    teststub_guard_result = 0x507;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(@as(ra8_err_t, 0x507), epub_entry_open(&book, "a.css", &reader, null));
}

test "entry open iterator failure" {
    resetStubs();
    teststub_fail_iter = 1;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_entry_open(&book, "a.css", &reader, null));
}

test "entry open success fills the reader and reports the size" {
    resetStubs();
    teststub_stat_uncomp = 100;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    var size: u64 = 999;
    try testing.expectEqual(k_ra8_ok, epub_entry_open(&book, "a.css", &reader, &size));
    try testing.expect(reader.iter != null);
    try testing.expectEqual(@as(u64, 100), reader.total);
    try testing.expectEqual(@as(u64, 0), reader.consumed);
    try testing.expectEqual(@as(u8, 0), reader.done);
    try testing.expectEqual(@as(u64, 100), size);
    // A zero-length entry reports done immediately.
    teststub_stat_uncomp = 0;
    try testing.expectEqual(k_ra8_ok, epub_entry_open(&book, "empty.css", &reader, &size));
    try testing.expectEqual(@as(u8, 1), reader.done);
    try testing.expectEqual(@as(u64, 0), size);
}

test "entry read null guards" {
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    var reader: epub_entry_reader_t = std.mem.zeroes(epub_entry_reader_t);
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_read(null, &buf, 4, &got));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_read(&reader, null, 4, &got));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_read(&reader, &buf, 4, null));
}

test "entry read refuses a never-opened reader" {
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    var reader: epub_entry_reader_t = std.mem.zeroes(epub_entry_reader_t);
    try testing.expectEqual(k_ra8_err_not_initialized, epub_entry_read(&reader, &buf, 4, &got));
    try testing.expectEqual(@as(usize, 0), got);
}

test "entry read refuses a zero capacity" {
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    var reader: epub_entry_reader_t = std.mem.zeroes(epub_entry_reader_t);
    reader.iter = @ptrFromInt(k_reader_iter_sentinel);
    try testing.expectEqual(k_ra8_err_invalid_size, epub_entry_read(&reader, &buf, 0, &got));
}

test "entry read after the done flag is a clean EOF" {
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    var reader: epub_entry_reader_t = std.mem.zeroes(epub_entry_reader_t);
    reader.iter = @ptrFromInt(k_reader_iter_sentinel);
    reader.done = 1;
    try testing.expectEqual(k_ra8_ok, epub_entry_read(&reader, &buf, 4, &got));
    try testing.expectEqual(@as(usize, 0), got);
}

test "entry read full chunk consumes exactly the capacity" {
    resetStubs();
    teststub_stat_uncomp = 8;
    teststub_iter_remaining = 8;
    teststub_iter_fill = 0x5A;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(k_ra8_ok, epub_entry_open(&book, "a.css", &reader, null));
    var buf: [8]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_ok, epub_entry_read(&reader, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 8), got);
    for (buf) |byte| try testing.expectEqual(@as(u8, 0x5A), byte);
    try testing.expectEqual(@as(u64, 8), reader.consumed);
    try testing.expectEqual(@as(u8, 0), reader.done);
}

test "entry read short at EOF marks the reader done" {
    resetStubs();
    teststub_stat_uncomp = 50;
    teststub_iter_remaining = 50;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(k_ra8_ok, epub_entry_open(&book, "a.css", &reader, null));
    var buf: [100]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_ok, epub_entry_read(&reader, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 50), got);
    try testing.expectEqual(@as(u8, 1), reader.done);
    // A follow-up read reports the same clean EOF with nothing delivered.
    got = 9;
    try testing.expectEqual(k_ra8_ok, epub_entry_read(&reader, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 0), got);
}

test "entry read short before EOF is a failed stream" {
    resetStubs();
    teststub_stat_uncomp = 200;
    teststub_iter_remaining = 50;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var reader: epub_entry_reader_t = undefined;
    try testing.expectEqual(k_ra8_ok, epub_entry_open(&book, "a.css", &reader, null));
    var buf: [100]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_entry_read(&reader, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 50), got);
    try testing.expectEqual(@as(u8, 0), reader.done);
}

test "entry close null guard and idempotent close" {
    var reader: epub_entry_reader_t = std.mem.zeroes(epub_entry_reader_t);
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_close(null));
    try testing.expectEqual(k_ra8_ok, epub_entry_close(&reader));
}

test "entry close after a full read propagates the iterator result" {
    resetStubs();
    var reader = fakeOpenReader(undefined, 8);
    reader.consumed = 8;
    teststub_iter_free_result = 1;
    try testing.expectEqual(k_ra8_ok, epub_entry_close(&reader));
    try testing.expect(reader.iter == null);
    reader.iter = @ptrFromInt(k_reader_iter_sentinel);
    teststub_iter_free_result = 0;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_entry_close(&reader));
}

test "entry close before a full read ignores the iterator result" {
    resetStubs();
    var reader = fakeOpenReader(undefined, 8);
    reader.consumed = 3;
    teststub_iter_free_result = 0;
    try testing.expectEqual(k_ra8_ok, epub_entry_close(&reader));
}

test "entry close with a zero total ignores the iterator result" {
    resetStubs();
    var reader = fakeOpenReader(undefined, 0);
    teststub_iter_free_result = 0;
    try testing.expectEqual(k_ra8_ok, epub_entry_close(&reader));
}

test "entry pread null guards" {
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_pread(null, "x", 0, &buf, 4, &got));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_pread(&book, null, 0, &buf, 4, &got));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_pread(&book, "x", 0, null, 4, &got));
    try testing.expectEqual(k_ra8_err_null_ptr, epub_entry_pread(&book, "x", 0, &buf, 4, null));
}

test "entry pread refuses an unopened book" {
    resetStubs();
    teststub_not_ready = 1;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_err_not_initialized, epub_entry_pread(&book, "a.css", 0, &buf, 4, &got));
}

test "entry pread rejects a DEFLATE entry" {
    resetStubs();
    teststub_stat_method = 8;
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_err_not_supported, epub_entry_pread(&book, "a.css", 0, &buf, 4, &got));
}

test "entry pread at or after EOF delivers nothing" {
    resetStubs();
    teststub_stat_uncomp = stored_blob_uncomp;
    teststub_stat_lho = stored_blob_lho;
    const blob = storedBlob();
    var book = storedBook(&blob);
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_ok, epub_entry_pread(&book, "a.tile", stored_blob_uncomp, &buf, 4, &got));
    try testing.expectEqual(@as(usize, 0), got);
    try testing.expectEqual(k_ra8_ok, epub_entry_pread(&book, "a.tile", stored_blob_uncomp + 1, &buf, 4, &got));
    try testing.expectEqual(@as(usize, 0), got);
}

const stored_blob_lho: u64 = 16;
const stored_blob_fname_len: u64 = 8;
const stored_blob_extra_len: u64 = 4;
const stored_blob_data_ofs: u64 = stored_blob_lho + k_epub_ldh_size + stored_blob_fname_len + stored_blob_extra_len;
const stored_blob_uncomp: u64 = 8;

fn storedBlob() [stored_blob_data_ofs + stored_blob_uncomp]u8 {
    var blob = @as([stored_blob_data_ofs + stored_blob_uncomp]u8, @splat(0));
    const lho: usize = @intCast(stored_blob_lho);
    @memcpy(blob[lho..][0..4], &k_epub_ldh_sig);
    blob[lho + k_epub_ldh_fname_len_ofs] = @intCast(stored_blob_fname_len);
    blob[lho + k_epub_ldh_fname_len_ofs + 1] = @intCast(stored_blob_fname_len >> 8);
    blob[lho + k_epub_ldh_extra_len_ofs] = @intCast(stored_blob_extra_len);
    blob[lho + k_epub_ldh_extra_len_ofs + 1] = @intCast(stored_blob_extra_len >> 8);
    blob[@intCast(stored_blob_data_ofs)..][0..@intCast(stored_blob_uncomp)].* = @splat(0xC3);
    return blob;
}

fn storedBook(blob: []const u8) epub_book_t {
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    book.zip_bytes = blob.ptr;
    book.zip_size = blob.len;
    return book;
}

test "entry pread resident window reads the stored bytes" {
    resetStubs();
    teststub_stat_uncomp = stored_blob_uncomp;
    teststub_stat_lho = stored_blob_lho;
    const blob = storedBlob();
    var book = storedBook(&blob);
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_ok, epub_entry_pread(&book, "a.tile", 0, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 4), got);
    for (buf) |byte| try testing.expectEqual(@as(u8, 0xC3), byte);
    // A window past the stored payload clamps to the uncompressed size.
    got = 9;
    try testing.expectEqual(k_ra8_ok, epub_entry_pread(&book, "a.tile", 6, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 2), got);
    try testing.expectEqual(@as(u8, 0xC3), buf[0]);
    try testing.expectEqual(@as(u8, 0xC3), buf[1]);
}

test "entry pread resident window past the blob fails validation" {
    resetStubs();
    teststub_stat_uncomp = 100; // the stat lies: the blob only holds 8 payload bytes
    teststub_stat_lho = stored_blob_lho;
    const blob = storedBlob();
    var book = storedBook(&blob);
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_err_validation_failed, epub_entry_pread(&book, "a.tile", 60, &buf, buf.len, &got));
}

test "entry pread streamed window reads through the media callback" {
    resetStubs();
    teststub_stat_uncomp = stored_blob_uncomp;
    teststub_stat_lho = stored_blob_lho;
    const blob = storedBlob();
    var book: epub_book_t = std.mem.zeroes(epub_book_t);
    book.in_use = 1;
    book.zip_archive_active = 1;
    book.stream_media.read = streamRead;
    book.stream_media.ctx = @ptrCast(@constCast(&blob));
    book.stream_media.size = blob.len;
    var buf: [4]u8 = undefined;
    var got: usize = 9;
    try testing.expectEqual(k_ra8_ok, epub_entry_pread(&book, "a.tile", 2, &buf, buf.len, &got));
    try testing.expectEqual(@as(usize, 4), got);
    for (buf) |byte| try testing.expectEqual(@as(u8, 0xC3), byte);
}

var stream_read_seen_ofs: u64 = 0;

fn streamRead(ctx: ?*anyopaque, ofs: u64, buf: ?*anyopaque, len: u32, got: ?*u32) callconv(.c) ra8_err_t {
    const blob: [*]const u8 = @ptrCast(ctx orelse return 0x501);
    stream_read_seen_ofs = ofs;
    const dst: [*]u8 = @ptrCast(buf orelse return 0x501);
    const sink: ?*u32 = got;
    @memcpy(dst[0..len], blob[ofs..][0..len]);
    if (sink) |g| g.* = len;
    return k_ra8_ok;
}
