//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for `epub_fs_abi.zig`.

const std = @import("std");
const testing = std.testing;
const sut = @import("epub_fs_abi");

const epub_close_streamed_fs = sut.epub_close_streamed_fs;
const epub_open_streamed_fs = sut.epub_open_streamed_fs;
const epub_stream_fs_ctx_t = sut.epub_stream_fs_ctx_t;
const epub_stream_read_fn = sut.epub_stream_read_fn;
const k_ra8_err_null_ptr = sut.k_ra8_err_null_ptr;
const k_ra8_err_out_of_range = sut.k_ra8_err_out_of_range;
const k_ra8_ok = sut.k_ra8_ok;
const priv_epub_fs_stream_read = sut.priv_epub_fs_stream_read;
const ra8_err_t = sut.ra8_err_t;
const ra8_fs_file_t = sut.ra8_fs_file_t;

extern var fs_open_err: c_int;

extern var fs_seek_err: c_int;

extern var fs_read_err: c_int;

extern var fs_read_got: c_uint;

extern var fs_size_value: u64;

extern var fs_open_calls: c_int;

extern var fs_close_calls: c_int;

extern var fs_streamed_err: c_int;

extern var fs_close_err: c_int;

extern var epub_close_calls: c_int;

extern var fs_streamed_size_seen: u64;

extern var fs_streamed_ctx_seen: ?*anyopaque;

extern var fs_streamed_read_seen: ?*anyopaque;

extern var fs_stub_file_ptr: ?*anyopaque;

extern var fs_stub_book_ptr: ?*anyopaque;

fn reset() void {
    fs_open_err = 0;
    fs_seek_err = 0;
    fs_read_err = 0;
    fs_read_got = 0;
    fs_size_value = 100;
    fs_open_calls = 0;
    fs_close_calls = 0;
    fs_streamed_err = 0;
    fs_close_err = 0;
    epub_close_calls = 0;
    fs_streamed_size_seen = 0;
    fs_streamed_ctx_seen = null;
    fs_streamed_read_seen = null;
}

test "stream_read: null out_read is null_ptr" {
    reset();
    var io = epub_stream_fs_ctx_t{ .file = null };
    const out: c_uint = 99;
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_fs_stream_read(&io, 0, @as(?*anyopaque, @ptrCast(&io)), 4, null));
    try std.testing.expectEqual(@as(c_uint, 99), out);
}

test "stream_read: null ctx, null file, null buf are null_ptr with out_read cleared" {
    reset();
    var out: c_uint = 5;
    // null ctx
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_fs_stream_read(null, 0, @as(?*anyopaque, @ptrCast(&out)), 4, &out));
    try std.testing.expectEqual(@as(c_uint, 0), out);
    // null file
    var io = epub_stream_fs_ctx_t{ .file = null };
    out = 5;
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_fs_stream_read(&io, 0, @as(?*anyopaque, @ptrCast(&out)), 4, &out));
    try std.testing.expectEqual(@as(c_uint, 0), out);
    // null buf
    var file_mark: u8 = 0;
    io.file = @ptrCast(&file_mark);
    try std.testing.expectEqual(k_ra8_err_null_ptr, priv_epub_fs_stream_read(&io, 0, null, 4, &out));
    try std.testing.expectEqual(@as(c_uint, 0), out);
}

test "stream_read: offset above 4 GiB is out_of_range" {
    reset();
    var file_mark: u8 = 0;
    var io = epub_stream_fs_ctx_t{ .file = @ptrCast(&file_mark) };
    var out: c_uint = 5;
    try std.testing.expectEqual(k_ra8_err_out_of_range, priv_epub_fs_stream_read(&io, 0x1_0000_0000, @as(?*anyopaque, @ptrCast(&out)), 4, &out));
    try std.testing.expectEqual(@as(c_uint, 0), out);
}

test "stream_read: seek and read errors pass through" {
    reset();
    var file_mark: u8 = 0;
    var io = epub_stream_fs_ctx_t{ .file = @ptrCast(&file_mark) };
    var out: c_uint = 5;
    fs_seek_err = 0x301;
    try std.testing.expectEqual(@as(ra8_err_t, 0x301), priv_epub_fs_stream_read(&io, 0, @as(?*anyopaque, @ptrCast(&out)), 4, &out));
    try std.testing.expectEqual(@as(c_uint, 0), out);
    fs_seek_err = 0;
    fs_read_err = 0x302;
    try std.testing.expectEqual(@as(ra8_err_t, 0x302), priv_epub_fs_stream_read(&io, 0, @as(?*anyopaque, @ptrCast(&out)), 4, &out));
    try std.testing.expectEqual(@as(c_uint, 0), out);
}

test "stream_read: happy path copies the read count" {
    reset();
    var file_mark: u8 = 0;
    var io = epub_stream_fs_ctx_t{ .file = @ptrCast(&file_mark) };
    var out: c_uint = 5;
    fs_read_got = 7;
    try std.testing.expectEqual(k_ra8_ok, priv_epub_fs_stream_read(&io, 0, @as(?*anyopaque, @ptrCast(&out)), 4, &out));
    try std.testing.expectEqual(@as(c_uint, 7), out);
}

test "open_streamed_fs: null guards" {
    reset();
    var io = epub_stream_fs_ctx_t{ .file = null };
    var book_mark: u8 = 0;
    const path: ?[*:0]const u8 = "/x.epub";
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_open_streamed_fs(null, path, &io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_open_streamed_fs(@ptrCast(&book_mark), null, &io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_open_streamed_fs(@ptrCast(&book_mark), path, null, @ptrCast(&book_mark)));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_open_streamed_fs(@ptrCast(&book_mark), path, &io, null));
    try std.testing.expectEqual(@as(c_int, 0), fs_open_calls);
}

test "open_streamed_fs: open error passes through" {
    reset();
    var io = epub_stream_fs_ctx_t{ .file = null };
    var book_mark: u8 = 0;
    fs_open_err = 0x205;
    try std.testing.expectEqual(@as(ra8_err_t, 0x205), epub_open_streamed_fs(@ptrCast(&book_mark), "/x.epub", &io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(@as(c_int, 1), fs_open_calls);
    try std.testing.expect(io.file == null);
}

test "open_streamed_fs: happy path wires the media and keeps the file open" {
    reset();
    var io = epub_stream_fs_ctx_t{ .file = null };
    var book_mark: u8 = 0;
    fs_size_value = 100;
    try std.testing.expectEqual(k_ra8_ok, epub_open_streamed_fs(@ptrCast(&book_mark), "/x.epub", &io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(@as(u64, 100), fs_streamed_size_seen);
    // The media ctx is the io struct the caller owns.
    try std.testing.expectEqual(@as(?*anyopaque, @ptrCast(&io)), fs_streamed_ctx_seen);
    // The file handle stays open for the book's lifetime.
    try std.testing.expectEqual(@as(?*ra8_fs_file_t, @ptrCast(fs_stub_file_ptr)), io.file);
    // The media read callback is the bridge's own.
    const read_fn: epub_stream_read_fn = @ptrCast(@alignCast(fs_streamed_read_seen));
    var out: c_uint = 0;
    fs_read_got = 4;
    try std.testing.expectEqual(k_ra8_ok, read_fn(fs_streamed_ctx_seen, 10, @as(?*anyopaque, @ptrCast(&out)), 4, &out));
    try std.testing.expectEqual(@as(c_uint, 4), out);
}

test "open_streamed_fs: over-4-GiB size clamps to the reject path" {
    reset();
    var io = epub_stream_fs_ctx_t{ .file = null };
    var book_mark: u8 = 0;
    fs_size_value = 0x1_0000_0000;
    try std.testing.expectEqual(k_ra8_ok, epub_open_streamed_fs(@ptrCast(&book_mark), "/x.epub", &io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(@as(u64, 0), fs_streamed_size_seen);
}

test "open_streamed_fs: streamed-open failure closes the file and clears io" {
    reset();
    var io = epub_stream_fs_ctx_t{ .file = null };
    var book_mark: u8 = 0;
    fs_streamed_err = 0x103;
    try std.testing.expectEqual(@as(ra8_err_t, 0x103), epub_open_streamed_fs(@ptrCast(&book_mark), "/x.epub", &io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(@as(c_int, 1), fs_close_calls);
    try std.testing.expect(io.file == null);
}

test "close_streamed_fs: null guards and err passthrough with the file closed" {
    reset();
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_close_streamed_fs(null, @ptrCast(&fs_stub_book_ptr)));
    try std.testing.expectEqual(k_ra8_err_null_ptr, epub_close_streamed_fs(@ptrCast(&fs_stub_book_ptr), null));

    var file_mark: u8 = 0;
    var io = epub_stream_fs_ctx_t{ .file = @ptrCast(&file_mark) };
    var book_mark: u8 = 0;
    fs_close_err = 0x999;
    try std.testing.expectEqual(@as(ra8_err_t, 0x999), epub_close_streamed_fs(&io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(@as(c_int, 1), epub_close_calls);
    try std.testing.expectEqual(@as(c_int, 1), fs_close_calls);
    try std.testing.expect(io.file == null);

    // Closing twice is idempotent on the fs handle: the second call sees no file.
    fs_close_err = 0;
    fs_close_calls = 0;
    try std.testing.expectEqual(k_ra8_ok, epub_close_streamed_fs(&io, @ptrCast(&book_mark)));
    try std.testing.expectEqual(@as(c_int, 2), epub_close_calls);
    try std.testing.expectEqual(@as(c_int, 0), fs_close_calls);
}
