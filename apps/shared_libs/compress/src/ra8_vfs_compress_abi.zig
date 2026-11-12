//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Transparent whole-file compress-on-write / decompress-on-read over the
//! ra8_io VFS, ported from src/ra8_vfs_compress.c (RA8FW-1195) behind the
//! unchanged C headers in inc/.
//!
//! The layer composes the buffer-to-buffer codec (ra8_compress /
//! ra8_decompress, Zig since RA8FW-1155) with the VFS file ops
//! (ra8_io_vfs_open + ra8_fs_write / ra8_fs_read + ra8_fs_close), which stay
//! C ABI: `extern` decls resolve to whatever library the image links (the
//! ra8_io fabric). Every buffer is caller-provided, so the allocator is never
//! touched. The test step links teststub/vfs_compress_test_stubs.c, stand-ins
//! for the VFS seam (a RAM-backed file) and a reversible codec stub; the real
//! codec has its own package tests (RA8FW-1155).

const std = @import("std");

const ra8_err_t = u16;
const k_ra8_ok: ra8_err_t = 0;
const k_ra8_err_invalid_size: ra8_err_t = 0x105;
const k_ra8_err_null_ptr: ra8_err_t = 0x504;

/// ra8_fs_types.h: `k_ra8_fs_mode_read = 0`, `k_ra8_fs_mode_write = 1`.
const k_ra8_fs_mode_read: c_int = 0;
const k_ra8_fs_mode_write: c_int = 1;

const ra8_fs_file_t = opaque {};

extern fn ra8_io_vfs_open(path: ?[*:0]const u8, mode: c_int, out_file: ?*?*ra8_fs_file_t) ra8_err_t;
extern fn ra8_fs_close(file: ?*ra8_fs_file_t) ra8_err_t;
extern fn ra8_fs_read(file: ?*ra8_fs_file_t, buf: ?[*]u8, max_len: c_uint, got_len: ?*c_uint) ra8_err_t;
extern fn ra8_fs_write(file: ?*ra8_fs_file_t, buf: ?[*]const u8, len: c_uint) ra8_err_t;
extern fn ra8_compress(src: ?[*]const u8, src_len: u32, out: ?[*]u8, out_cap: u32, scratch: ?*anyopaque, scratch_len: u32, out_len: ?*u32) ra8_err_t;
extern fn ra8_decompress(src: ?[*]const u8, src_len: u32, out: ?[*]u8, out_cap: u32, out_len: ?*u32) ra8_err_t;

/// ra8_vfs_compress.h: `[[nodiscard]] ra8_err_t ra8_vfs_compress_write(...)`.
/// Validate, compress into the caller blob, stream the blob to the file.
pub export fn ra8_vfs_compress_write(
    path: ?[*:0]const u8,
    src: ?[*]const u8,
    src_len: u32,
    scratch: ?*anyopaque,
    scratch_len: u32,
    blob_buf: ?[*]u8,
    blob_cap: u32,
    out_blob_len: ?*u32,
) callconv(.c) ra8_err_t {
    if (path == null) return k_ra8_err_null_ptr;
    if (src == null) return k_ra8_err_null_ptr;
    if (scratch == null) return k_ra8_err_null_ptr;
    if (blob_buf == null) return k_ra8_err_null_ptr;
    if (out_blob_len == null) return k_ra8_err_null_ptr;

    var blob_len: u32 = 0;
    const cerr = ra8_compress(src, src_len, blob_buf, blob_cap, scratch, scratch_len, &blob_len);
    if (cerr != k_ra8_ok) {
        return cerr;
    }
    const werr = store_blob(path.?, blob_buf.?, blob_len);
    if (werr != k_ra8_ok) {
        return werr;
    }
    out_blob_len.?.* = blob_len;
    return k_ra8_ok;
}

/// ra8_vfs_compress.h: `[[nodiscard]] ra8_err_t ra8_vfs_compress_read(...)`.
/// Pull the whole blob off the file, inflate it into the caller output.
pub export fn ra8_vfs_compress_read(
    path: ?[*:0]const u8,
    blob_buf: ?[*]u8,
    blob_cap: u32,
    out: ?[*]u8,
    out_cap: u32,
    out_len: ?*u32,
) callconv(.c) ra8_err_t {
    if (path == null) return k_ra8_err_null_ptr;
    if (blob_buf == null) return k_ra8_err_null_ptr;
    if (out == null) return k_ra8_err_null_ptr;
    if (out_len == null) return k_ra8_err_null_ptr;

    var blob_len: u32 = 0;
    const lerr = load_blob(path.?, blob_buf.?, blob_cap, &blob_len);
    if (lerr != k_ra8_ok) {
        return lerr;
    }
    return ra8_decompress(blob_buf.?, blob_len, out, out_cap, out_len);
}

/// Open `path` for write, stream the whole blob, close. The write and close
/// error codes are captured separately so the file is always closed even when
/// the write fails; the write error takes precedence over the close error.
fn store_blob(path: [*:0]const u8, blob: [*]const u8, blob_len: u32) ra8_err_t {
    var f: ?*ra8_fs_file_t = null;
    const oerr = ra8_io_vfs_open(path, k_ra8_fs_mode_write, &f);
    if (oerr != k_ra8_ok) {
        return oerr;
    }
    const werr = ra8_fs_write(f, blob, blob_len);
    const cerr = ra8_fs_close(f);
    if (werr != k_ra8_ok) {
        return werr;
    }
    return cerr;
}

/// Open `path` for read, copy up to `blob_cap` bytes, close. Same
/// close-always discipline. Because ra8_fs_read caps the copy at blob_cap, a
/// blob that fills the buffer exactly cannot be told from a truncated larger
/// file, so a full-buffer read is rejected as invalid_size.
fn load_blob(path: [*:0]const u8, blob_buf: [*]u8, blob_cap: u32, out_blob_len: *u32) ra8_err_t {
    var f: ?*ra8_fs_file_t = null;
    const oerr = ra8_io_vfs_open(path, k_ra8_fs_mode_read, &f);
    if (oerr != k_ra8_ok) {
        return oerr;
    }
    var blob_len: c_uint = 0;
    const rerr = ra8_fs_read(f, blob_buf, blob_cap, &blob_len);
    const cerr = ra8_fs_close(f);
    if (rerr != k_ra8_ok) {
        return rerr;
    }
    if (cerr != k_ra8_ok) {
        return cerr;
    }
    if (blob_len >= blob_cap) {
        return k_ra8_err_invalid_size;
    }
    out_blob_len.* = blob_len;
    return k_ra8_ok;
}

test "write rejects null pointers" {
    var blob: [16]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var out_len: u32 = 0;
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_write(null, &blob, 1, &scratch, 8, &blob, 16, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_write("ram:/a", null, 1, &scratch, 8, &blob, 16, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_write("ram:/a", &blob, 1, null, 8, &blob, 16, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_write("ram:/a", &blob, 1, &scratch, 8, null, 16, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_write("ram:/a", &blob, 1, &scratch, 8, &blob, 16, null));
}

test "read rejects null pointers" {
    var blob: [16]u8 = undefined;
    var out: [16]u8 = undefined;
    var out_len: u32 = 0;
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_read(null, &blob, 16, &out, 16, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_read("ram:/a", null, 16, &out, 16, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_read("ram:/a", &blob, 16, null, 16, &out_len));
    try std.testing.expectEqual(k_ra8_err_null_ptr, ra8_vfs_compress_read("ram:/a", &blob, 16, &out, 16, null));
}

test "round trip through the stubbed VFS" {
    var payload: [24]u8 = undefined;
    for (&payload, 0..) |*b, i| b.* = @intCast(i * 7 + 1);
    var blob: [64]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var blob_len: u32 = 0;
    var restored: [32]u8 = undefined;
    var out_len: u32 = 0;

    try std.testing.expectEqual(k_ra8_ok, ra8_vfs_compress_write("ram:/STORY.RBK", &payload, payload.len, &scratch, scratch.len, &blob, blob.len, &blob_len));
    try std.testing.expectEqual(@as(u32, payload.len + 4), blob_len);
    try std.testing.expectEqual(k_ra8_ok, ra8_vfs_compress_read("ram:/STORY.RBK", &blob, blob.len, &restored, restored.len, &out_len));
    try std.testing.expectEqual(payload.len, out_len);
    try std.testing.expectEqualSlices(u8, &payload, restored[0..out_len]);
}

test "blob filling blob_cap exactly is refused as invalid_size" {
    var payload: [16]u8 = undefined;
    @memset(&payload, 0x42);
    var blob: [24]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var blob_len: u32 = 0;
    var out: [16]u8 = undefined;
    var out_len: u32 = 0;
    // The stub codec writes 20 bytes for this payload; a read whose blob_cap
    // equals the on-disk size hits the truncation refusal.
    try std.testing.expectEqual(k_ra8_ok, ra8_vfs_compress_write("ram:/FULL", &payload, payload.len, &scratch, scratch.len, &blob, blob.len, &blob_len));
    try std.testing.expectEqual(@as(u32, 20), blob_len);
    try std.testing.expectEqual(k_ra8_err_invalid_size, ra8_vfs_compress_read("ram:/FULL", &blob, blob_len, &out, out.len, &out_len));
}

test "missing mount propagates not_found" {
    var payload: [4]u8 = .{ 1, 2, 3, 4 };
    var blob: [32]u8 = undefined;
    var scratch: [8]u8 = undefined;
    var blob_len: u32 = 0;
    var out: [32]u8 = undefined;
    var out_len: u32 = 0;
    // The stub VFS refuses "absent:/..." with k_ra8_err_not_found (0x106).
    try std.testing.expectEqual(@as(ra8_err_t, 0x106), ra8_vfs_compress_write("absent:/X", &payload, payload.len, &scratch, scratch.len, &blob, blob.len, &blob_len));
    try std.testing.expectEqual(@as(ra8_err_t, 0x106), ra8_vfs_compress_read("absent:/X", &blob, blob.len, &out, out.len, &out_len));
}
