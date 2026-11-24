//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_open_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_open_abi");
const priv_epub_mem_read = sut.priv_epub_mem_read;
const priv_epub_stream_read = sut.priv_epub_stream_read;
const priv_epub_dirname = sut.priv_epub_dirname;
const priv_epub_finish_open = sut.priv_epub_finish_open;
const epub_open = sut.epub_open;
const epub_open_streamed = sut.epub_open_streamed;
const epub_close = sut.epub_close;

const epub_book_t = sut.epub_book_t;
const epub_mem_media_t = sut.epub_mem_media_t;
const epub_stream_media_t = sut.epub_stream_media_t;
const k_epub_toc_nav = sut.k_epub_toc_nav;
const k_epub_toc_ncx = sut.k_epub_toc_ncx;
const k_epub_toc_none = sut.k_epub_toc_none;
const k_epub_zip_archive_bytes = sut.k_epub_zip_archive_bytes;
const k_ra8_err_invalid_arg = sut.k_ra8_err_invalid_arg;
const k_ra8_err_not_initialized = sut.k_ra8_err_not_initialized;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_validation_failed = sut.k_ra8_err_validation_failed;
const k_ra8_ok = sut.k_ra8_ok;
const mz_zip_archive = sut.mz_zip_archive;
const ra8_err_t = sut.ra8_err_t;

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
