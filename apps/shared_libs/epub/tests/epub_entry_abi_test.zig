//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_entry_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_entry_abi");
const epub_entry_open = sut.epub_entry_open;
const epub_entry_read = sut.epub_entry_read;
const epub_entry_close = sut.epub_entry_close;
const epub_entry_pread = sut.epub_entry_pread;

const epub_book_t = sut.epub_book_t;
const epub_entry_reader_t = sut.epub_entry_reader_t;
const k_epub_ldh_extra_len_ofs = sut.k_epub_ldh_extra_len_ofs;
const k_epub_ldh_fname_len_ofs = sut.k_epub_ldh_fname_len_ofs;
const k_epub_ldh_sig = sut.k_epub_ldh_sig;
const k_epub_ldh_size = sut.k_epub_ldh_size;
const k_ra8_err_invalid_size = sut.k_ra8_err_invalid_size;
const k_ra8_err_not_found = sut.k_ra8_err_not_found;
const k_ra8_err_not_initialized = sut.k_ra8_err_not_initialized;
const k_ra8_err_not_supported = sut.k_ra8_err_not_supported;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_validation_failed = sut.k_ra8_err_validation_failed;
const k_ra8_ok = sut.k_ra8_ok;
const k_reader_iter_sentinel = sut.k_reader_iter_sentinel;
const ra8_err_t = sut.ra8_err_t;

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
