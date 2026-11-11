//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_fs` -> `epub` bridge (Phase 4.2 file adapter), ported from
//! src/epub_fs.c (RA8FW-1161) behind the unchanged C headers in inc/.
//!
//! The bridge lives in its own Zig library (ra8_epub_fs, built here) rather
//! than inside the epub guard library: the C TU was empty unless
//! __has_include("ra8_fs.h") held, so the pure epub core could link into
//! apps and hosts that never pull in ra8_fs. A separate Zig artifact keeps
//! that property at archive granularity -- nothing references the bridge's
//! three exports unless it opens a streamed book off ra8_fs, so the linker
//! never pulls this archive (or its ra8_fs externs) for a core-only consumer.
//! The epub core (epub_open.c, epub_entry.c, epub_chapter.c) stays C and is
//! entered through epub_open_streamed()/epub_close(); ra8_fs_* resolves to
//! whatever library the image links (the Zig ra8_fs archive or its C build).
//! The test step links teststub/fs_bridge_test_stubs.c, stand-ins for the
//! ra8_fs seam and the two epub core entry points the bridge calls.

const std = @import("std");

const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;
const k_ra8_err_out_of_range: ra8_err_t = 0x208;
/// ra8_fs_types.h: `k_ra8_fs_mode_read = 0`.
const k_ra8_fs_mode_read: c_int = 0;

const ra8_fs_file_t = opaque {};
const ra8_fs_mount_t = opaque {};
const epub_book_t = opaque {};

/// epub_fs.h: `typedef struct { ra8_fs_file_t* file; } epub_stream_fs_ctx_t;`
const epub_stream_fs_ctx_t = extern struct { file: ?*ra8_fs_file_t };

/// epub.h: `typedef ra8_err_t (*epub_stream_read_fn)(void* ctx, uint64_t offset,
/// void* buf, uint32_t len, uint32_t* out_read);`
const epub_stream_read_fn =
    *const fn (?*anyopaque, u64, ?*anyopaque, c_uint, ?*c_uint) callconv(.c) ra8_err_t;

/// epub.h: `typedef struct { epub_stream_read_fn read; void* ctx;
/// uint64_t size; } epub_stream_media_t;`
const epub_stream_media_t = extern struct {
    read: epub_stream_read_fn,
    ctx: ?*anyopaque,
    size: u64,
};

extern fn ra8_fs_open(mount: ?*ra8_fs_mount_t, path: ?[*:0]const u8, mode: c_int, out_file: ?*?*ra8_fs_file_t) ra8_err_t;
extern fn ra8_fs_close(file: ?*ra8_fs_file_t) ra8_err_t;
extern fn ra8_fs_seek(file: ?*ra8_fs_file_t, offset_bytes: u64) ra8_err_t;
extern fn ra8_fs_read(file: ?*ra8_fs_file_t, buf: ?[*]u8, max_len: c_uint, got_len: ?*c_uint) ra8_err_t;
extern fn ra8_fs_size(file: ?*const ra8_fs_file_t, out_bytes: ?*u64) ra8_err_t;
extern fn epub_open_streamed(media: ?*epub_stream_media_t, path: ?[*:0]const u8, out_book: ?*epub_book_t) ra8_err_t;
extern fn epub_close(book: ?*epub_book_t) ra8_err_t;

/// epub_fs_internal.h: `priv_epub_fs_stream_read` -- the media read callback
/// the bridge hands to `epub_open_streamed()`. Seek+read on demand; the file
/// stays open for the book's lifetime.
pub export fn priv_epub_fs_stream_read(ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: c_uint, out_read: ?*c_uint) callconv(.c) ra8_err_t {
    if (out_read == null) {
        return k_ra8_err_null_ptr;
    }
    out_read.?.* = 0;
    const io: ?*epub_stream_fs_ctx_t = @ptrCast(@alignCast(ctx));
    if (io == null or io.?.file == null or buf == null) {
        return k_ra8_err_null_ptr;
    }
    if (offset > std.math.maxInt(u32)) {
        return k_ra8_err_out_of_range; // a >4 GiB .epub is rejected before here
    }
    const serr = ra8_fs_seek(io.?.file, offset);
    if (serr != k_ra8_ok) {
        return serr; // seek clamps to size and cannot fail on a live handle
    }
    var got: c_uint = 0;
    const rerr = ra8_fs_read(io.?.file, @ptrCast(buf), len, &got);
    if (rerr != k_ra8_ok) {
        // The card said no. That is not the end of the book, and the reader is
        // now told so rather than being handed a short count it would read as EOF.
        return rerr;
    }
    out_read.?.* = got;
    return k_ra8_ok;
}

pub export fn epub_open_streamed_fs(mount: ?*ra8_fs_mount_t, path: ?[*:0]const u8, io: ?*epub_stream_fs_ctx_t, out_book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if (mount == null or path == null or io == null or out_book == null) {
        return k_ra8_err_null_ptr;
    }
    io.?.file = null;

    var file: ?*ra8_fs_file_t = null;
    var err = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
    if (err != k_ra8_ok) {
        return err;
    }

    // Size the archive for miniz. ra8_fs_size cannot fail on the handle
    // ra8_fs_open just returned (in_use set, both out-params non-NULL); on any
    // future contract change `size` stays 0 and epub_open_streamed rejects it
    // as invalid_arg, so no separate (untestable) error branch is needed here.
    var size64: u64 = 0;
    _ = ra8_fs_size(file, &size64);
    // The EPUB pipeline sizes archives in 32 bits; an over-4-GiB file is not a
    // plausible EPUB and is clamped to the reject path (0 -> invalid_arg).
    const size: u32 = if (size64 <= std.math.maxInt(u32)) @intCast(size64) else 0;

    io.?.file = file;
    var media = epub_stream_media_t{
        .read = priv_epub_fs_stream_read,
        .ctx = io,
        .size = size,
    };
    err = epub_open_streamed(&media, path, out_book);
    if (err != k_ra8_ok) {
        _ = ra8_fs_close(file);
        io.?.file = null;
        return err;
    }
    return k_ra8_ok;
}

pub export fn epub_close_streamed_fs(io: ?*epub_stream_fs_ctx_t, book: ?*epub_book_t) callconv(.c) ra8_err_t {
    if (io == null or book == null) {
        return k_ra8_err_null_ptr;
    }
    const err = epub_close(book);
    if (io.?.file != null) {
        _ = ra8_fs_close(io.?.file);
        io.?.file = null;
    }
    return err;
}

// ---------------------------------------------------------------------------
// Host tests against the teststub stand-ins.
// ---------------------------------------------------------------------------

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
