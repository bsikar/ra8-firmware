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
//! The epub core (epub_open_abi.zig and epub_chapter_abi.zig) is Zig and is
//! entered through epub_open_streamed()/epub_close(); ra8_fs_* resolves to
//! whatever library the image links (the Zig ra8_fs archive or its C build).
//! The test step links teststub/fs_bridge_test_stubs.c, stand-ins for the
//! ra8_fs seam and the two epub core entry points the bridge calls.

const std = @import("std");

pub const ra8_err_t = u16;
pub const k_ra8_ok: ra8_err_t = 0;
pub const k_ra8_err_null_ptr: ra8_err_t = 0x504;
pub const k_ra8_err_out_of_range: ra8_err_t = 0x208;
/// ra8_fs_types.h: `k_ra8_fs_mode_read = 0`.
const k_ra8_fs_mode_read: c_int = 0;

pub const ra8_fs_file_t = opaque {};
const ra8_fs_mount_t = opaque {};
const epub_book_t = opaque {};

/// epub_fs.h: `typedef struct { ra8_fs_file_t* file; } epub_stream_fs_ctx_t;`
pub const epub_stream_fs_ctx_t = extern struct { file: ?*ra8_fs_file_t };

/// epub.h: `typedef ra8_err_t (*epub_stream_read_fn)(void* ctx, uint64_t offset,
/// void* buf, uint32_t len, uint32_t* out_read);`
pub const epub_stream_read_fn =
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
