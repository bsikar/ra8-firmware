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
pub const ra8_err_t = u16;
pub const k_ra8_ok: ra8_err_t = 0;
pub const k_ra8_err_invalid_size: ra8_err_t = 0x105;
pub const k_ra8_err_not_found: ra8_err_t = 0x106;
pub const k_ra8_err_not_supported: ra8_err_t = 0x107;
pub const k_ra8_err_not_initialized: ra8_err_t = 0x10F;
pub const k_ra8_err_validation_failed: ra8_err_t = 0x501;
pub const k_ra8_err_null_ptr: ra8_err_t = 0x504;

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
pub const k_epub_ldh_size = 30;
pub const k_epub_ldh_fname_len_ofs = 26;
pub const k_epub_ldh_extra_len_ofs = 28;
pub const k_epub_ldh_sig = [4]u8{ 0x50, 0x4B, 0x03, 0x04 };

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

pub export fn epub_entry_open(book: ?*epub_book_t, path: ?[*:0]const u8, out_reader: ?*epub_entry_reader_t, out_size: ?*u64) callconv(.c) ra8_err_t {
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

pub export fn epub_entry_read(reader: ?*epub_entry_reader_t, buf: ?[*]u8, cap: usize, got: ?*usize) callconv(.c) ra8_err_t {
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

pub export fn epub_entry_close(reader: ?*epub_entry_reader_t) callconv(.c) ra8_err_t {
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

pub export fn epub_entry_pread(book: ?*epub_book_t, path: ?[*:0]const u8, offset: u64, buf: ?[*]u8, len: usize, got: ?*usize) callconv(.c) ra8_err_t {
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

pub const k_reader_iter_sentinel: usize = 0x1000;
